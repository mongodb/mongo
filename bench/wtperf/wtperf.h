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

#ifndef	HAVE_WTPERF_H
#define	HAVE_WTPERF_H

#include <wt_internal.h>
#include <assert.h>
#include <math.h>

#ifdef _WIN32
#include "windows_shim.h"
#endif

#include "config_opt.h"

typedef struct __config CONFIG;
typedef struct __config_thread CONFIG_THREAD;
typedef struct __truncate_queue_entry TRUNCATE_QUEUE_ENTRY;

#define	EXT_PFX	",extensions=("
#define	EXT_SFX	")"
#define	EXTPATH "../../ext/compressors/"		/* Extensions path */
#define	BLKCMP_PFX	",block_compressor="

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
	uint64_t throttle;		/* Maximum operations/second */
		/* Number of operations per transaction. Zero for autocommit */
	int64_t ops_per_txn;
	int64_t truncate;		/* Truncate ratio */
	uint64_t truncate_pct;		/* Truncate Percent */
	uint64_t truncate_count;	/* Truncate Count */
	int64_t update_delta;		/* Value size change on update */

#define	WORKER_INSERT		1	/* Insert */
#define	WORKER_INSERT_RMW	2	/* Insert with read-modify-write */
#define	WORKER_READ		3	/* Read */
#define	WORKER_TRUNCATE		4	/* Truncate */
#define	WORKER_UPDATE		5	/* Update */
	uint8_t ops[100];		/* Operation schedule */
} WORKLOAD;

/* Steering items for the truncate workload */
typedef struct {
	uint64_t stone_gap;
	uint64_t needed_stones;
	uint64_t final_stone_gap;
	uint64_t expected_total;
	uint64_t total_inserts;
	uint64_t last_total_inserts;
	uint64_t num_stones;
	uint64_t last_key;
	uint64_t catchup_multiplier;
} TRUNCATE_CONFIG;

/* Queue entry for use with the Truncate Logic */
struct __truncate_queue_entry {
	char *key;			/* Truncation point */
	uint64_t diff;			/* Number of items to be truncated*/
	TAILQ_ENTRY(__truncate_queue_entry) q;
};

struct __config_queue_entry {
	char *string;
	TAILQ_ENTRY(__config_queue_entry) c;
};
typedef struct __config_queue_entry CONFIG_QUEUE_ENTRY;

/* Steering for the throttle configuration */
typedef struct {
	struct timespec last_increment;	/* Time that we last added more ops */
	uint64_t ops_count;		/* The number of ops this increment */
	uint64_t ops_per_increment;	/* Ops to add per increment */
	uint64_t usecs_increment;	/* Time interval of each increment */
} THROTTLE_CONFIG;

#define	LOG_PARTIAL_CONFIG	",log=(enabled=false)"
#define	READONLY_CONFIG		",readonly=true"
/*
 * NOTE:  If you add any fields to this structure here, you must also add
 * an initialization in wtperf.c in the default_cfg.
 */
struct __config {			/* Configuration structure */
	const char *home;		/* WiredTiger home */
	const char *monitor_dir;	/* Monitor output dir */
	char *partial_config;		/* Config string for partial logging */
	char *reopen_config;		/* Config string for conn reopen */
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
	uint64_t truncate_ops;		/* truncate operations */
	uint64_t update_ops;		/* update operations */

	uint64_t insert_key;		/* insert key */

	volatile int ckpt;		/* checkpoint in progress */
	volatile int error;		/* thread error */
	volatile int stop;		/* notify threads to stop */
	volatile int in_warmup;		/* Running warmup phase */

	volatile bool idle_cycle_run;	/* Signal for idle cycle thread */

	volatile uint32_t totalsec;	/* total seconds running */

#define	CFG_GROW	0x0001		/* There is a grow workload */
#define	CFG_SHRINK	0x0002		/* There is a shrink workload */
#define	CFG_TRUNCATE	0x0004		/* There is a truncate workload */
	uint32_t	flags;		/* flags */

	/* Queue head for use with the Truncate Logic */
	TAILQ_HEAD(__truncate_qh, __truncate_queue_entry) stone_head;

	/* Queue head to save a copy of the config to be output */
	TAILQ_HEAD(__config_qh, __config_queue_entry) config_head;

	/* Fields changeable on command line are listed in wtperf_opt.i */
#define	OPT_DECLARE_STRUCT
#include "wtperf_opt.i"
#undef OPT_DECLARE_STRUCT
};

#define	ELEMENTS(a)	(sizeof(a) / sizeof(a[0]))

#define	READ_RANGE_OPS	10
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

	WT_RAND_STATE rnd;		/* Random number generation state */

	pthread_t handle;		/* Handle */

	char *key_buf, *value_buf;	/* Key/value memory */

	WORKLOAD *workload;		/* Workload */

	THROTTLE_CONFIG throttle_cfg;   /* Throttle configuration */

	TRUNCATE_CONFIG trunc_cfg;      /* Truncate configuration */

	TRACK ckpt;			/* Checkpoint operations */
	TRACK insert;			/* Insert operations */
	TRACK read;			/* Read operations */
	TRACK update;			/* Update operations */
	TRACK truncate;			/* Truncate operations */
	TRACK truncate_sleep;		/* Truncate sleep operations */
};

void	 cleanup_truncate_config(CONFIG *);
int	 config_assign(CONFIG *, const CONFIG *);
int	 config_compress(CONFIG *);
void	 config_free(CONFIG *);
int	 config_opt_file(CONFIG *, const char *);
int	 config_opt_line(CONFIG *, const char *);
int	 config_opt_str(CONFIG *, const char *, const char *);
void	 config_to_file(CONFIG *);
void	 config_consolidate(CONFIG *);
void	 config_print(CONFIG *);
int	 config_sanity(CONFIG *);
void	 latency_insert(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_read(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_update(CONFIG *, uint32_t *, uint32_t *, uint32_t *);
void	 latency_print(CONFIG *);
int	 run_truncate(
    CONFIG *, CONFIG_THREAD *, WT_CURSOR *, WT_SESSION *, int *);
int	 setup_log_file(CONFIG *);
int	 setup_throttle(CONFIG_THREAD*);
int	 setup_truncate(CONFIG *, CONFIG_THREAD *, WT_SESSION *);
int	 start_idle_table_cycle(CONFIG *, pthread_t *);
int	 stop_idle_table_cycle(CONFIG *, pthread_t);
uint64_t sum_ckpt_ops(CONFIG *);
uint64_t sum_insert_ops(CONFIG *);
uint64_t sum_pop_ops(CONFIG *);
uint64_t sum_read_ops(CONFIG *);
uint64_t sum_truncate_ops(CONFIG *);
uint64_t sum_update_ops(CONFIG *);
void	 usage(void);
int	 worker_throttle(CONFIG_THREAD*);

void	 lprintf(const CONFIG *, int err, uint32_t, const char *, ...)
#if defined(__GNUC__)
__attribute__((format (printf, 4, 5)))
#endif
;

static inline void
generate_key(CONFIG *cfg, char *key_buf, uint64_t keyno)
{
	/*
	 * Don't change to snprintf, sprintf is faster in some tests.
	 */
	sprintf(key_buf, "%0*" PRIu64, cfg->key_sz - 1, keyno);
}

static inline void
extract_key(char *key_buf, uint64_t *keynop)
{
	(void)sscanf(key_buf, "%" SCNu64, keynop);
}

/*
 * die --
 *      Print message and exit on failure.
 */
static inline void
die(int, const char *)
    WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static inline void
die(int e, const char *str)
{
	fprintf(stderr, "Call to %s failed: %s", str, wiredtiger_strerror(e));
	exit(EXIT_FAILURE);
}

/*
 * dmalloc --
 *      Call malloc, dying on failure.
 */
static inline void *
dmalloc(size_t len)
{
	void *p;

	if ((p = malloc(len)) == NULL)
		die(errno, "malloc");
	return (p);
}

/*
 * dcalloc --
 *      Call calloc, dying on failure.
 */
static inline void *
dcalloc(size_t num, size_t size)
{
	void *p;

	if ((p = calloc(num, size)) == NULL)
		die(errno, "calloc");
	return (p);
}

/*
 * drealloc --
 *      Call realloc, dying on failure.
 */
static inline void *
drealloc(void *p, size_t len)
{
	void *repl;

	if ((repl = realloc(p, len)) == NULL)
		die(errno, "realloc");
	return (repl);
}

/*
 * dstrdup --
 *      Call strdup, dying on failure.
 */
static inline char *
dstrdup(const char *str)
{
	char *p;

	if ((p = strdup(str)) == NULL)
		die(errno, "strdup");
	return (p);
}

/*
 * dstrndup --
 *      Call emulating strndup, dying on failure. Don't use actual strndup here
 *	as it is not supported within MSVC.
 */
static inline char *
dstrndup(const char *str, const size_t len)
{
	char *p;

	p = dcalloc(len + 1, sizeof(char));
	memcpy(p, str, len);
	return (p);
}
#endif
