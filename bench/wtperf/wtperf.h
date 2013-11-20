/*-
 * Public Domain 2008-2013 WiredTiger, Inc.
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
#include <sys/time.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wiredtiger.h>
#include <wiredtiger_ext.h>
#include <gcc.h>				/* WiredTiger internal */

typedef struct {		/* Per-thread structure */
	void *cfg;		/* Enclosing configuration */

	pthread_t  handle;	/* Handle */

	uint64_t   ckpt_ops;	/* Checkpoint ops */
	uint64_t   insert_ops;	/* Insert ops */
	uint64_t   read_ops;	/* Read ops */
	uint64_t   update_ops;	/* Update ops */
} CONFIG_THREAD;

typedef struct {
	const char *home;	/* WiredTiger home */
	char *uri;		/* Object URI */

	WT_CONNECTION *conn;	/* Database connection */

	FILE *logf;		/* Logging handle */

	CONFIG_THREAD *ckptthreads,
	    *ithreads, *popthreads, *rthreads, *uthreads;

	enum { WT_PERF_INIT, WT_PERF_POPULATE, WT_PERF_WORKER } phase;

	struct timeval phase_start_time;

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define	OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT
} CONFIG;

typedef enum {
	BOOL_TYPE, CONFIG_STRING_TYPE, INT_TYPE, STRING_TYPE, UINT32_TYPE
} CONFIG_OPT_TYPE;

typedef struct {
	const char *name;
	const char *description;
	const char *defaultval;
	CONFIG_OPT_TYPE type;
	size_t offset;
} CONFIG_OPT;

/* Worker thread types. */
typedef enum {
    WORKER_READ, WORKER_INSERT, WORKER_INSERT_RMW, WORKER_UPDATE } worker_type;

int	 config_assign(CONFIG *, const CONFIG *);
void	 config_free(CONFIG *);
int	 config_opt_file(CONFIG *, WT_SESSION *, const char *);
int	 config_opt_line(CONFIG *, WT_SESSION *, const char *);
int	 config_opt_str(CONFIG *, WT_SESSION *, const char *, const char *);
int	 enomem(const CONFIG *);
void	 lprintf(const CONFIG *, int err, uint32_t, const char *, ...)
	   WT_GCC_ATTRIBUTE((format (printf, 4, 5)));
void	 print_config(CONFIG *);
int	 setup_log_file(CONFIG *);
void	 usage(void);
