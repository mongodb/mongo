/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

#include <signal.h>

#include "test_util.h"

#define FNAME "file:cursor_order.%03d" /* File name */

typedef enum { FIX, ROW, VAR } __ftype; /* File type */

typedef struct {
    uint64_t append_inserters; /* Number of append threads */
    WT_CONNECTION *conn;       /* WiredTiger connection */
    __ftype ftype;
    uint64_t key_range;        /* Current key range */
    uint64_t max_nops;         /* Operations per thread */
    bool multiple_files;       /* File per thread */
    uint64_t nkeys;            /* Keys to load */
    uint64_t reverse_scanners; /* Number of scan threads */
    uint64_t reverse_scan_ops; /* Keys to visit per scan */
    bool thread_finish;        /* Signal to finish run. */
    bool vary_nops;            /* Operations per thread */

} SHARED_CONFIG;

void load(SHARED_CONFIG *, const char *);
void ops_start(SHARED_CONFIG *);
void verify(SHARED_CONFIG *, const char *);
