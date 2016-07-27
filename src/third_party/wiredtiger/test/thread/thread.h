/*-
 * Public Domain 2014-2016 MongoDB, Inc.
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

#include "test_util.h"

#include <signal.h>

#define	FNAME		"file:wt.%03d"		/* File name */
#define	FNAME_STAT	"__stats"		/* File name for statistics */

extern WT_CONNECTION *conn;			/* WiredTiger connection */

typedef enum { FIX, ROW, VAR } __ftype;		/* File type */
extern __ftype ftype;

extern int   log_print;				/* Log print per operation */
extern int   multiple_files;			/* File per thread */
extern u_int nkeys;				/* Keys to load */
extern u_int max_nops;				/* Operations per thread */
extern int   vary_nops;				/* Operations per thread */
extern int   session_per_op;			/* New session per operation */

void load(const char *);
int  rw_start(u_int, u_int);
void stats(void);
void verify(const char *);
