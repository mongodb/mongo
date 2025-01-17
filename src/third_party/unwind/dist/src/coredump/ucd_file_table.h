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
#ifndef include_ucd_file_table_h_
#define include_ucd_file_table_h_

#include "libunwind_i.h"

#include <stdint.h>
#include <sys/types.h>
/**
 * Describes a backing file.
 *
 * A *backing file* is usually the ELF image file that was the source of a
 * particular PT_LOAD segment in memory: it could be the program executable or
 * it could be a shared library, either resolved through the dynamic loader at
 * program start or later through dl_open().
 *
 * There may be one or more in-memory segments associated with the same backing
 * file.
 */
struct ucd_file_s
  {
    char const *filename;  /**< Name of the file */
    int         fd;        /**< File descriptor of the file if open, -1 otherwise */
    off_t       size;      /**< File size in bytyes */
    uint8_t    *image;     /**< Memory-mapped file image */
  };

typedef struct ucd_file_s ucd_file_t;

HIDDEN unw_error_t  ucd_file_init(ucd_file_t *ucd_file, char const *filename);
HIDDEN unw_error_t  ucd_file_dispose(ucd_file_t *ucd_file);
HIDDEN uint8_t     *ucd_file_map(ucd_file_t *ucd_file);
HIDDEN void         ucd_file_unmap(ucd_file_t *ucd_file);


/**
 * A table of backing files.
 *
 * Each entry in this table should be unique.
 *
 * This table is dynamically sized and should grow as required.
 */
struct ucd_file_table_s
  {
    size_t      uft_count;  /**< number of valid entries in table */
    size_t      uft_size;   /**< size (in entries) of the table storage */
    ucd_file_t *uft_files;  /**< the table data */
  };

typedef struct ucd_file_table_s ucd_file_table_t;
typedef int                     ucd_file_index_t;
static const ucd_file_index_t   ucd_file_no_index = -1;

HIDDEN unw_error_t ucd_file_table_init(ucd_file_table_t *ucd_file_table);
HIDDEN unw_error_t ucd_file_table_dispose(ucd_file_table_t *ucd_file_table);
HIDDEN ucd_file_index_t ucd_file_table_insert(ucd_file_table_t *ucd_file_table, char const *filename);
HIDDEN ucd_file_t *ucd_file_table_at(ucd_file_table_t *ucd_file_table, ucd_file_index_t index);

#endif /* include_ucd_file_table_h_ */
