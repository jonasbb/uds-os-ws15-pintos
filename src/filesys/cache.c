#include "filesys/cache.h"
#include <list.h>
#include <stdio.h>
#include <string.h>
#include <round.h>
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "vm/frames.h"

// TODO DEBUG
const bool print_hex = false;
const bool print_debug = false;
const bool print_cache_state = false;

// static functions
static cache_t get_and_lock_sector_data(block_sector_t sector);
static void set_accessed (cache_t idx,
                          bool    accessed);
static void set_dirty (cache_t idx,
                       bool    dirty);
static void set_pin (cache_t idx,
                     bool    pin);
static void set_unready (cache_t idx,
                         bool    unready);
static void pin (cache_t idx);
static void *idx_to_ptr(cache_t idx);

/***********************************************************
 * Configuration / Data for cache
 ***********************************************************/

// Maximal number of pages
#define CACHE_SIZE ((cache_t)64)
const cache_t NOT_IN_CACHE = 0xFF;
const block_sector_t NO_SECTOR = 0xFFFFFFFF;

enum cache_state {
    ACCESSED = 1<<0,
    DIRTY = 1<<1,
    PIN = 1<<2, // bitte bitte lieber evict algorithm, lass meinen Block im Cache
    UNREADY = 1<<3 // Eintrag wird mal Daten für sector enthalten, aber nocht nicht jetzt, warte auf condition und recheck
};
typedef uint8_t cache_state_t;

struct cache_entry {
    volatile block_sector_t sector;
    uint16_t refs;
    cache_state_t state;
    struct lock lock;
    struct condition cond;
};

// lock for datastructures
struct lock cache_lock;
struct lock block_meta_lock;

// array of actual blocks
void *blocks[CACHE_SIZE];
// array of metadata for cache entries
struct cache_entry *blocks_meta;
// next block to check for eviction
volatile cache_t evict_ptr;

/***********************************************************
 * Configuration / Data for cache END
 ***********************************************************/

/***********************************************************
 * scheduler
 ***********************************************************/
// function declaraions
static
bool request_item_less_func (const struct list_elem *a_,
                             const struct list_elem *b_,
                             void *aux);
static
cache_t sched_contains_req(block_sector_t sector,
                                        bool           read);
static
void sched_init(void);
static
void sched_background(void *aux UNUSED);
static
cache_t sched_read(block_sector_t sector);
static
cache_t sched_read_do(block_sector_t sector,
                      bool           isprefetch);
static
void sched_write(block_sector_t sector,
                 cache_t        idx);
static
cache_t sched_insert(block_sector_t sector,
                     cache_t        cache_idx);

struct lock sched_lock;
struct list sched_outstanding_requests;
struct condition sched_new_requests_cond;
struct request_item {
    struct list_elem elem;
    block_sector_t sector;
    cache_t idx;
    bool read;
};

static
bool request_item_less_func (const struct list_elem *a_,
                             const struct list_elem *b_,
                             void *aux UNUSED) {
    struct request_item *a = list_entry(a_, struct request_item, elem);
    struct request_item *b = list_entry(b_, struct request_item, elem);
    return a->sector < b->sector;
}

cache_t sched_contains_req(block_sector_t sector,
                           bool           read) {
    lock_acquire_re(&sched_lock);
    cache_t res = NOT_IN_CACHE;
    struct list_elem *e;
    for (e = list_begin (&sched_outstanding_requests);
         e != list_end (&sched_outstanding_requests);
         e = list_next (e)) {
        struct request_item *r = list_entry (e, struct request_item, elem);
        if (r->sector == sector && r->read == read) {
            res = r->idx;
            break;
        }
    }
    lock_release_re(&sched_lock);
    return res;
}

static
void sched_init() {
    // init data structures
    lock_init(&sched_lock);
    list_init(&sched_outstanding_requests);
    cond_init(&sched_new_requests_cond);

    // start background thread for reading/writing blocks
    /*thread_create("BLCK_WRTR",
                  thread_current()->priority,
                  &sched_background,
                  NULL);*/
}

static
void sched_background(void *aux UNUSED) {
  lock_acquire_re(&block_meta_lock);
    lock_acquire_re(&sched_lock);
  //  while(true) {
        struct list_elem *e = NULL;
        struct request_item *r = NULL;

        for (e = list_begin(&sched_outstanding_requests);
             e != list_end(&sched_outstanding_requests);
             ) {
            r = list_entry (e, struct request_item, elem);
            e = list_next(e);
            list_remove(&r->elem);
            int cnt = lock_release_re_mult(&sched_lock);
            // perform block operation
            if (r->read) {
                log_debug(":S: BLCK_WRTR is reading... :S:\n");
                block_read(fs_device,
                           r->sector,
                           idx_to_ptr(r->idx));
                lock_acquire_re(&block_meta_lock);
                // now ready as data is loaded and inform interrested parties
                set_unready(r->idx, false);
                // mark cache as reusable again
                unpin(r->idx);
                cond_broadcast(&blocks_meta[r->idx].cond, &block_meta_lock);
                lock_release_re(&block_meta_lock);
                log_debug(":S: BLCK_WRTR finisched reading. :S:\n");
            } else {
                log_debug(":S: BLCK_WRTR is writing... :S:\n");
                lock_acquire_re(&block_meta_lock);
                block_write(fs_device,
                            r->sector,
                            idx_to_ptr(r->idx));
                set_dirty(r->idx, false);
                // mark cache as reusable again
                unpin(r->idx);
                lock_release_re(&block_meta_lock);
                log_debug(":S: BLCK_WRTR finisched writing. :S:\n");
            }

            lock_acquire_re_mult(&sched_lock, cnt);

            free(r);
            r = NULL;
        }
        lock_release_re(&sched_lock);
        lock_release_re(&block_meta_lock);
        return; /*
        if (!list_empty(&sched_outstanding_requests)) {
            continue;
        }

        log_debug(":S: BLCK_WRTR is going to sleep... :S:\n");
        // wait until there is something to do
        cond_wait(&sched_new_requests_cond, &sched_lock);
        log_debug(":S: BLCK_WRTR was woken up :S:\n");
    //} */
}

/* Increases reference count on new block */
static
cache_t sched_read(block_sector_t sector) {
    ASSERT(sector < block_size(fs_device));

    lock_acquire_re(&sched_lock);
    cache_t res;
    res = sched_read_do(sector, false);
    //lock_acquire_re(&blocks_meta[res].lock);
    //blocks_meta[res].refs += 1;
    //lock_release_re(&blocks_meta[res].lock);
    lock_release_re(&sched_lock);
    return res;
}

static
cache_t sched_read_do(block_sector_t sector,
                      bool           isprefetch) {
    lock_acquire_re(&sched_lock);
    cache_t res;
    if ((res = sched_contains_req(sector, true)) == NOT_IN_CACHE) {
        res = sched_insert(sector, NOT_IN_CACHE);
    }
    // TODO implement as extra thread
    /*if (!isprefetch && sched_contains_req(sector+1, true) == NOT_IN_CACHE
            && sector < block_size(fs_device)) // check for out of bound accesses
            {
        sched_insert(sector+1, NOT_IN_CACHE);
    }*/
    lock_release_re(&sched_lock);
    return res;
}

static
void sched_write(block_sector_t sector,
                 cache_t        idx) {
    ASSERT(sector < block_size(fs_device));

    lock_acquire_re(&sched_lock);
    if (sched_contains_req(sector, false) == NOT_IN_CACHE) {
        sched_insert(sector, idx);
    }
    lock_release_re(&sched_lock);
}

static
cache_t sched_insert(block_sector_t sector,
                     cache_t        cache_idx) {
    lock_acquire_re(&sched_lock);
    cache_t res = NOT_IN_CACHE;
                       struct request_item *r = malloc(sizeof(*r));
    ASSERT(r != NULL);
    r->sector = sector;
    r->read = cache_idx == NOT_IN_CACHE;
    if (cache_idx == NOT_IN_CACHE) {
        r->idx = get_and_pin_block(sector);
    } else {
        r->idx = cache_idx;
    }
    res = r->idx;

    // add to queue
    list_insert_ordered(&sched_outstanding_requests,
                        &r->elem,
                        request_item_less_func,
                        NULL);
    sched_background(NULL);
    log_debug(":S: Broadcast to thread :S:\n");
    cond_broadcast(&sched_new_requests_cond, &sched_lock);
    lock_release_re(&sched_lock);
    return res;
}
/***********************************************************
 * scheduler END
 ***********************************************************/

void cache_init() {
    sched_init();
    lock_init(&cache_lock);

    // init state
    evict_ptr = 0;

    // reserve memory for actual blocks
    size_t numpages = DIV_ROUND_UP(CACHE_SIZE * BLOCK_SECTOR_SIZE, PGSIZE);
    int i;
    for (i = 0; i < CACHE_SIZE; i+=8) {
        void *page = frame_get_free();
        ASSERT(page != NULL);
        blocks[i+0] = page + 0 * BLOCK_SECTOR_SIZE;
        blocks[i+1] = page + 1 * BLOCK_SECTOR_SIZE;
        blocks[i+2] = page + 2 * BLOCK_SECTOR_SIZE;
        blocks[i+3] = page + 3 * BLOCK_SECTOR_SIZE;
        blocks[i+4] = page + 4 * BLOCK_SECTOR_SIZE;
        blocks[i+5] = page + 5 * BLOCK_SECTOR_SIZE;
        blocks[i+6] = page + 6 * BLOCK_SECTOR_SIZE;
        blocks[i+7] = page + 7 * BLOCK_SECTOR_SIZE;
    }

    // reserve metadata memory
    numpages = DIV_ROUND_UP(CACHE_SIZE * sizeof(struct cache_entry), PGSIZE);
    ASSERT(numpages == 1);
    blocks_meta = frame_get_free();
    ASSERT(blocks_meta != NULL);

    for (i = 0; i < CACHE_SIZE; i++) {
        blocks_meta[i].sector = NO_SECTOR;
        blocks_meta[i].state = 0;
        blocks_meta[i].refs = 0;

        cond_init(&blocks_meta[i].cond);
    }
    lock_init(&block_meta_lock);
}

/*
 * Returns an empty space in the buffer. The space in buffer is pinned until
 * the pin is removed manually.
 * Load a block into cache and return position in cache.
 */
/* Evict a block. Performs clock algorithm until suitable space is found.
 * Return cache index.
 * Returns NOT_IN_CACHE on failure.
 */
cache_t get_and_pin_block (block_sector_t sector) {
    // sector is used to relabel the cache entry for new usage
    cache_t ptr;
    int cnt = 0;

    while(true) {
        ptr = evict_ptr;
        cnt++;
        if (ptr == 0) {
            if (cnt == CACHE_SIZE) {
                // be nice to the others
                // apparently there is nothing to do for you right now
                thread_yield();
            }
            cnt = 0;
        }
        // increment
        evict_ptr = (evict_ptr + 1) % CACHE_SIZE;

        if (lock_try_acquire_re(&block_meta_lock)) {
            if ((blocks_meta[ptr].state & PIN) != 0
                    || blocks_meta[ptr].refs > 0) {
                if (print_cache_state) {
                    log_debug("=|= %d is PINNED (%d), refs %d =|=\n", ptr, blocks_meta[ptr].state & PIN, blocks_meta[ptr].refs);
                }
                // pinned page, may not do anything about it
                goto cont1;
            } else if ((blocks_meta[ptr].state & DIRTY) == DIRTY) {
                if (print_cache_state) {
                    log_debug("=|= %d is scheduled for write =|=\n", ptr);
                }
                // dirty, shedule write

                // pin page so that we may release the lock
                pin(ptr);
                lock_release_re(&block_meta_lock);
                sched_write(blocks_meta[ptr].sector, ptr);
                goto cont2;
            } else if ((blocks_meta[ptr].state & DIRTY) == 0
                       && (blocks_meta[ptr].state & ACCESSED) == ACCESSED) {
                if (print_cache_state) {
                    log_debug("=|= %d was accessed =|=\n", ptr);
                }
                // was access, give chance again
                set_accessed(ptr, false);
                goto cont1;
            } else if ((blocks_meta[ptr].state & DIRTY) == 0
                       && (blocks_meta[ptr].state & ACCESSED) == 0) {
                if (print_cache_state) {
                    log_debug("=|= %d is now evicted =|=\n", ptr);
                }
                // not accessed since last time, may be overwritten
                // mark this entry as to be used by new sector
                blocks_meta[ptr].sector = sector;
                pin(ptr);
                set_unready(ptr, true);
                goto done;
            }

            // the above case should handle all different cache states
            NOT_REACHED();
cont1:
            lock_release_re(&block_meta_lock);
cont2:
            continue;
        } else {
            if (print_cache_state) {
                log_debug("=|= %d is locked =|=\n", ptr);
            }
        }
    }
done:
    lock_release_re(&block_meta_lock);
    ASSERT(ptr < CACHE_SIZE);
    return ptr;
}

/* Set a whole block to only zeros */
void zero_out_sector_data(block_sector_t sector) {
    lock_acquire_re(&block_meta_lock);
    lock_acquire(&cache_lock);
    cache_t idx = get_and_pin_block(sector);
    lock_release(&cache_lock);

    lock_acquire_re(&block_meta_lock);
    memset(idx_to_ptr(idx), 0, BLOCK_SECTOR_SIZE);
    unpin(idx);
    set_unready(idx, false);
    lock_release_re(&block_meta_lock);
    lock_release_re(&block_meta_lock);
}

static
cache_t get_and_lock_sector_data(block_sector_t sector) {
    ASSERT(sector < block_size(fs_device));
    // return locked block with data from sector
    // if not already in cache load into cache

    cache_t res = NOT_IN_CACHE;

    // search for existing position

    // assures no other insertions are possible w/o our knowledge
    lock_acquire_re(&block_meta_lock);
    lock_acquire(&cache_lock);

    cache_t i;
    for (i = 0; i < CACHE_SIZE; i++) {
        if (blocks_meta[i].sector == sector) {
            lock_acquire_re(&block_meta_lock);
            // re-check
            if (blocks_meta[i].sector != sector) {
                // somehow sector changed, no other
                // concurrent thread will have requested a cache position for this
                // sector so this is up to us
                lock_release_re(&block_meta_lock);
                break;
            } else if ((blocks_meta[i].state & UNREADY) != 0) {
                // count how many threads are interested in this block
                blocks_meta[i].refs += 1;

                lock_release(&cache_lock);
                // wait until data is in cache
                cond_wait(&blocks_meta[i].cond, &block_meta_lock);

                blocks_meta[i].refs -= 1;
                res = i;
                goto entry_found;
            }
            res = i;
            lock_release(&cache_lock);

            goto entry_found;
        }
    }


    // schedule read
    res = sched_read(sector);
    // reference count is increase for our thread
    // this entry will be valid until ref is 0 again
    lock_release(&cache_lock);
    lock_acquire_re(&block_meta_lock);
    /*if ((blocks_meta[res].state & UNREADY) != 0) {
        // wait until data is in cache
        cond_wait(&blocks_meta[res].cond, &block_meta_lock);
    }
    //blocks_meta[i].refs -= 1;*/

entry_found:
    // sector is the correct one and data is available (due to cond)
    // and metadata lock is held
    ASSERT(res < CACHE_SIZE);
    lock_release_re(&block_meta_lock);
    return res;
}

/*
 * Loads `sector` into cache if not already present and write `length` bytes
 * from `data` to `ofs` within the block.
 *
 * `length` == 0 calls are nops.
 *
 * ofs + length MUST be smaller than BLOCK_SECTOR_SIZE.
 */
void in_cache_and_overwrite_block(block_sector_t  sector,
                              size_t          ofs,
                              void           *data,
                              size_t          length) {
    lock_acquire_re(&block_meta_lock);
    if (!(ofs + length <= BLOCK_SECTOR_SIZE)) {
        printf("ofs %d, length %d, BLOCK_SECTOR_SIZE %d\n", ofs, length, BLOCK_SECTOR_SIZE);
    }
    ASSERT(ofs + length <= BLOCK_SECTOR_SIZE);
    if (!(sector < block_size(fs_device))) {
        printf("sector %d, block_size %d\n", sector, block_size(fs_device));
    }
    ASSERT(sector < block_size(fs_device));

    if (print_debug) {
        log_debug("Write to sector %d\n    ofs: %d - length: %d\n", sector, ofs, length);
    }
    if (print_hex) {
        hex_dump(ofs, data, length, false);
        printf("\n");
    }

    if (length == 0) {
        return;
    }

    // get block pos
    cache_t ind = get_and_lock_sector_data(sector);
    // write data
    // to, from, length
    memcpy(idx_to_ptr(ind)+ofs, data, length);
    set_dirty(ind, true);
    set_accessed(ind, true);
    lock_release_re(&block_meta_lock);
    if (print_hex) {
        hex_dump(ofs, idx_to_ptr(ind), length, false);
        printf("\n");
    }
    lock_release_re(&block_meta_lock);
}

/* analoge in_cache_and_overwrite_block but read */;
void in_cache_and_read(block_sector_t  sector,
                       size_t          ofs,
                       void           *data,
                       size_t          length) {
    lock_acquire_re(&block_meta_lock);
    if (!(ofs + length <= BLOCK_SECTOR_SIZE)) {
        printf("ofs %d, length %d, BLOCK_SECTOR_SIZE %d\n", ofs, length, BLOCK_SECTOR_SIZE);
    }
    ASSERT(ofs + length <= BLOCK_SECTOR_SIZE);
    if (!(sector < block_size(fs_device))) {
        printf("sector %d, block_size %d\n", sector, block_size(fs_device));
    }
    ASSERT(sector < block_size(fs_device));
    if (print_debug) {
        log_debug("Read from sector %d\n    ofs: %d - length: %d\n", sector, ofs, length);
    }

    if (length == 0) {
        return;
    }

    // get block pos
    cache_t ind = get_and_lock_sector_data(sector);
    // read data
    // to, from, length
    memcpy(data, idx_to_ptr(ind)+ofs, length);
    set_accessed(ind, true);
    lock_release_re(&block_meta_lock);
    if (print_hex) {
        hex_dump(ofs, data, length, false);
        printf("\n");
    }
    lock_release_re(&block_meta_lock);
}

/*
 * Set the accessed flag
 */
static
void set_accessed (cache_t idx, bool accessed) {
    // valid range
    ASSERT(idx < CACHE_SIZE);
    lock_acquire_re(&block_meta_lock);
    if (accessed) {
        blocks_meta[idx].state |= ACCESSED;
    } else {
        blocks_meta[idx].state &= ~ACCESSED;
    }
    lock_release_re(&block_meta_lock);
}

/*
 * Set the dirty flag
 */
static
void set_dirty (cache_t idx, bool dirty) {
    // valid range
    ASSERT(idx < CACHE_SIZE);
    lock_acquire_re(&block_meta_lock);
    if (dirty) {
        blocks_meta[idx].state |= DIRTY;
    } else {
        blocks_meta[idx].state &= ~DIRTY;
    }
    lock_release_re(&block_meta_lock);
}

/*
 * Set the unready flag
 */
static
void set_unready (cache_t idx, bool unready) {
    // valid range
    ASSERT(idx < CACHE_SIZE);
    lock_acquire_re(&block_meta_lock);
    if (unready) {
        blocks_meta[idx].state |= UNREADY;
    } else {
        blocks_meta[idx].state &= ~UNREADY;
    }
    lock_release_re(&block_meta_lock);
}

/*
 * Set the pin flag
 */
static
void set_pin (cache_t idx, bool pin) {
    // valid range
    ASSERT(idx < CACHE_SIZE);
    lock_acquire_re(&block_meta_lock);
    if (pin) {
        blocks_meta[idx].state |= PIN;
    } else {
        blocks_meta[idx].state &= ~PIN;
    }
    lock_release_re(&block_meta_lock);
}

/*
 * Set the pin flag
 */
static
void pin (cache_t idx) {
    set_pin(idx, true);
}

/*
 * Removes the pin flag
 */
void unpin (cache_t idx) {
    set_pin(idx, false);
}

static
void *idx_to_ptr(cache_t idx) {
    // valid range
    ASSERT(idx < CACHE_SIZE);
    return blocks[idx];
}
