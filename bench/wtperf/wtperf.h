/*-
 * Public Domain 2014-2015 MongoDB, Inc.
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

#ifndef _WIN32
#include <sys/time.h>
#endif
#include <sys/types.h>
#include <sys/stat.h>

#include <assert.h>
#include <ctype.h>
#ifndef _WIN32
#include <dirent.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#ifndef _WIN32
#include <pthread.h>
#endif
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <wt_internal.h>

#ifdef _WIN32
#include "windows_shim.h"
#endif

#include "config_opt.h"

typedef struct __config CONFIG;
typedef struct __config_thread CONFIG_THREAD;

#define	EXT_PFX	",extensions=("
#define	EXT_SFX	")"
#define	EXTPATH "../../ext/compressors/"		/* Extensions path */
#define	BLKCMP_PFX	",block_compressor="

#define	BZIP_BLK BLKCMP_PFX "bzip2"
#define	BZIP_EXT							\
	EXT_PFX EXTPATH "bzip2/.libs/libwiredtiger_bzip2.so" EXT_SFX
#define	LZ4_BLK BLKCMP_PFX "lz4"
#define	LZ4_EXT							\
	EXT_PFX EXTPATH "lz4/.libs/libwiredtiger_lz4.so" EXT_SFX
#define	SNAPPY_BLK BLKCMP_PFX "snappy"
#define	SNAPPY_EXT							\
	EXT_PFX EXTPATH "snappy/.libs/libwiredtiger_snappy.so" EXT_SFX
#define	ZLIB_BLK BLKCMP_PFX "zlib"
#define	ZLIB_EXT							\
	EXT_PFX EXTPATH "zlib/.libs/libwiredtiger_zlib.so" EXT_SFX

typedef struct {
	int64_t threads;		/* Thread count */
	int64_t insert;			/* Insert ratio */
	int64_t read;			/* Read ratio */
	int64_t update;			/* Update ratio */
	int64_t throttle;		/* Maximum operations/second */
		/* Number of operations per transaction. Zero for autocommit */
	int64_t ops_per_txn;

#define	WORKER_INSERT		1	/* Insert */
#define	WORKER_INSERT_RMW	2	/* Insert with read-modify-write */
#define	WORKER_READ		3	/* Read */
#define	WORKER_UPDATE		4	/* Update */
	uint8_t ops[100];		/* Operation schedule */
} WORKLOAD;

/*
 * NOTE:  If you add any fields to this structure here, you must also add
 * an initialization in wtperf.c in the default_cfg.
 */
struct __config {			/* Configuration struction */
	const char *home;		/* WiredTiger home */
	const char *monitor_dir;	/* Monitor output dir */
	char *base_uri;			/* Object URI */
	char **uris;			/* URIs if multiple tables */
	const char *helium_mount;	/* Optional Helium mount point */

	WT_CONNECTION *conn;		/* Database connection */

	FILE *logf;			/* Logging handle */

	char *async_config;		/* Config string for async */

	const char *compress_ext;	/* Compression extension for conn */
	const char *compress_table;	/* Compression arg to table create */

	CONFIG_THREAD *ckptthreads, *popthreads;

#define	WORKLOAD_MAX	50
	CONFIG_THREAD	*workers;	/* Worker threads */
	u_int		 workers_cnt;

	WORKLOAD	*workload;	/* Workloads */
	u_int		 workload_cnt;

	uint32_t	 use_asyncops;	/* Use async operations */
	/* State tracking variables. */

	uint64_t ckpt_ops;		/* checkpoint operations */
	uint64_t insert_ops;		/* insert operations */
	uint64_t read_ops;		/* read operations */
	uint64_t update_ops;		/* update operations */

	uint64_t insert_key;		/* insert key */

	volatile int ckpt;		/* checkpoint in progress */
	volatile int error;		/* thread error */
	volatile int stop;		/* notify threads to stop */
	volatile int in_warmup;		/* Running warmup phase */

	volatile uint32_t totalsec;	/* total seconds running */

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define	OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT
};

#define	ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

#define	THROTTLE_OPS	100

#define	THOUSAND	(1000ULL)
#define	MILLION		(1000000ULL)
#define	BILLION		(1000000000ULL)

#define	NSEC_PER_SEC	BILLION
#define	USEC_PER_SEC	MILLION
#define	MSEC_PER_SEC	THOUSAND

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
	/*
	 * Threads maintain the total thread operation and total latency they've
	 * experienced; the monitor thread periodically copies these values into
	 * the last_XXX fields.
	 */
	uint64_t ops;			/* Total operations */
	uint64_t latency_ops;		/* Total ops sampled for latency */
	uint64_t latency;		/* Total latency */

	uint64_t last_latency_ops;	/* Last read by monitor thread */
	uint64_t last_latency;

	/*
	 * Minimum/maximum latency, shared with the monitor thread, that is, the
	 * monitor thread clears it so it's recalculated again for each period.
	 */
	uint32_t min_latency;		/* Minimum latency (uS) */
	uint32_t max_latency;		/* Maximum latency (uS) */

	/*
	 * Latency buckets.
	 */
	uint32_t us[1000];		/* < 1us ... 1000us */
	uint32_t ms[1000];		/* < 1ms ... 1000ms */
	uint32_t sec[100];		/* < 1s 2s ... 100s */
} TRACK;

struct __config_thread {		/* Per-thread structure */
	CONFIG *cfg;			/* Enclosing configuration */

	uint32_t rnd[2];		/* Random number generation state */

	pthread_t handle;		/* Handle */

	char *key_buf, *value_buf;	/* Key/value memory */

	WORKLOAD *workload;		/* Workload */

	TRACK ckpt;			/* Checkpoint operations */
	TRACK insert;			/* Insert operations */
	TRACK read;			/* Read operations */
	TRACK update;			/* Update operations */
};

int	 config_assign(CONFIG *, const CONFIG *);
int	 config_compress(CONFIG *);
void	 config_free(CONFIG *);
int	 config_opt_file(CONFIG *, const char *);
int	 config_opt_line(CONFIG *, const char *);
int	 config_opt_str(CONFIG *, const char *, const char *);
void	 config_print(CONFIG *);
int	 config_sanity(CONFIG *);
void	 latency_insert(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_read(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_update(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_print(CONFIG *);
int	 enomem(const CONFIG *);
int	 setup_log_file(CONFIG *);
uint64_t sum_ckpt_ops(CONFIG *);
uint64_t sum_insert_ops(CONFIG *);
uint64_t sum_pop_ops(CONFIG *);
uint64_t sum_read_ops(CONFIG *);
uint64_t sum_update_ops(CONFIG *);
void	 usage(void);

void	 lprintf(const CONFIG *, int err, uint32_t, const char *, ...)
#if defined(__GNUC__)
__attribute__((format (printf, 4, 5)))
#endif
;
