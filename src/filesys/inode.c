#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define NON_EXISTANT 0x0

struct lock inode_list_lock;

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    /* DO NOT change start length or is_dir without change in inode */
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    bool is_dir;
    uint8_t unused2[3];
    unsigned magic;                     /* Magic number. */
    uint32_t unused[124];               /* Not used. */

  };

/* In-memory inode. */
/* DO NOT change start length or is_dir without change in inode_disk */
struct inode 
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    block_sector_t start; /* DO NOT change start length or is_dir without change in inode_disk */
    off_t length;/* DO NOT change start length or is_dir without change in inode_disk */
    bool is_dir;/* DO NOT change start length or is_dir without change in inode_disk */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */

    struct lock lock;

  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (struct inode *inode, off_t pos)
{
  log_debug("!!!byte_to_sector, inode->start: %d, pos: %d!!!\n", inode ->start, pos);
  block_sector_t sector;
  ASSERT (inode != NULL);
    in_cache_and_read(inode->start,
                    (pos/(128*BLOCK_SECTOR_SIZE))*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
  ASSERT(sector < block_size(fs_device));
    if (sector == NON_EXISTANT) return sector;
  in_cache_and_read(sector,
                    (pos%(128*BLOCK_SECTOR_SIZE))/BLOCK_SECTOR_SIZE*sizeof(block_sector_t)
                    ,
                    &sector,
                    sizeof(sector));
    ASSERT(sector < block_size(fs_device));
  return sector;
}

static block_sector_t
byte_to_sector_expand (struct inode *inode, off_t pos)
{
  log_debug("!!!byte_to_sector_expand!!!\n");
  block_sector_t tmp, sector;
  ASSERT (inode != NULL);

  in_cache_and_read(inode->start,
                    (pos/(128*BLOCK_SECTOR_SIZE))*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
  ASSERT(sector < block_size(fs_device));
  if (sector == NON_EXISTANT) {
    lock_acquire_re(&inode->lock);
    /* Revalidate still not existant */
    in_cache_and_read(inode->start,
                    (pos/(128*BLOCK_SECTOR_SIZE))*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
    /* Already added, no need anymore */
    if (sector != NON_EXISTANT) {
      lock_release_re(&inode->lock);
      goto step2;
    }
    if (!free_map_allocate(1, &sector)){
      lock_release_re(&inode->lock);
      return NON_EXISTANT;
    }

    zero_out_sector_data(sector); //TODO
    in_cache_and_overwrite_block(inode->start,
                    (pos/(128*BLOCK_SECTOR_SIZE))*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
    lock_release_re(&inode->lock);
  }

step2:
  tmp = sector;
  in_cache_and_read(tmp,
                    (pos%(128*BLOCK_SECTOR_SIZE))/BLOCK_SECTOR_SIZE*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
  if (sector == NON_EXISTANT) {
    lock_acquire_re(&inode->lock);
    /* Revalidate still not existant */
    in_cache_and_read(tmp,
                    (pos%(128*BLOCK_SECTOR_SIZE))/BLOCK_SECTOR_SIZE*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
    /* Already added, no need anymore */
    if (sector != NON_EXISTANT) {
      lock_release_re(&inode->lock);
      goto end;
    }
    if (!free_map_allocate(1,&sector)) {
      lock_release_re(&inode->lock);
      return NON_EXISTANT;
    }
    zero_out_sector_data(sector);
    in_cache_and_overwrite_block(tmp,
                    (pos%(128*BLOCK_SECTOR_SIZE))/BLOCK_SECTOR_SIZE*sizeof(block_sector_t),
                    &sector,
                    sizeof(sector));
    lock_release_re(&inode->lock);
  }
end:
  ASSERT(sector < block_size(fs_device));
  return sector;
}



/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init(&inode_list_lock);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  log_debug("!!!inode_create (sector %d, length %d, dir %d)!!!\n", sector, length, is_dir);
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->is_dir = is_dir;
      if (free_map_allocate (1, &disk_inode->start))
        {
          in_cache_and_overwrite_block (sector, 0, disk_inode, sizeof(*disk_inode));

          zero_out_sector_data(disk_inode->start);

          if (sector == FREE_MAP_SECTOR) {
              // special case for handling the free map
              // as soon as the free_map_file is create the free map will always
              // try to write the data to disk immediately
              // this causes us problems in case we want to allocate space on disk
              // for the free_map itself, because we get into a loop of request/write/expand
              // cycles
              //
              // By allocating the file completely in front, this should be avoidable


              // assume at most 128 blocks are needed for the free map
              block_sector_t indirect_blk, data_start, tmp;
              ASSERT(free_map_allocate(1, &indirect_blk));

              // link indirect in start block
              in_cache_and_overwrite_block(disk_inode->start,
                                           0, // first indirect block
                                           &indirect_blk,
                                           sizeof(indirect_blk));
              // get blocks for raw file data
              size_t blocks_needed = DIV_ROUND_UP(length, BLOCK_SECTOR_SIZE);
              ASSERT(free_map_allocate(blocks_needed, &data_start));
              zero_out_sector_data(indirect_blk);
              int i;
              for (i = 0; i < blocks_needed; i++) {
                  tmp = data_start + i;
              
                  in_cache_and_overwrite_block(indirect_blk,
                                               i * sizeof(tmp),
                                               &tmp,
                                               sizeof(tmp));
              }
          }
          // now update the data in the start sector with actual addresses
          success = true;
        }
      free (disk_inode);
    }
  ASSERT(sector < block_size(fs_device));
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  log_debug("!!!inode_open (sector %d)!!!\n", sector);
  struct list_elem *e;
  struct inode *inode;

  lock_acquire_re(&inode_list_lock);
  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e)) 
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector) 
        {
          inode_reopen (inode);
          lock_release_re(&inode_list_lock);
          return inode; 
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL) {
    lock_release_re(&inode_list_lock);
    return NULL;
  }
  /* Initialize. */

  list_push_front (&open_inodes, &inode->elem);
  lock_init(&(inode->lock));
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  lock_release_re(&inode_list_lock);

  in_cache_and_read(inode->sector,
                    offsetof(struct inode_disk,start),
                    ((void*)inode) + offsetof(struct inode,start),
                    offsetof(struct inode_disk,unused2) - offsetof(struct inode_disk,start));
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  log_debug("!!!inode_reopen!!!\n");
  if (inode != NULL) {
    lock_acquire_re(&inode->lock);
    inode->open_cnt++;
    lock_release_re(&inode->lock);
  }
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (struct inode *inode)
{
  log_debug("!!!inode_get_inumber!!!\n");
  lock_acquire_re(&inode->lock);
  block_sector_t tmp = inode->sector;
  lock_release_re(&inode->lock);
  return tmp;
}


bool
inode_get_removed (struct inode *inode)
{
  log_debug("!!!inode_get_removed!!!\n");
  lock_acquire_re(&inode->lock);
  bool tmp = inode->removed;
  lock_release_re(&inode->lock);
  return tmp;
}


/* Returns INODE's inode number. */
bool
inode_isdir (struct inode *inode)
{
  log_debug("!!!inode_isdir!!!\n");
  lock_acquire_re(&inode->lock);
  bool tmp = inode->is_dir;
  lock_release_re(&inode->lock);
  return tmp;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode) 
{
  log_debug("!!!inode_close (sector %d)!!!\n", inode->sector);
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  lock_acquire_re(&inode->lock);
  /* Release_Re resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      log_debug("Close inode %d.\n", inode->sector);
      lock_acquire_re(&inode_list_lock);
      /* Remove from inode list and release_re lock. */
      list_remove (&inode->elem);
      lock_release_re(&inode_list_lock);
      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          log_debug("Remove inode %d after close.\n", inode->sector);
          block_sector_t start[128];
          in_cache_and_read(inode->start, 0, &start, BLOCK_SECTOR_SIZE);
          int i,j;
          for (i=0; i<128;i++) {
            block_sector_t blocks[128];
            if (start[i]== NON_EXISTANT) continue;
            in_cache_and_read(start[i], 0, &blocks, BLOCK_SECTOR_SIZE);
            for (j=0; j<128; j++) {
              if (blocks[j]== NON_EXISTANT) continue;
              free_map_release(blocks[j], 1);
            }
            free_map_release(start[i],1);
          }
          free_map_release(inode->start,1);
          free_map_release(inode->sector, 1);
        }
      free (inode);
      return;
    }
  lock_release_re(&inode->lock);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  log_debug("!!!inode_remove!!!\n");
  ASSERT (inode != NULL);
  lock_acquire_re(&inode->lock);
  inode->removed = true;
  lock_release_re(&inode->lock);
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  log_debug("!!!inode_read_at!!!\n");
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_idx == NON_EXISTANT) {
        memset(buffer + bytes_read, 0, chunk_size);
      }
      else {
          /* Read full sector directly into caller's buffer. */
      in_cache_and_read (sector_idx,
                         sector_ofs,
                         buffer + bytes_read,
                         chunk_size);
      }


      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, void *buffer_, off_t size,
                off_t offset) 
{
  log_debug("!!!inode_write_at (inode %d, size %d, offset %d)!!!\n", inode->sector, size, offset);
  uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  off_t o_offset = offset;

  lock_acquire_re(&inode->lock);
  if (inode->deny_write_cnt) {
     lock_release_re(&inode->lock);
     return 0;
  }
  lock_release_re(&inode->lock);

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector_expand (inode, offset);
      if (sector_idx == NON_EXISTANT) break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */

      int min_left = BLOCK_SECTOR_SIZE - sector_ofs;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      in_cache_and_overwrite_block (sector_idx,
                                    sector_ofs,
                                    buffer + bytes_written,
                                    chunk_size);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  lock_acquire_re(&inode->lock);
  inode->length = inode->length > o_offset + bytes_written ?
                  inode->length : o_offset + bytes_written ;
  lock_release_re(&inode->lock);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  log_debug("!!!inode_deny_write!!!\n");
  lock_acquire_re(&inode->lock);
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  lock_release_re(&inode->lock);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  log_debug("!!!inode_allow_write!!!\n");
  lock_acquire_re(&inode->lock);
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
  lock_release_re(&inode->lock);
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  log_debug("!!!inode_length!!!\n");
  lock_acquire_re(&inode->lock);
  off_t tmp = inode->length;
  lock_release_re(&inode->lock);
  return tmp;
}

void
inode_acquire(struct inode * i) {
  ASSERT(i!=NULL);
  lock_acquire_re(&i->lock);
}

void
inode_release(struct inode * i) {
  ASSERT(i!=NULL);
  lock_release_re(&i->lock);
}
