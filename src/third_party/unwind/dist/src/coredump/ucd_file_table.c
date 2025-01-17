/*
 * Copyright 2022 Blackberry Limited.
 * Contributed by Stephen M. Webb <stephen.webb@bregmasoft.ca>
 *
 * This file is part of libunwind, a platform-independent unwind library.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "ucd_file_table.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>


/**
 * Initialize a UCD file object.
 * @param[in] ucd_file  The `ucd_file_t` object to initialize.
 * @param[in] filename  Name of a file.
 *
 * Stores the filename in the object and sets the fd to an uninitialized state.
 *
 * @returns UNW_ESUCCESS on success, a negated `unw_error_t` code otherwise.
 */
unw_error_t
ucd_file_init (ucd_file_t *ucd_file, char const *filename)
{
  size_t name_size = strlen (filename) + 1;
  ucd_file->filename = malloc (name_size);
  if (ucd_file->filename == NULL)
    {
      Debug (0, "error %d from malloc(): %s\n", errno, strerror (errno));
      return (unw_error_t) - UNW_ENOMEM;
    }
  memcpy ((char *)ucd_file->filename, filename, name_size);
  ucd_file->fd    = -1;
  ucd_file->size  = 0;
  ucd_file->image = NULL;

  return UNW_ESUCCESS;
}


/**
 * Dispose of a UCD file object.
 * @param[in] ucd_file  The UCD file to dispose.
 *
 * Releases any resources sued and sets the object to an uninitialized state.
 *
 * @returns UNW_ESUCCESS always.
 */
unw_error_t
ucd_file_dispose (ucd_file_t *ucd_file)
{
  ucd_file_unmap(ucd_file);
  if (ucd_file->filename != NULL)
    {
      free ((char *)ucd_file->filename);
      ucd_file->filename = NULL;
    }

  return UNW_ESUCCESS;
}


/**
 * Opens a UCD file and gets its size
 */
static void
_ucd_file_open (ucd_file_t *ucd_file)
{
  ucd_file->fd = open(ucd_file->filename, O_RDONLY);
  if (ucd_file->fd == -1)
	{
	  Debug(0, "error %d in open(%s): %s\n", errno, ucd_file->filename, strerror(errno));
	  return;
	}

  struct stat sbuf;
  int sstat = fstat(ucd_file->fd, &sbuf);
  if (sstat != 0)
    {
	  Debug(0, "error %d in fstat(%s): %s\n", errno, ucd_file->filename, strerror(errno));
	  close(ucd_file->fd);
	  ucd_file->fd = -1;
    }
  ucd_file->size = sbuf.st_size;
}


/**
 * Memory-maps a UCD file
 */
uint8_t *
ucd_file_map (ucd_file_t *ucd_file)
{
  if (ucd_file->image != NULL)
    {
      return ucd_file->image;
    }

  if (ucd_file->fd == -1)
    {
      _ucd_file_open (ucd_file);
	}

  ucd_file->image = mi_mmap(NULL, ucd_file->size, PROT_READ, MAP_PRIVATE, ucd_file->fd, 0);
  if (ucd_file->image == MAP_FAILED)
	{
	  Debug(0, "error in mmap(%s)\n", ucd_file->filename);
	  ucd_file->image = NULL;
	  return NULL;
	}
  return ucd_file->image;
}


void
ucd_file_unmap (ucd_file_t *ucd_file)
{
  if (ucd_file->image != NULL)
    {
    	munmap(ucd_file->image, ucd_file->size);
    	ucd_file->image = NULL;
    	ucd_file->size  = 0;
    }
  if (ucd_file->fd != -1)
    {
    	close(ucd_file->fd);
    	ucd_file->fd = -1;
    }
}


/**
 * Initialize a UCD file table.
 * @param[in] ucd_file_table  The UCD file table to initialize.
 *
 * @returns UNW_ESUCCESS on success, a negated `unw_error_t` code otherwise.
 */
unw_error_t
ucd_file_table_init (ucd_file_table_t *ucd_file_table)
{
  ucd_file_table->uft_count = 0;
  ucd_file_table->uft_size = 2;
  ucd_file_table->uft_files = calloc (ucd_file_table->uft_size,
  									  sizeof (ucd_file_t));

  if (ucd_file_table->uft_files == NULL)
    {
      Debug (0, "error %d from malloc(): %s\n", errno, strerror (errno));
      return (unw_error_t) - UNW_ENOMEM;
    }

  return UNW_ESUCCESS;
}


/**
 * Dispose of a UCD file table object.
 * @param[in] ucd_file_table  The UCD file table to dispose.
 *
 * Releases any resources used and sets the object to an uninitialized state.
 *
 * @returns UNW_ESUCCESS always.
 */
unw_error_t
ucd_file_table_dispose (ucd_file_table_t *ucd_file_table)
{
  if (ucd_file_table->uft_files != NULL)
    {
      for (size_t i = 0; i < ucd_file_table->uft_count; ++i)
        {
          ucd_file_dispose(&ucd_file_table->uft_files[i]);
        }
      free (ucd_file_table->uft_files);
      ucd_file_table->uft_files = NULL;
    }

  ucd_file_table->uft_count = 0;
  ucd_file_table->uft_size = 0;

  return UNW_ESUCCESS;
}


/**
 * Insert a new entry in a UCD file table.
 * @param[in] ucd_file_table  A UCD file table
 * @param[in] filename        The filename to add.
 *
 * This table doe not allow duplicates: if a filename is already in the table,
 * the index of that entry is returned, otherwise a new entry is created and
 * the index of the new entry is returned.
 *
 * @returns the index of the newly-added UCD file, a negative `unw_error_t`
 * code indicating failure otherwise.
 */
ucd_file_index_t ucd_file_table_insert (ucd_file_table_t *ucd_file_table,
										char const       *filename)
{
  for (int i = 0; i < (int)ucd_file_table->uft_count; ++i)
    {
      if (strcmp (ucd_file_table->uft_files[i].filename, filename) == 0)
        {
          return i;
        }
    }

  ucd_file_index_t index = ucd_file_table->uft_count;
  ++ucd_file_table->uft_count;

  if (ucd_file_table->uft_count >= ucd_file_table->uft_size)
    {
      size_t new_size = ucd_file_table->uft_size * 2;
      ucd_file_table->uft_files = realloc (ucd_file_table->uft_files,
      									   new_size * sizeof (ucd_file_t));
      if (ucd_file_table->uft_files == NULL)
        {
          Debug (0, "error %d from malloc(): %s\n", errno, strerror (errno));
          return (unw_error_t) - UNW_ENOMEM;
        }

      ucd_file_table->uft_size = new_size;
    }

  unw_error_t err = ucd_file_init (&ucd_file_table->uft_files[index], filename);
  if (err != UNW_ESUCCESS)
    {
	  return err;
	}
  return index;
}


/**
 * Get an indicated entry from a UCD file table.
 * @param[in] ucd_file_table  A UCD file table
 * @param[in] index           Indicate which entry to retrieve.
 *
 * @returns a pointer to the indicated UCD file, NULL if the index is out of
 * range.
 */
ucd_file_t *
ucd_file_table_at (ucd_file_table_t *ucd_file_table,
							   ucd_file_index_t  index)
{
  if (0 <= index && index < (int)ucd_file_table->uft_count)
    {
      return &ucd_file_table->uft_files[index];
    }

  return NULL;
}

