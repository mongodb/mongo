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

#include "test_util.h"

#ifdef HAVE_SETRLIMIT
#include <sys/resource.h>
#endif
#include <signal.h>

#define EXTPATH "../../ext/" /* Extensions path */

#ifndef LZ4_PATH
#define LZ4_PATH EXTPATH "compressors/lz4/.libs/libwiredtiger_lz4.so"
#endif

#ifndef SNAPPY_PATH
#define SNAPPY_PATH EXTPATH "compressors/snappy/.libs/libwiredtiger_snappy.so"
#endif

#ifndef ZLIB_PATH
#define ZLIB_PATH EXTPATH "compressors/zlib/.libs/libwiredtiger_zlib.so"
#endif

#ifndef ZSTD_PATH
#define ZSTD_PATH EXTPATH "compressors/zstd/.libs/libwiredtiger_zstd.so"
#endif

#ifndef REVERSE_PATH
#define REVERSE_PATH EXTPATH "collators/reverse/.libs/libwiredtiger_reverse_collator.so"
#endif

#ifndef ROTN_PATH
#define ROTN_PATH EXTPATH "encryptors/rotn/.libs/libwiredtiger_rotn.so"
#endif

#ifndef SODIUM_PATH
#define SODIUM_PATH EXTPATH "encryptors/sodium/.libs/libwiredtiger_sodium.so"
#endif

/*
 * To test the sodium encryptor, we use secretkey= rather than setting a keyid, because for a "real"
 * (vs. test-only) encryptor, keyids require some kind of key server, and (a) setting one up for
 * testing would be a nuisance and (b) currently the sodium encryptor doesn't support any anyway.
 *
 * It expects secretkey= to provide a hex-encoded 256-bit chacha20 key. This key will serve for
 * testing purposes.
 */
#define SODIUM_TESTKEY "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"

#undef M
#define M(v) ((v)*WT_MILLION) /* Million */
#undef KILOBYTE
#define KILOBYTE(v) ((v)*WT_KILOBYTE)
#undef MEGABYTE
#define MEGABYTE(v) ((v)*WT_MEGABYTE)

#define BACKUP_INFO_FILE "BACKUP_INFO"         /* Format's backup information for restart */
#define BACKUP_INFO_FILE_TMP "BACKUP_INFO.TMP" /* Format's backup information for restart */
#define BACKUP_MAX_COPY MEGABYTE(64)           /* Maximum size we'll read/write at a time */
#define WT_NAME "wt"                           /* Object name */

#define DATASOURCE(v) (strcmp(v, g.c_data_source) == 0 ? 1 : 0)

#define FORMAT_OPERATION_REPS 3 /* 3 thread operations sets */

#define MAX_MODIFY_ENTRIES 5 /* maximum change vectors */

/*
 * Abstract lock that lets us use either pthread reader-writer locks or WiredTiger's own (likely
 * faster) implementation.
 */
typedef struct {
    union {
        WT_RWLOCK wt;
        pthread_rwlock_t pthread;
    } l;
    enum { LOCK_NONE = 0, LOCK_WT, LOCK_PTHREAD } lock_type;
} RWLOCK;

#define LOCK_INITIALIZED(lock) ((lock)->lock_type != LOCK_NONE)

typedef struct {
    char tidbuf[128]; /* thread ID in printable form */

    WT_CONNECTION *wts_conn;
    WT_CONNECTION *wts_conn_inmemory;
    WT_SESSION *wts_session;

    char *uri; /* Object name */

    bool backward_compatible; /* Backward compatibility testing */
    bool reopen;              /* Reopen an existing database */
    bool workers_finished;    /* Operations completed */

    char *home;          /* Home directory */
    char *home_config;   /* Run CONFIG file path */
    char *home_hsdump;   /* HS dump filename */
    char *home_init;     /* Initialize home command */
    char *home_key;      /* Key file filename */
    char *home_log;      /* Operation log file path */
    char *home_pagedump; /* Page dump filename */
    char *home_rand;     /* RNG log file path */
    char *home_stats;    /* Statistics file path */

    char *config_open; /* Command-line configuration */

    uint32_t run_cnt; /* Run counter */

    bool trace;                /* trace operations  */
    bool trace_all;            /* trace all operations  */
    bool trace_local;          /* write trace to the primary database */
    WT_CONNECTION *trace_conn; /* optional tracing database */
    WT_SESSION *trace_session;

    RWLOCK backup_lock; /* Backup running */
    uint64_t backup_id; /* Block incremental id */

    WT_RAND_STATE rnd; /* Global RNG state */

    uint32_t rts_no_check; /* track unsuccessful RTS checking */

    /*
     * Prepare will return an error if the prepare timestamp is less than any active read timestamp.
     * Lock across allocating prepare and read timestamps.
     *
     * We get the last committed timestamp periodically in order to update the oldest timestamp,
     * that requires locking out transactional ops that set a timestamp.
     */
    RWLOCK ts_lock;
    /*
     * Lock to prevent the stable timestamp from moving during the commit of prepared transactions.
     * Otherwise, it may panic if the stable timestamp is moved to greater than or equal to the
     * prepared transaction's durable timestamp when it is committing.
     */
    RWLOCK prepare_commit_lock;

    uint64_t timestamp;        /* Counter for timestamps */
    uint64_t oldest_timestamp; /* Last timestamp used for oldest */
    uint64_t stable_timestamp; /* Last timestamp used for stable */

    uint64_t truncate_cnt; /* Counter for truncation */

    /*
     * Single-thread failure. Always use pthread lock rather than WT lock in case WT library is
     * misbehaving.
     */
    pthread_rwlock_t death_lock;
    WT_CURSOR *page_dump_cursor; /* Snapshot isolation read failed, modifies failure handling. */

    uint32_t c_abort; /* Config values */
    uint32_t c_alter;
    uint32_t c_assert_read_timestamp;
    uint32_t c_assert_write_timestamp;
    uint32_t c_auto_throttle;
    char *c_backup_incremental;
    uint32_t c_backup_incr_granularity;
    uint32_t c_backups;
    uint32_t c_bitcnt;
    uint32_t c_block_cache;
    uint32_t c_block_cache_cache_on_checkpoint;
    uint32_t c_block_cache_cache_on_writes;
    uint32_t c_block_cache_size;
    uint32_t c_bloom;
    uint32_t c_bloom_bit_count;
    uint32_t c_bloom_hash_count;
    uint32_t c_bloom_oldest;
    uint32_t c_cache;
    uint32_t c_cache_minimum;
    char *c_checkpoint;
    uint32_t c_checkpoint_log_size;
    uint32_t c_checkpoint_wait;
    char *c_checksum;
    uint32_t c_chunk_size;
    uint32_t c_compact;
    char *c_compression;
    char *c_config_open;
    uint32_t c_data_extend;
    char *c_data_source;
    uint32_t c_delete_pct;
    uint32_t c_dictionary;
    uint32_t c_direct_io;
    char *c_encryption;
    uint32_t c_evict_max;
    char *c_file_type;
    uint32_t c_firstfit;
    uint32_t c_hs_cursor;
    uint32_t c_huffman_value;
    uint32_t c_import;
    uint32_t c_in_memory;
    uint32_t c_independent_thread_rng;
    uint32_t c_insert_pct;
    uint32_t c_internal_key_truncation;
    uint32_t c_intl_page_max;
    uint32_t c_key_max;
    uint32_t c_key_min;
    uint32_t c_leaf_page_max;
    uint32_t c_leak_memory;
    uint32_t c_logging;
    uint32_t c_logging_archive;
    char *c_logging_compression;
    uint32_t c_logging_file_max;
    uint32_t c_logging_prealloc;
    uint32_t c_lsm_worker_threads;
    uint32_t c_major_timeout;
    uint32_t c_memory_page_max;
    uint32_t c_merge_max;
    uint32_t c_mmap;
    uint32_t c_mmap_all;
    uint32_t c_modify_pct;
    uint32_t c_ops;
    uint32_t c_prefix;
    uint32_t c_prefix_compression;
    uint32_t c_prefix_compression_min;
    uint32_t c_prepare;
    uint32_t c_quiet;
    uint32_t c_random_cursor;
    uint32_t c_read_pct;
    uint32_t c_repeat_data_pct;
    uint32_t c_reverse;
    uint32_t c_rows;
    uint32_t c_runs;
    uint32_t c_salvage;
    uint32_t c_split_pct;
    uint32_t c_statistics;
    uint32_t c_statistics_server;
    uint32_t c_threads;
    uint32_t c_timer;
    uint32_t c_timing_stress_aggressive_sweep;
    uint32_t c_timing_stress_checkpoint;
    uint32_t c_timing_stress_checkpoint_reserved_txnid_delay;
    uint32_t c_timing_stress_failpoint_hs_delete_key_from_ts;
    uint32_t c_timing_stress_failpoint_hs_insert_1;
    uint32_t c_timing_stress_failpoint_hs_insert_2;
    uint32_t c_timing_stress_hs_checkpoint_delay;
    uint32_t c_timing_stress_hs_search;
    uint32_t c_timing_stress_hs_sweep;
    uint32_t c_timing_stress_checkpoint_prepare;
    uint32_t c_timing_stress_split_1;
    uint32_t c_timing_stress_split_2;
    uint32_t c_timing_stress_split_3;
    uint32_t c_timing_stress_split_4;
    uint32_t c_timing_stress_split_5;
    uint32_t c_timing_stress_split_6;
    uint32_t c_timing_stress_split_7;
    uint32_t c_truncate;
    uint32_t c_txn_implicit;
    uint32_t c_txn_timestamps;
    uint32_t c_value_max;
    uint32_t c_value_min;
    uint32_t c_verify;
    uint32_t c_verify_failure_dump;
    uint32_t c_write_pct;
    uint32_t c_wt_mutex;

#define FIX 1
#define ROW 2
#define VAR 3
    u_int type; /* File type's flag value */

#define INCREMENTAL_BLOCK 1
#define INCREMENTAL_LOG 2
#define INCREMENTAL_OFF 3
    u_int c_backup_incr_flag; /* Incremental backup flag value */

#define CHECKPOINT_OFF 1
#define CHECKPOINT_ON 2
#define CHECKPOINT_WIREDTIGER 3
    u_int c_checkpoint_flag; /* Checkpoint flag value */

#define CHECKSUM_OFF 1
#define CHECKSUM_ON 2
#define CHECKSUM_UNCOMPRESSED 3
#define CHECKSUM_UNENCRYPTED 4
    u_int c_checksum_flag; /* Checksum flag value */

#define COMPRESS_NONE 1
#define COMPRESS_LZ4 2
#define COMPRESS_SNAPPY 3
#define COMPRESS_ZLIB 4
#define COMPRESS_ZSTD 5
    u_int c_compression_flag;         /* Compression flag value */
    u_int c_logging_compression_flag; /* Log compression flag value */

#define ENCRYPT_NONE 1
#define ENCRYPT_ROTN_7 2
#define ENCRYPT_SODIUM 3
    u_int c_encryption_flag; /* Encryption flag value */

/* The page must be a multiple of the allocation size, and 512 always works. */
#define BLOCK_ALLOCATION_SIZE 512
    uint32_t intl_page_max; /* Maximum page sizes */
    uint32_t leaf_page_max;

    uint64_t rows; /* Total rows */

    uint32_t prefix_len;         /* Common key prefix length */
    uint32_t key_rand_len[1031]; /* Key lengths */
} GLOBAL;
extern GLOBAL g;

/* Worker thread operations. */
typedef enum { INSERT = 1, MODIFY, READ, REMOVE, TRUNCATE, UPDATE } thread_op;

/* Worker read operations. */
typedef enum { NEXT, PREV, SEARCH, SEARCH_NEAR } read_operation;

typedef struct {
    thread_op op;  /* Operation */
    uint64_t opid; /* Operation ID */

    uint64_t keyno; /* Row number */

    uint64_t ts;     /* Read/commit timestamp */
    bool repeatable; /* Operation can be repeated */

    uint64_t last; /* Inclusive end of a truncate range */

    void *kdata; /* If an insert, the generated key */
    size_t ksize;
    size_t kmemsize;

    void *vdata; /* If not a delete, the value */
    size_t vsize;
    size_t vmemsize;
} SNAP_OPS;

typedef struct {
    SNAP_OPS *snap_state_current;
    SNAP_OPS *snap_state_end;
    SNAP_OPS *snap_state_first;
    SNAP_OPS *snap_state_list;
} SNAP_STATE;

typedef struct {
    int id;           /* simple thread ID */
    wt_thread_t tid;  /* thread ID */
    char tidbuf[128]; /* thread ID in printable form */

    WT_RAND_STATE rnd; /* thread RNG state */

    volatile bool quit; /* thread should quit */

    uint64_t ops;    /* total operations */
    uint64_t commit; /* operation counts */
    uint64_t insert;
    uint64_t prepare;
    uint64_t remove;
    uint64_t rollback;
    uint64_t search;
    uint64_t truncate;
    uint64_t update;

    WT_SESSION *session; /* WiredTiger session */
    WT_CURSOR *cursor;   /* WiredTiger cursor */

    WT_SESSION *trace; /* WiredTiger operations tracing session */

    uint64_t keyno;     /* key */
    WT_ITEM *key, _key; /* key, value */
    WT_ITEM *value, _value;

    uint64_t last; /* truncate range */
    WT_ITEM *lastkey, _lastkey;

    bool repeatable_reads; /* if read ops repeatable */
    bool repeatable_wrap;  /* if circular buffer wrapped */
    uint64_t opid;         /* Operation ID */
    uint64_t read_ts;      /* read timestamp */
    uint64_t commit_ts;    /* commit timestamp */
    uint64_t stable_ts;    /* stable timestamp */
    SNAP_STATE snap_states[2];
    SNAP_STATE *s; /* points to one of the snap_states */

#define snap_current s->snap_state_current
#define snap_end s->snap_state_end
#define snap_first s->snap_state_first
#define snap_list s->snap_state_list

    uint64_t insert_list[256]; /* column-store inserted records */
    u_int insert_list_cnt;

    WT_ITEM vprint;     /* Temporary buffer for printable values */
    WT_ITEM moda, modb; /* Temporary buffer for modify operations */

#define TINFO_RUNNING 1  /* Running */
#define TINFO_COMPLETE 2 /* Finished */
#define TINFO_JOINED 3   /* Resolved */
    volatile int state;  /* state */
} TINFO;
extern TINFO **tinfo_list;

#define SNAP_LIST_SIZE 512

WT_THREAD_RET alter(void *);
WT_THREAD_RET backup(void *);
WT_THREAD_RET checkpoint(void *);
WT_THREAD_RET compact(void *);
WT_THREAD_RET hs_cursor(void *);
WT_THREAD_RET import(void *);
WT_THREAD_RET random_kv(void *);
WT_THREAD_RET timestamp(void *);

void config_clear(void);
void config_compat(const char **);
void config_error(void);
void config_file(const char *);
void config_final(void);
void config_print(bool);
void config_run(void);
void config_single(const char *, bool);
void create_database(const char *home, WT_CONNECTION **connp);
void fclose_and_clear(FILE **);
bool fp_readv(FILE *, char *, uint32_t *);
void key_gen_common(WT_ITEM *, uint64_t, const char *);
void key_gen_init(WT_ITEM *);
void key_gen_teardown(WT_ITEM *);
void key_init(void);
void lock_destroy(WT_SESSION *, RWLOCK *);
void lock_init(WT_SESSION *, RWLOCK *);
void operations(u_int, bool);
void path_setup(const char *);
void set_alarm(u_int);
void set_core_off(void);
void set_oldest_timestamp(void);
void snap_init(TINFO *);
void snap_teardown(TINFO *);
void snap_op_init(TINFO *, uint64_t, bool);
void snap_repeat_rollback(WT_CURSOR *, TINFO **, size_t);
void snap_repeat_single(WT_CURSOR *, TINFO *);
int snap_repeat_txn(WT_CURSOR *, TINFO *);
void snap_repeat_update(TINFO *, bool);
void snap_track(TINFO *, thread_op);
void timestamp_init(void);
void timestamp_once(WT_SESSION *, bool, bool);
void timestamp_teardown(WT_SESSION *);
int trace_config(const char *);
void trace_init(void);
void trace_ops_init(TINFO *);
void trace_teardown(void);
void track(const char *, uint64_t, TINFO *);
void val_gen(WT_RAND_STATE *, WT_ITEM *, uint64_t);
void val_gen_init(WT_ITEM *);
void val_gen_teardown(WT_ITEM *);
void val_init(void);
void wts_checkpoints(void);
void wts_close(WT_CONNECTION **, WT_SESSION **);
void wts_create(const char *);
void wts_dump(const char *, bool);
void wts_load(void);
void wts_open(const char *, WT_CONNECTION **, WT_SESSION **, bool);
void wts_read_scan(void);
void wts_reopen(void);
void wts_salvage(void);
void wts_stats(void);
void wts_verify(WT_CONNECTION *, const char *);

#if !defined(CUR2S)
#define CUR2S(c) ((WT_SESSION_IMPL *)((WT_CURSOR *)c)->session)
#endif

#include "format.i"
