#ifndef USERPROG_PAGEDIR_H
#define USERPROG_PAGEDIR_H

#include <stdbool.h>
#include <stdint.h>

#include "threads/synch.h"

extern struct lock vm_lock;

uint32_t *pagedir_create (void);
void pagedir_destroy (uint32_t *pd);
bool pagedir_set_page (uint32_t *pd, void *upage, void *kpage, bool rw);
bool pagedir_set_page_pin (uint32_t *pd, void *upage, void *kpage, bool rw, bool pin);
void *pagedir_get_page (uint32_t *pd, const void *upage);
void pagedir_clear_page (uint32_t *pd, void *upage);
bool pagedir_set_page_not_present(uint32_t *pd, void *upage);
bool pagedir_is_assigned(uint32_t *pd, const void *upage);
bool pagedir_is_present (uint32_t *pd, const void *vpage);
bool pagedir_is_writeable (uint32_t *pd, const void *vpage);
void pagedir_set_not_present (uint32_t *pd, const void *vpage);
bool pagedir_is_dirty (uint32_t *pd, const void *upage);
void pagedir_set_dirty (uint32_t *pd, const void *upage, bool dirty);
bool pagedir_is_accessed (uint32_t *pd, const void *upage);
void pagedir_set_accessed (uint32_t *pd, const void *upage, bool accessed);
void pagedir_activate (uint32_t *pd);

#endif /* userprog/pagedir.h */
