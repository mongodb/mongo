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

#ifndef HAVE_WTPERF_H
#define HAVE_WTPERF_H

#include "test_util.h"

#include <assert.h>
#include <math.h>

#include "config_opt.h"

typedef struct __wtperf WTPERF;
typedef struct __wtperf_thread WTPERF_THREAD;
typedef struct __truncate_queue_entry TRUNCATE_QUEUE_ENTRY;

#define EXT_PFX ",extensions=("
#define EXT_SFX ")"
#define EXTPATH "../../ext/compressors/" /* Extensions path */
#define BLKCMP_PFX "block_compressor="

#define LZ4_BLK BLKCMP_PFX "lz4"
#define LZ4_EXT EXT_PFX EXTPATH "lz4/.libs/libwiredtiger_lz4.so" EXT_SFX
#define SNAPPY_BLK BLKCMP_PFX "snappy"
#define SNAPPY_EXT EXT_PFX EXTPATH "snappy/.libs/libwiredtiger_snappy.so" EXT_SFX
#define ZLIB_BLK BLKCMP_PFX "zlib"
#define ZLIB_EXT EXT_PFX EXTPATH "zlib/.libs/libwiredtiger_zlib.so" EXT_SFX
#define ZSTD_BLK BLKCMP_PFX "zstd"
#define ZSTD_EXT EXT_PFX EXTPATH "zstd/.libs/libwiredtiger_zstd.so" EXT_SFX

#define MAX_MODIFY_PCT 10
#define MAX_MODIFY_NUM 16

typedef struct {
    int64_t threads;   /* Thread count */
    int64_t insert;    /* Insert ratio */
    int64_t modify;    /* Modify ratio */
    int64_t read;      /* Read ratio */
    int64_t update;    /* Update ratio */
    uint64_t throttle; /* Maximum operations/second */
                       /* Number of operations per transaction. Zero for autocommit */

    int64_t modify_delta;   /* Value size change on modify */
    bool modify_distribute; /* Distribute the change of modifications across the whole new record */
    bool modify_force_update; /* Do force update instead of modify */
    int64_t ops_per_txn;
    int64_t pause;           /* Time between scans */
    int64_t read_range;      /* Range of reads */
    int32_t table_index;     /* Table to focus ops on */
    int64_t truncate;        /* Truncate ratio */
    uint64_t truncate_pct;   /* Truncate Percent */
    uint64_t truncate_count; /* Truncate Count */
    int64_t update_delta;    /* Value size change on update */

#define WORKER_INSERT 1     /* Insert */
#define WORKER_INSERT_RMW 2 /* Insert with read-modify-write */
#define WORKER_MODIFY 3     /* Modify */
#define WORKER_READ 4       /* Read */
#define WORKER_TRUNCATE 5   /* Truncate */
#define WORKER_UPDATE 6     /* Update */
    uint8_t ops[100];       /* Operation schedule */
} WORKLOAD;

/* Steering items for the truncate workload */
typedef struct {
    uint64_t stone_gap;
    uint64_t needed_stones;
    uint64_t expected_total;
    uint64_t total_inserts;
    uint64_t last_total_inserts;
    uint64_t num_stones;
    uint64_t last_key;
    uint64_t catchup_multiplier;
} TRUNCATE_CONFIG;

/* Queue entry for use with the Truncate Logic */
struct __truncate_queue_entry {
    char *key;     /* Truncation point */
    uint64_t diff; /* Number of items to be truncated*/
    TAILQ_ENTRY(__truncate_queue_entry) q;
};

/* Steering for the throttle configuration */
typedef struct {
    struct timespec last_increment; /* Time that we last added more ops */
    uint64_t ops_count;             /* The number of ops this increment */
    uint64_t ops_per_increment;     /* Ops to add per increment */
    uint64_t usecs_increment;       /* Time interval of each increment */
} THROTTLE_CONFIG;

#define LOG_PARTIAL_CONFIG ",log=(enabled=false)"
#define READONLY_CONFIG ",readonly=true"
struct __wtperf {         /* Per-database structure */
    char *home;           /* WiredTiger home */
    char *monitor_dir;    /* Monitor output dir */
    char *partial_config; /* Config string for partial logging */
    char *reopen_config;  /* Config string for conn reopen */
    char *log_table_uri;  /* URI for log table */
    char **uris;          /* URIs */

    WT_CONNECTION *conn; /* Database connection */

    FILE *logf; /* Logging handle */

    const char *compress_ext;   /* Compression extension for conn */
    const char *compress_table; /* Compression arg to table create */

    WTPERF_THREAD *backupthreads; /* Backup threads */
    WTPERF_THREAD *ckptthreads;   /* Checkpoint threads */
    WTPERF_THREAD *popthreads;    /* Populate threads */
    WTPERF_THREAD *scanthreads;   /* Scan threads */

#define WORKLOAD_MAX 50
    WTPERF_THREAD *workers; /* Worker threads */
    u_int workers_cnt;

    WORKLOAD *workload; /* Workloads */
    u_int workload_cnt;

    /* State tracking variables. */
    uint64_t backup_ops;   /* backup operations */
    uint64_t ckpt_ops;     /* checkpoint operations */
    uint64_t scan_ops;     /* scan operations */
    uint64_t insert_ops;   /* insert operations */
    uint64_t modify_ops;   /* modify operations */
    uint64_t read_ops;     /* read operations */
    uint64_t truncate_ops; /* truncate operations */
    uint64_t update_ops;   /* update operations */

    uint64_t insert_key;         /* insert key */
    uint64_t log_like_table_key; /* used to allocate IDs for log table */

    volatile bool backup;    /* backup in progress */
    volatile bool ckpt;      /* checkpoint in progress */
    volatile bool scan;      /* scan in progress */
    volatile bool error;     /* thread error */
    volatile bool stop;      /* notify threads to stop */
    volatile bool in_warmup; /* running warmup phase */

    volatile bool idle_cycle_run; /* Signal for idle cycle thread */

    volatile uint32_t totalsec; /* total seconds running */

#define CFG_GROW 0x0001     /* There is a grow workload */
#define CFG_SHRINK 0x0002   /* There is a shrink workload */
#define CFG_TRUNCATE 0x0004 /* There is a truncate workload */
    uint32_t flags;         /* flags */

    /* Queue head for use with the Truncate Logic */
    TAILQ_HEAD(__truncate_qh, __truncate_queue_entry) stone_head;

    CONFIG_OPTS *opts; /* Global configuration */
};

#define ELEMENTS(a) (sizeof(a) / sizeof(a[0]))

#define READ_RANGE_OPS 10
#define THROTTLE_OPS 100

#define THOUSAND (1000ULL)
#define MILLION (1000000ULL)
#define BILLION (1000000000ULL)

#define NSEC_PER_SEC BILLION
#define USEC_PER_SEC MILLION
#define MSEC_PER_SEC THOUSAND

#define ns_to_ms(v) ((v) / MILLION)
#define ns_to_sec(v) ((v) / BILLION)
#define ns_to_us(v) ((v) / THOUSAND)

#define us_to_ms(v) ((v) / THOUSAND)
#define us_to_ns(v) ((v)*THOUSAND)
#define us_to_sec(v) ((v) / MILLION)

#define ms_to_ns(v) ((v)*MILLION)
#define ms_to_us(v) ((v)*THOUSAND)
#define ms_to_sec(v) ((v) / THOUSAND)

#define sec_to_ns(v) ((v)*BILLION)
#define sec_to_us(v) ((v)*MILLION)
#define sec_to_ms(v) ((v)*THOUSAND)

typedef struct {
    /*
     * Threads maintain the total thread operation and total latency they've experienced; the
     * monitor thread periodically copies these values into the last_XXX fields.
     */
    uint64_t ops;         /* Total operations */
    uint64_t latency_ops; /* Total ops sampled for latency */
    uint64_t latency;     /* Total latency */

    uint64_t last_latency_ops; /* Last read by monitor thread */
    uint64_t last_latency;

    /*
     * Minimum/maximum latency, shared with the monitor thread, that is, the monitor thread clears
     * it so it's recalculated again for each period.
     */
    uint32_t min_latency; /* Minimum latency (uS) */
    uint32_t max_latency; /* Maximum latency (uS) */

    /*
     * Latency buckets.
     */
    uint32_t us[1000]; /* < 1us ... 1000us */
    uint32_t ms[1000]; /* < 1ms ... 1000ms */
    uint32_t sec[100]; /* < 1s 2s ... 100s */
} TRACK;

struct __wtperf_thread {    /* Per-thread structure */
    WTPERF *wtperf;         /* Enclosing configuration */
    WT_CURSOR *rand_cursor; /* Random key cursor */

    WT_RAND_STATE rnd; /* Random number generation state */

    wt_thread_t handle; /* Handle */

    char *key_buf, *value_buf; /* Key/value memory */

    WORKLOAD *workload; /* Workload */

    THROTTLE_CONFIG throttle_cfg; /* Throttle configuration */

    TRUNCATE_CONFIG trunc_cfg; /* Truncate configuration */

    TRACK backup;         /* Backup operations */
    TRACK ckpt;           /* Checkpoint operations */
    TRACK insert;         /* Insert operations */
    TRACK modify;         /* Modify operations */
    TRACK read;           /* Read operations */
    TRACK scan;           /* Scan operations */
    TRACK truncate;       /* Truncate operations */
    TRACK truncate_sleep; /* Truncate sleep operations */
    TRACK update;         /* Update operations */
};

void backup_read(WTPERF *, const char *);
void cleanup_truncate_config(WTPERF *);
int config_opt_file(WTPERF *, const char *);
void config_opt_cleanup(CONFIG_OPTS *);
void config_opt_init(CONFIG_OPTS **);
void config_opt_log(CONFIG_OPTS *, const char *);
int config_opt_name_value(WTPERF *, const char *, const char *);
void config_opt_print(WTPERF *);
int config_opt_str(WTPERF *, const char *);
void config_opt_usage(void);
int config_sanity(WTPERF *);
void latency_insert(WTPERF *, uint32_t *, uint32_t *, uint32_t *);
void latency_modify(WTPERF *, uint32_t *, uint32_t *, uint32_t *);
void latency_print(WTPERF *);
void latency_read(WTPERF *, uint32_t *, uint32_t *, uint32_t *);
void latency_update(WTPERF *, uint32_t *, uint32_t *, uint32_t *);
int run_truncate(WTPERF *, WTPERF_THREAD *, WT_CURSOR *, WT_SESSION *, int *);
int setup_log_file(WTPERF *);
void setup_throttle(WTPERF_THREAD *);
void setup_truncate(WTPERF *, WTPERF_THREAD *, WT_SESSION *);
void start_idle_table_cycle(WTPERF *, wt_thread_t *);
void stop_idle_table_cycle(WTPERF *, wt_thread_t);
void worker_throttle(WTPERF_THREAD *);
uint64_t sum_backup_ops(WTPERF *);
uint64_t sum_ckpt_ops(WTPERF *);
uint64_t sum_scan_ops(WTPERF *);
uint64_t sum_insert_ops(WTPERF *);
uint64_t sum_modify_ops(WTPERF *);
uint64_t sum_pop_ops(WTPERF *);
uint64_t sum_read_ops(WTPERF *);
uint64_t sum_truncate_ops(WTPERF *);
uint64_t sum_update_ops(WTPERF *);

void lprintf(const WTPERF *, int err, uint32_t, const char *, ...)
#if defined(__GNUC__)
  __attribute__((format(printf, 4, 5)))
#endif
  ;

static inline void
generate_key(CONFIG_OPTS *opts, char *key_buf, uint64_t keyno)
{
    u64_to_string_zf(keyno, key_buf, opts->key_sz);
}

static inline void
extract_key(char *key_buf, uint64_t *keynop)
{
    (void)sscanf(key_buf, "%" SCNu64, keynop);
}

/*
 * die --
 *     Print message and exit on failure.
 */
static inline void die(int, const char *) WT_GCC_FUNC_DECL_ATTRIBUTE((noreturn));
static inline void
die(int e, const char *str)
{
    fprintf(stderr, "Call to %s failed: %s", str, wiredtiger_strerror(e));
    exit(EXIT_FAILURE);
}
#endif
