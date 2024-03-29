#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  // TODO flush cache
  free_map_close ();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *path,
                off_t       initial_size,
                bool        isdir)
{
  struct file *parent = NULL;
  char dirname_ [NAME_MAX + 1];
  char *dirname = (char*) dirname_;
  bool success = false;
  if (!file_deconstruct_path(path, &parent, NULL, &dirname) || parent == NULL) {
    return false;
  }
  block_sector_t inode_sector = 0;
  success = (free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, isdir)
                  && dir_add (parent, dirname, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (parent);
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  struct file* file;
  if(!file_deconstruct_path(name, NULL,&file, NULL)) {
    return NULL;
  }
  return file;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *path)
{
  struct file *dir, *f;
  char fname_[NAME_MAX + 1];
  char *fname = fname_;

  if (!file_deconstruct_path(path, &dir, &f, &fname)) {
    return false;
  }

  bool success = !file_isroot(f) && dir_remove (dir, fname);
  dir_close (dir);
  if (file_isdir(f)) {
    file_close(f);
  } else {
    dir_close(f);
  }
  return success;
}

/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
