#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "userprog/syscall.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "lib/kernel/list.h"
#include "lib/user/syscall.h"
#include "vm/spage.h"
#include "vm/frames.h"

#define PID_ERROR ((pid_t) -1)
#define PID_MAX ((pid_t) 256)
#define PROCESS_NO_EXIT_STATUS -3

static pid_t allocate_pid (void);
static void clear_process_state(pid_t pid);
static void clear_process_state_(pid_t pid, bool init_list);
static thread_func start_process NO_RETURN;
static bool load (char *cmdline, void (**eip) (void), void **esp);

struct start_process_param
{
  pid_t pid;
  pid_t parent_pid;
  char *cmdline;
  struct semaphore sema;
  bool success;
};

struct pid_item
{
  // boilerplate entry
  struct list_elem elem;
  
  pid_t pid;
};


static bool
pid_item_less (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED)
{
  struct pid_item *pid_a = list_entry(a, struct pid_item, elem);
  struct pid_item *pid_b = list_entry(b, struct pid_item, elem);
  return pid_a->pid < pid_b->pid;
}

enum process_status
{
  PROCESS_UNUSED,     /* indicates a free entry */
  PROCESS_RUNNING,    /* Normal state */
  PROCESS_WAIT,       /* indicates process called wait() */
  PROCESS_ZOMBIE      /* Process is dead but parent did not call wait() yet */
};

// listitem for open file desricptor list
struct fdlist_item
{
  struct list_elem elem;
  int fd;
  struct file *file;
};

struct mmaplist_item
{
  struct list_elem elem;
  struct mmapdata data;
};

struct process_state_item
{
  // tid of this process
  tid_t tid;
  // pid of parent process
  pid_t parent_pid;
  
  // executeable file, to prevent writes on this file
  struct file *file;
  
  // status
  enum process_status status;
  // exit status as set by exit()
  int exit_status_value;
  
  // if process_state == PROCESS_WAIT the pid
  // of the process we are waiting on
  pid_t wait_for_child;
  
  // un-wait()-ed child processes
  // list must be ordered
  struct list to_wait_on_list;
  
  // open file descriptor list
  int nextfd;
  struct list fdlist;

  // mmaped files
  int nextmapid;
  struct list mmaplist;
};

// contains for all pid an entry
static struct process_state_item process_states[PID_MAX];
// Set to the lowest pid we have to check for a
// free pid value. If we reclaim a pid and it 
// is lower than pid_search_start we of course
// have to set pid_search_start to the reclaimed
// value.
static pid_t pid_search_start = 1;

// general lock for all pid related things
static struct lock pid_lock;
// condition to wait on any exit of any other process
// caller must hold the `pid_lock` to use this condition
static struct condition process_exit_cond;

/* Returns a pid to use for a new thread or 0 on error
   e.g. no more usable pids. */
static pid_t
allocate_pid (void) 
{
  pid_t pid = PID_ERROR;
  
  /*
  for (int i = 0; i < PID_MAX; i++)
  {
    switch(process_states[i].status)
    {
      case PROCESS_UNUSED:
          putchar('.');
          break;
      case PROCESS_RUNNING:
          putchar('R');
          break;
      case PROCESS_WAIT:
          putchar('W');
          break;
      case PROCESS_ZOMBIE:
          putchar('+');
          break;
      default:
          break;
    }
  }
  putchar('\n');
  */

  lock_acquire (&pid_lock);
  pid_t pid_ = pid_search_start;
  while (pid_ < PID_MAX)
  {
    if (process_states[pid_].status == PROCESS_UNUSED)
    {
      // process unused
      pid = pid_;
      // next possible free pid is pid+1
      pid_search_start = pid+1;
      break;
    }
    pid_++;
  }
  lock_release (&pid_lock);

  return pid;
}

// MUST only be called if pid_lock is held
static void
clear_process_state(pid_t pid)
{
  if (0 <= pid && pid <= PID_MAX)
  {
    clear_process_state_(pid, false);
  }
}

// MUST only be called if pid_lock is held
static void
clear_process_state_(pid_t pid, bool init_list)
{
  ASSERT(lock_held_by_current_thread(&pid_lock));
  
  // reset PID search start, as we now know
  // this PID is no longer in use
  pid_search_start = pid > 0 && pid < pid_search_start ? pid : pid_search_start;
  
  // list manipulation is ok, because lock is held
  process_states[pid].tid = 0;
  process_states[pid].parent_pid = PID_ERROR;
  process_states[pid].status = PROCESS_UNUSED;
  process_states[pid].exit_status_value = PROCESS_NO_EXIT_STATUS;
  process_states[pid].wait_for_child = 0;
  process_states[pid].nextfd = 2;
  process_states[pid].nextmapid = 1;
  if (init_list)
  {
    process_states[pid].file = NULL;
    
    // initialize a new list
    list_init (&process_states[pid].to_wait_on_list);
    list_init (&process_states[pid].fdlist);
    list_init (&process_states[pid].mmaplist);
  }
  else
  {
    // close potential open file
    if (process_states[pid].file != NULL)
    {
      file_close(process_states[pid].file);
      process_states[pid].file = NULL;
    }
    
    struct list_elem *e;
    // remove all entries from list
    while (!list_empty(&process_states[pid].to_wait_on_list))
    {
      // TODO make rm_parent function
      // use here and in normal cleanup code
      e = list_front(&process_states[pid].to_wait_on_list);
      list_remove(e);
      free(e);
    }
    
    // Cleanup fdlist
    close_fdlist(pid);
    close_mmaplist(pid);

    e = NULL;

  };
}

/* function to cleanup mmaplist. */
void
close_mmaplist(int pid)
{
  struct list_elem *e;
  while (!list_empty(&process_states[pid].mmaplist))
    {
      e = list_front(&process_states[pid].mmaplist);
      struct mmaplist_item *e_ = list_entry (e, struct mmaplist_item, elem);
      delete_mmaplist(pid, e_->data.mapid);
    }
}

/* function to insert mappings into the mmaplist.
   returns the mapid used for this mapping */
mapid_t
insert_mmaplist(pid_t pid, void *base_addr, struct file *f)
{
  log_debug("    insert_mmaplist (pid: %d)    \n", pid);
  lock_acquire(&pid_lock);
  struct mmaplist_item *e = malloc(sizeof(struct mmaplist_item));
  e->data.file = f;
  e->data.base_addr = base_addr;
  e->data.pgcount = 0;
  e->data.mapid = process_states[pid].nextmapid++;
  list_push_back(&process_states[pid].mmaplist, (struct list_elem *) e);
  lock_release(&pid_lock);
  return e->data.mapid;
}

/* function to insert mappings into the mmaplist.
   returns the mapid used for this mapping */
bool
inc_pgcount_mmaplist(pid_t pid, mapid_t mapid)
{
  log_debug("    inc_pgcount_mmaplist (pid: %d, mapid: %d)    \n", pid, mapid);
  lock_acquire(&pid_lock);
  struct list_elem *e;
  bool result = false;

  for (e = list_begin (&process_states[pid].mmaplist); e != list_end (&process_states[pid].mmaplist);
    e = list_next (e))
    {
      struct mmaplist_item *e_ = list_entry (e, struct mmaplist_item, elem);
      if (e_->data.mapid == mapid) {
        e_->data.pgcount++;
        result = true;
        break;
      }
    }
  lock_release(&pid_lock);
  return result;
}


/* function to delete mappings from the mmaplist.
   returns true if mapping was found otherwise returns false */
void
delete_mmaplist(pid_t pid, mapid_t mapid)
{
  log_debug("    delete_mmaplist (pid: %d, mapid: %d)    \n", pid, mapid);
  lock_acquire(&pid_lock);
  struct list_elem *e;

  for (e = list_begin (&process_states[pid].mmaplist); e != list_end (&process_states[pid].mmaplist);
    e = list_next (e))
    {
      struct mmaplist_item *e_ = list_entry (e, struct mmaplist_item, elem);
      if (e_->data.mapid == mapid) {
        list_remove(e);
        size_t i;
        for(i = 0; i < e_->data.pgcount; i++) {
            spage_map_munmap(e_->data.base_addr + i * PGSIZE);
        }
        file_close(e_->data.file);
        free(e_);
        break;
      }
    }
  lock_release(&pid_lock);
  return;
}

/* function to cleanup fdlist. */
void
close_fdlist(int pid)
{
  struct list_elem *e;
  while (!list_empty(&process_states[pid].fdlist))
    {
      e = list_front(&process_states[pid].fdlist);
      struct fdlist_item *f = list_entry (e, struct fdlist_item, elem);
      file_close(f->file);
      list_remove(e);
      free(e);
    }
}

/* function to insert files into the fdlist.
   returns the fd used for this file */
int
insert_fdlist(pid_t pid, struct file* f)
{
  lock_acquire(&pid_lock);
  struct fdlist_item *e = malloc(sizeof(struct fdlist_item));
  e->file = f;
  e->fd = process_states[pid].nextfd++;
  list_push_back(&process_states[pid].fdlist, (struct list_elem *) e);
  lock_release(&pid_lock);
  return e->fd;
}


/* function to delete files from the fdlist.
   returns true if file was found otherwise returns false */
bool
delete_fdlist(pid_t pid, int fd)
{
  lock_acquire(&pid_lock);
  struct list_elem *e;
  bool result = false;

  for (e = list_begin (&process_states[pid].fdlist); e != list_end (&process_states[pid].fdlist);
    e = list_next (e))
    {
      struct fdlist_item *f = list_entry (e, struct fdlist_item, elem);
      if (f->fd == fd)
      {
        list_remove(&(f->elem));
        result = true;
      }
    }
  result = false;
  lock_release(&pid_lock);
  return result;
}

/* returns the file struct pointer given the pid and the filedescriptor.
   if filedescriptor is unused returns null. */
struct file*
get_fdlist(pid_t pid, int fd)
{
  lock_acquire(&pid_lock);
  struct list_elem *e;
  struct file *result = NULL;

  for (e = list_begin (&process_states[pid].fdlist); e != list_end (&process_states[pid].fdlist);
    e = list_next (e))
    {
      struct fdlist_item *f = list_entry (e, struct fdlist_item, elem);
      if (f->fd == fd)
      {
        result =  f->file;
        break;
      }
    }
  lock_release(&pid_lock);
  return result;
}

void
process_init(void)
{
  log_debug("@@@ process_init called @@@\n");
  // init locks
  lock_init(&pid_lock);
  
  // init conditions
  cond_init(&process_exit_cond);
  
  lock_acquire (&pid_lock);
  // init process state list
  pid_t i;
  for (i = 0; i < PID_MAX; i++)
  {
    clear_process_state_(i, true);
  }
  lock_release (&pid_lock);
}

/* Starts a new thread running a user program loaded from
   CMDLINE.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created.
   
   cmdline MUST be smaller than one page */
pid_t
process_execute (const char *cmdline)
{
  log_debug("@@@ process_execute called: %s @@@\n", cmdline);
  char *fn_copy, *save_ptr, thread_name[16];
  tid_t tid;
  pid_t pid, parent_pid = thread_current()->pid;
  struct start_process_param param;
  
  // reserve a pid
  pid = allocate_pid();
  if (pid == PID_ERROR)
  {
    goto execute_fail;
  }
  
  // TODO do argument parsing here
  // start_process requires the provided string to start
  // on a non space character, so skip all spaces
  while (*cmdline == ' ')
  {
    cmdline++;
  }
  strlcpy(thread_name, cmdline, 16);
  strtok_r(thread_name, " ", &save_ptr);

  /* Make a copy of CMDLINE.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page (0);
  if (fn_copy == NULL)
    goto execute_fail;
  strlcpy (fn_copy, cmdline, PGSIZE);

  /* Create a new thread to execute CMDLINE. */
  param.pid = pid;
  param.parent_pid = parent_pid;
  param.cmdline = fn_copy;
  sema_init(&param.sema, 0);
  tid = thread_create (thread_name, PRI_DEFAULT, start_process, &param);
  if (tid == TID_ERROR)
  {
    goto execute_fail_free;
  }
  // wait till child has signaled its status
  sema_down(&param.sema);
  if (param.success == false)
  {
    goto execute_fail;
  }
  
  // setup of child successfull, allow to wait on child
  lock_acquire(&pid_lock);
  // aquire memory for list entry
  struct pid_item *e = malloc(sizeof(struct pid_item));
  e->pid = pid;
  list_insert_ordered(&(process_states[parent_pid].to_wait_on_list), (struct list_elem *) e, &pid_item_less, NULL);
  lock_release(&pid_lock);
  return pid;
  
  execute_fail_free:
  palloc_free_page (fn_copy);
  execute_fail:
  // reset process state
  lock_acquire(&pid_lock);
  clear_process_state(pid);
  lock_release(&pid_lock);
  return PID_ERROR;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *args)
{
  struct start_process_param *param = (struct start_process_param *) args;
  pid_t pid  = param->pid;
  pid_t parent_pid  = param->parent_pid;
  char *cmdline = param->cmdline;
  log_debug("@@@ start_process called: pid (%d) parent_pid (%d) cmdline %s @@@\n",
          pid, parent_pid, cmdline);
  struct intr_frame if_;
  bool success;
  
  // init process state
  thread_current()->pid = pid;
  lock_acquire(&pid_lock);
  process_states[pid].tid = thread_current()->tid;
  process_states[pid].parent_pid = parent_pid;
  process_states[pid].status = PROCESS_RUNNING;
  lock_release(&pid_lock);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (cmdline, &if_.eip, &if_.esp);

  /* If load failed, quit. */
  palloc_free_page (cmdline);
  if (!success)
  {
    // indicate failure to parent
    param->success = false;
    sema_up(&param->sema);
    process_exit_with_value(-1);
  }
  // indicate success to parent
  param->success = true;
  sema_up(&param->sema);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread PID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If PID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (pid_t child_pid) 
{
  log_debug("@@@ process_wait called (%d) @@@\n", child_pid);
  // pid of calling process
  pid_t pid = thread_current()->pid;
  log_debug("+++ pid %d +++\n", pid);
  bool may_wait = false;
  int res = -1;
  
  lock_acquire(&pid_lock);
  // process state of pid must contain child_pid
  // if not return -1
  // if child not zombie
  //    wait on condition
  // if child is zombie return exit value
  struct list_elem *e;

  // check if we are allowed to wait for this pid
  for (e = list_begin (&(process_states[pid].to_wait_on_list));
       e != list_end (&(process_states[pid].to_wait_on_list));
       e = list_next (e))
  {
    struct pid_item *pid_i = list_entry (e, struct pid_item, elem);
    
    log_debug("+++ (%d) may wait on pid: %d +++\n", pid, pid_i->pid);
    
    if (pid_i->pid == child_pid)
    {
      may_wait = true;
      break;
    }
  }
  
  log_debug("@@@ may_wait %d @@@\n", may_wait);
  // valid child
  if (may_wait)
  {
    // wait till child process becomes a zombie
    while (process_states[child_pid].status != PROCESS_ZOMBIE)
    {
      log_debug("--- (%d) waits on cond for child %d ---\n", pid, child_pid);
      // wait till the next process exits
      // could be our child, so recheck condition
      cond_wait(&process_exit_cond, &pid_lock);
      log_debug("--- (%d) continues on  cond for child %d ---\n", pid, child_pid);
    }
    log_debug("--- (%d) child %d now zombie ---\n", pid, child_pid);
    // remove posibility to wait for child a second time
    list_remove(e);
    free(e);
    e = NULL;
    // read exit value and reset state data
    res = process_states[child_pid].exit_status_value;
    clear_process_state(child_pid);
  }
  
  lock_release(&pid_lock);
  log_debug("exit process_wait with return value %d\n", res);
  return res;
}

/* Sets a exit status code and handles the process_state structure update.
   Afterwards thread_exit() is called, so it does not return.
 */
void NO_RETURN
process_exit_with_value (int exit_value)
{
  struct thread *cur = thread_current ();
  pid_t pid = cur->pid;
  
  // required exit message
  printf ("%s: exit(%d)\n", cur->name, exit_value);
  
  log_debug("@@@ (%d) process_exit_with_value called %d @@@\n", pid, exit_value);
  
  lock_acquire(&pid_lock);
  // remove all child zombies
  // remove parent from rest of childs
  //
  // signal_all condition
  
  // remove all child zombies
  // remove parent from rest of childs
  struct list_elem *e, *e2;
  for (e = list_begin (&(process_states[cur->pid].to_wait_on_list));
       e != list_end (&(process_states[cur->pid].to_wait_on_list));
       )
  {
    struct pid_item *pid_i = list_entry (e, struct pid_item, elem);
    
    // remove old zombie processes
    if (process_states[pid_i->pid].status == PROCESS_ZOMBIE)
    {
      clear_process_state(pid_i->pid);
    }
    else
    {
      // remove us as parent, so that those processes will not wait on us
      process_states[pid_i->pid].parent_pid = PID_ERROR;
    }
    
    // clear list
    e2 = list_next (e);
    list_remove(e);
    free(e);
    e = e2;
  }
  
  // if we have a parent the process must persist until a possible later call to wait
  // else we can remove it now
  if (process_states[pid].parent_pid != PID_ERROR)
  {
    process_states[pid].status = PROCESS_ZOMBIE;
    process_states[pid].exit_status_value = exit_value;
    close_fdlist(pid);
    // close open executeable file
    if (process_states[pid].file != NULL)
    {
      file_close(process_states[pid].file);
      process_states[pid].file = NULL;
    }
  
    // only if we have a parent any process could wait on us
    log_debug("--- Signal condition due to process %d ---\n", cur->pid);
    cond_broadcast(&process_exit_cond, &pid_lock);
  }
  else
  {
    clear_process_state(pid);
  }
  lock_release(&pid_lock);

  // cleanup additional entries
  close_mmaplist(pid);
  spage_destroy();
  
  thread_exit();
}

/* Free the current process's resources. */
void
process_exit (void)
{
  log_debug("@@@ process_exit called @@@\n");
  struct thread *cur = thread_current ();
  uint32_t *pd;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char *cmdline, char **save_ptr);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (char *cmdline, void (**eip) (void), void **esp) 
{
  log_debug("@@@ load called: %s @@@\n", cmdline);
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *save_ptr = NULL;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  /* Open executable file. */
  strtok_r(cmdline, " ", &save_ptr); /* terminate the filename with \n */
  file = filesys_open (cmdline);
  
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", cmdline);
      goto done; 
    }
  // prevent concurrent changes on this file
  // as long as it is executed
  file_deny_write(file);
  // copy file pointer to prevent writes
  // only works as long as the file is opened
  lock_acquire (&pid_lock);
  process_states[t->pid].file = file;
  lock_release (&pid_lock);

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", cmdline);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, cmdline, &save_ptr))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  log_debug("@@@ load status: %s @@@\n", success ? "success" : "!!PROBLEM!!");
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */


/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  while (read_bytes > 0 || zero_bytes > 0) {
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    bool res;
    if (read_bytes > 0) {
      res = spage_map_segment(file, ofs, upage, writable, page_read_bytes);
    } else {
      res = spage_map_zero(upage, writable);
    }
    if (res == false) {
      return false;
    }

    /* Advance */
/*
    printf("upage: %08x\nread_bytes: %d\nzero_bytes: %d\npage_read_bytes: %d\npage_zero_bytes: %d\nPGIZE: %d\n",
           upage,
           read_bytes,
           zero_bytes,
           page_read_bytes,
           page_zero_bytes,
           PGSIZE);
*/
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
    ofs += PGSIZE;
  }
  return true;

//  file_seek (file, ofs);
//  while (read_bytes > 0 || zero_bytes > 0)
//    {
//      /* Calculate how to fill this page.
//         We will read PAGE_READ_BYTES bytes from FILE
//         and zero the final PAGE_ZERO_BYTES bytes. */
//      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
//      size_t page_zero_bytes = PGSIZE - page_read_bytes;
//
//      /* Get a page of memory. */
//      uint8_t *kpage = palloc_get_page (PAL_USER);
//      if (kpage == NULL)
//        return false;
//
//      /* Load this page. */
//      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
//        {
//          palloc_free_page (kpage);
//          return false;
//        }
//      memset (kpage + page_read_bytes, 0, page_zero_bytes);
//
//      /* Add the page to the process's address space. */
//      if (!install_page (upage, kpage, writable))
//        {
//          palloc_free_page (kpage);
//          return false;
//        }
//
//      /* Advance. */
//      read_bytes -= page_read_bytes;
//      zero_bytes -= page_zero_bytes;
//      upage += PGSIZE;
//    }
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp_, char *cmdline_, char **save_ptr) 
{
  ASSERT(save_ptr != NULL);
  
  uint8_t *kpage1, *kpage2;
  bool success = false;

  kpage1 = frame_get_free();
  kpage2 = frame_get_free();
  if (kpage1 != NULL && kpage2 != NULL)
    {
      memset(kpage1, 0, PGSIZE);
      memset(kpage2, 0, PGSIZE);
      success = install_page (((uint8_t *) PHYS_BASE) - 2*PGSIZE, kpage1, true, false);
      success = success && install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage2, true, false);
      if (success)
      {
        // copy argument to upper page
        memcpy(kpage2, cmdline_, PGSIZE);
        char *cmdline = (char*)((uint8_t *) PHYS_BASE) - PGSIZE; /* nice pointer to work with */
        // since cmdline_ starts at a page save_ptr can be used
        // if we replace the page part but not the offset
        *save_ptr = cmdline + pg_ofs(*save_ptr);
        
        // we already called strtok_r one time, to terminate the filename
        // because filename must be at the start at the cmdline string
        // no leading spaces, the pointer to cmdline is also our first token.
        // The rest of the tokens can be aquired by additional calls to strtok_r
        
        uint32_t *esp_start, *esp_end, *esp;
        uint32_t argc = 0;
        esp = *esp_;
        esp = PHYS_BASE - PGSIZE - 4;
        
#define PUSH(PTR, VAL) PTR -= 1; *PTR = VAL;
        // first argument on the stack is a pointer to the page we also need to free
        // because it contains our argc values
        PUSH(esp, (uint32_t) cmdline);

        // NULL pointer to terminate the argv list
        PUSH(esp, (uint32_t) NULL);
        // push first token
        PUSH(esp, (uint32_t) cmdline);
        argc++;
        //DEBUG log_debug("ArgN: %2d\tADDR: %p\tESP: %p\n", argc, cmdline, esp);
        esp_start = esp;
        
        char *token;
        // push char* from left to right onto the stack
        while ((token = strtok_r(NULL, " ", save_ptr)) != NULL)
        {
          PUSH(esp, (uint32_t) token);
          argc++;
          //DEBUG log_debug("ArgN: %2d\tADDR: %p\tESP: %p\n", argc, token, esp);
          
          // check for stack page overflow
          // bottom of stack page needs space for at least
          // the thread struct and 3 more values
          if ((uint32_t)((void*)esp - pg_round_down(esp)) <
              (uint32_t)(sizeof(struct thread) + 3 * sizeof(uint32_t)))
          {
            // stack would overflow
            success = false;
            goto no_success;
          }
        }
        esp_end = esp;
        
        // we need to reorder the pushed char* so that they are ordered from
        // right to left on the stack
        while (esp_start > esp_end)
        {
          // save value and swap values
          uint32_t tmp = *esp_start;
          *esp_start = *esp_end;
          *esp_end = tmp;
          
          // move pointers
          esp_start--;
          esp_end++;
        }
        //DEBUG log_debug("%p -- %p\n", esp_start, esp_end);
        
        // push argv itself (char**)
        esp_end = esp;
        PUSH(esp, (uint32_t) esp_end);
        
        // push argc value
        PUSH(esp, argc);
        // saved instruction pointer
        PUSH(esp, (uint32_t) NULL);
        
        *esp_ = esp;
        //DEBUG hex_dump(cmdline, cmdline, 0x20, true);
        //DEBUG hex_dump(esp-0x15, esp-0x15, 0x80, true);        
      }
      
      no_success:
      // cleanup in case of error
      if (!success)
      {
        frame_remove(kpage1);
        frame_remove(kpage2);
      }
    }
  return success;
}
