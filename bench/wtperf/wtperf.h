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
#include <gcc.h>			/* WiredTiger internal */

typedef struct __config CONFIG;
typedef struct __config_thread CONFIG_THREAD;

struct __config {			/* Configuration struction */
	const char *home;		/* WiredTiger home */
	char *uri;			/* Object URI */

	WT_CONNECTION *conn;		/* Database connection */

	FILE *logf;			/* Logging handle */

	CONFIG_THREAD *ckptthreads,
	    *ithreads, *popthreads, *rthreads, *uthreads;

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define	OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT
};

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

#define	THOUSAND	(1000ULL)
#define	MILLION		(1000000ULL)
#define	BILLION		(1000000000ULL)
#define	ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

#define	ns_to_ms(v)	((v) / MILLION)
#define	ns_to_sec(v)	((v) / BILLION)
#define	ns_to_us(v)	((v) / THOUSAND)

#define	us_to_ms(v)	((v) / THOUSAND)
#define	us_to_ns(v)	((v) * THOUSAND)
#define	us_to_sec(v)	((v) / MILLION)

#define	ms_to_ns(v)	((v) * MILLION)
#define	ms_to_us(v)	((v) * THOUSAND)
#define	ms_to_sec(v)	((v) / THOUSAND)

#define	sec_to_ns(v)	((v) * BILLION)
#define	sec_to_us(v)	((v) * MILLION)
#define	sec_to_ms(v)	((v) * THOUSAND)

typedef struct {
	uint64_t ops;			/* Total operations */
	uint32_t aggro;			/* Aggregated operations */

	/*
	 * Latency buckets.
	 */
	uint64_t us[1000];		/* < 1us ... 1000us */
	uint64_t ms[1000];		/* < 1ms ... 1000ms */
	uint64_t sec[100];		/* < 1s 2s ... 100s */
} TRACK;

struct __config_thread {		/* Per-thread structure */
	CONFIG *cfg;			/* Enclosing configuration */

	pthread_t handle;		/* Handle */

	char *key_buf, *data_buf;	/* Key/data memory */

#define	WORKER_READ		1	/* Read */
#define	WORKER_INSERT		2	/* Insert */
#define	WORKER_INSERT_RMW	3	/* Insert with read-modify-write */
#define	WORKER_UPDATE		4	/* Update */
	uint8_t	schedule[100];		/* Thread operations */

	TRACK ckpt;			/* Checkpoint operations */
	TRACK insert;			/* Insert operations */
	TRACK read;			/* Read operations */
	TRACK update;			/* Update operations */
};

int	 config_assign(CONFIG *, const CONFIG *);
void	 config_free(CONFIG *);
int	 config_opt_file(CONFIG *, WT_SESSION *, const char *);
int	 config_opt_line(CONFIG *, WT_SESSION *, const char *);
int	 config_opt_str(CONFIG *, WT_SESSION *, const char *, const char *);
void	 config_print(CONFIG *);
int	 config_sanity(CONFIG *);
void	 dump_latency(CONFIG *);
int	 enomem(const CONFIG *);
const char *
	 op_name(uint8_t *);
void	 lprintf(const CONFIG *, int err, uint32_t, const char *, ...)
	   WT_GCC_ATTRIBUTE((format (printf, 4, 5)));
int	 setup_log_file(CONFIG *);
uint64_t sum_ckpt_ops(CONFIG *);
uint64_t sum_insert_ops(CONFIG *);
uint64_t sum_pop_ops(CONFIG *);
uint64_t sum_read_ops(CONFIG *);
uint64_t sum_update_ops(CONFIG *);
void	 usage(void);
