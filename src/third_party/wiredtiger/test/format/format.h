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
#ifndef EXT_LIBPATH
#define EXT_LIBPATH ".libs/"
#endif

#define LZ4_PATH EXTPATH "compressors/lz4/" EXT_LIBPATH "libwiredtiger_lz4.so"

#define SNAPPY_PATH EXTPATH "compressors/snappy/" EXT_LIBPATH "libwiredtiger_snappy.so"

#define ZLIB_PATH EXTPATH "compressors/zlib/" EXT_LIBPATH "libwiredtiger_zlib.so"

#define ZSTD_PATH EXTPATH "compressors/zstd/" EXT_LIBPATH "libwiredtiger_zstd.so"

#define REVERSE_PATH EXTPATH "collators/reverse/" EXT_LIBPATH "libwiredtiger_reverse_collator.so"

#define ROTN_PATH EXTPATH "encryptors/rotn/" EXT_LIBPATH "libwiredtiger_rotn.so"

#define SODIUM_PATH EXTPATH "encryptors/sodium/" EXT_LIBPATH "libwiredtiger_sodium.so"

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

/* Format isn't careful about path buffers, an easy to fix hard-coded length. */
#define MAX_FORMAT_PATH 1024

#define BACKUP_INFO_FILE "BACKUP_INFO"         /* Format's backup information for restart */
#define BACKUP_INFO_FILE_TMP "BACKUP_INFO.TMP" /* Format's backup information for restart */
#define BACKUP_MAX_COPY MEGABYTE(64)           /* Maximum size we'll read/write at a time */

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

#include "config.h"
extern CONFIG configuration_list[];

typedef struct {
    uint32_t v; /* integral value */
    char *vstr; /* string value */
    bool set;   /* value explicitly set */
} CONFIGV;

typedef enum { FIX, ROW, VAR } table_type;
typedef struct {
    u_int id;              /* table ID */
    char uri[32];          /* table URI */
    table_type type;       /* table type */
    char track_prefix[32]; /* table track message prefix */

    uint32_t max_intl_page; /* page size configurations converted to bytes */
    uint32_t max_leaf_page;
    uint32_t max_mem_page;

    uint32_t rows_current; /* current row count */

    uint64_t truncate_cnt; /* truncation operation counter */

    uint32_t key_rand_len[1031]; /* key: lengths */
    char *val_base;              /* value: base/original */
    uint32_t val_dup_data_len;   /* value: length of duplicate data items */

    CONFIGV v[V_ELEMENT_COUNT]; /* table configuration */
} TABLE;

/*
 * We read the configuration in a single pass, which means we don't know the table count until the
 * end, and it can be extended at any time. Start out with a single table, which contains all of the
 * global/default values, stored in the first slot of the tables array. If tables are added during
 * configuration, they are separately allocated, but we continue to use the first (base) table slot
 * for non-specific table or global configurations. In other words, the base information and the
 * only table's information are both in tables' slot 0 to start. If additional tables are
 * configured, the per-table information for each table is stored in tables slots 1-N. The number of
 * tables starts at 0, and if any tables are configured, it's incremented: in other words, if the
 * number of tables is 0, all of the information is in tables' slot 0. If the number of tables is
 * greater than 1, all of the base information is in tables slot 0, and tables slot 1 holds table
 * #1's specific information, slot #2 holds table #2's specific information and so on. This allows
 * general and table-specific information to be configured in any order, and as part of the final
 * table configuration, if there's more than a single table, the information in tables' slot 0 is
 * propagated out to the additional table slots.
 */
extern TABLE *tables[V_MAX_TABLES_CONFIG + 1]; /* Table array */
extern u_int ntables;

/*
 * Global and table-specific macros to retrieve configuration information. All of the tables contain
 * all of the possible configuration entries, but the first table slot contains all of the global
 * configuration information. The offset names a prefixed with "V_GLOBAL" and "V_TABLE" to reduce
 * the chance of a coding error retrieving the wrong configuration item. If returning string values,
 * convert NULL, where a configuration has never been set, to "off" for consistency.
 */
#define GV(off) (tables[0]->v[V_GLOBAL_##off].v)
#define GVS(off) \
    (tables[0]->v[V_GLOBAL_##off].vstr == NULL ? "off" : tables[0]->v[V_GLOBAL_##off].vstr)
#define TV(off) (table->v[V_TABLE_##off].v)
#define TVS(off) (table->v[V_TABLE_##off].vstr == NULL ? "off" : table->v[V_TABLE_##off].vstr)

#define DATASOURCE(table, ds) (strcmp((table)->v[V_TABLE_RUNS_SOURCE].vstr, ds) == 0)

typedef struct {
    WT_CONNECTION *wts_conn;
    WT_CONNECTION *wts_conn_inmemory;
    WT_SESSION *wts_session;

    bool backward_compatible; /* Backward compatibility testing */
    bool configured;          /* Configuration completed */
    bool reopen;              /* Reopen an existing database */
    bool workers_finished;    /* Operations completed */

    char *home;          /* Home directory */
    char *home_config;   /* Run CONFIG file path */
    char *home_hsdump;   /* HS dump filename */
    char *home_key;      /* Key file filename */
    char *home_pagedump; /* Page dump filename */
    char *home_stats;    /* Statistics file path */

    char *config_open; /* Command-line configuration */

    bool trace;                /* trace operations  */
    bool trace_all;            /* trace all operations  */
    bool trace_local;          /* write trace to the primary database */
    char tidbuf[128];          /* thread ID in printable form */
    WT_CONNECTION *trace_conn; /* optional tracing database */
    WT_SESSION *trace_session;

    RWLOCK backup_lock; /* Backup running */
    uint64_t backup_id; /* Block incremental id */
#define INCREMENTAL_BLOCK 1
#define INCREMENTAL_LOG 2
#define INCREMENTAL_OFF 3
    u_int backup_incr_flag; /* Incremental backup configuration */

    WT_RAND_STATE rnd; /* Global RNG state */

    u_int rts_no_check; /* track unsuccessful RTS checking */

    uint64_t timestamp;        /* Counter for timestamps */
    uint64_t oldest_timestamp; /* Last timestamp used for oldest */
    uint64_t stable_timestamp; /* Last timestamp used for stable */

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

    /*
     * Single-thread failure. Not a WiredTiger library lock because it's set up before configuring
     * anything.
     */
    pthread_rwlock_t death_lock;

    WT_CURSOR *page_dump_cursor; /* Snapshot isolation read failed, modifies failure handling. */

    /* Any runs.type configuration. */
    char runs_type[64];

    /*
     * The minimum key size: A minimum key size of 11 is necessary, row-store keys have a leading
     * 10-digit number and the 11 guarantees we never see a key we can't immediately convert to a
     * numeric value without modification (there's a trailing non-digit character after every key).
     *
     * Range of common key prefix selection and the maximum table prefix length.
     */
#define KEY_LEN_CONFIG_MIN 11
#define PREFIX_LEN_CONFIG_MIN 15
#define PREFIX_LEN_CONFIG_MAX 80
    uint32_t prefix_len_max;

    bool column_store_config;           /* At least one column-store table configured */
    bool lsm_config;                    /* At least one LSM data source configured */
    bool multi_table_config;            /* If configuring multiple tables */
    bool transaction_timestamps_config; /* If transaction timestamps configured on any table */

#define CHECKPOINT_OFF 1
#define CHECKPOINT_ON 2
#define CHECKPOINT_WIREDTIGER 3
    u_int checkpoint_config; /* Checkpoint configuration */
} GLOBAL;
extern GLOBAL g;

/* Worker thread operations. */
typedef enum { INSERT = 1, MODIFY, READ, REMOVE, TRUNCATE, UPDATE } thread_op;

/* Worker read operations. */
typedef enum { NEXT, PREV, SEARCH, SEARCH_NEAR } read_operation;

typedef struct {
    thread_op op;  /* Operation */
    uint64_t opid; /* Operation ID */

    uint32_t id;    /* Table ID */
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
    WT_CURSOR **cursors; /* WiredTiger cursors, maps one-to-one to tables */
    WT_CURSOR *cursor;   /* Current cursor */
    TABLE *table;        /* Current table */

    struct col_insert {
        uint32_t insert_list[256]; /* Inserted column-store records, maps one-to-one to tables */
        u_int insert_list_cnt;
    } * col_insert;

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
void config_print(bool);
void config_run(void);
void config_single(TABLE *, const char *, bool);
void create_database(const char *home, WT_CONNECTION **connp);
void fclose_and_clear(FILE **);
bool fp_readv(FILE *, char *, uint32_t *);
void key_gen_common(TABLE *, WT_ITEM *, uint64_t, const char *);
void key_gen_init(WT_ITEM *);
void key_gen_teardown(WT_ITEM *);
void key_init(TABLE *, void *);
void lock_destroy(WT_SESSION *, RWLOCK *);
void lock_init(WT_SESSION *, RWLOCK *);
uint64_t maximum_read_ts(void);
void operations(u_int, bool);
void path_setup(const char *);
void set_alarm(u_int);
void set_core_off(void);
void set_oldest_timestamp(void);
void snap_init(TINFO *);
void snap_op_init(TINFO *, uint64_t, bool);
void snap_repeat_rollback(TINFO **, size_t);
void snap_repeat_single(TINFO *);
int snap_repeat_txn(TINFO *);
void snap_repeat_update(TINFO *, bool);
void snap_teardown(TINFO *);
void snap_track(TINFO *, thread_op);
void timestamp_init(void);
void timestamp_once(WT_SESSION *, bool, bool);
void timestamp_teardown(WT_SESSION *);
void trace_config(const char *);
void trace_init(void);
void trace_ops_init(TINFO *);
void trace_teardown(void);
void track(const char *, uint64_t);
void track_ops(TINFO *);
void val_gen(TABLE *, WT_RAND_STATE *, WT_ITEM *, uint64_t);
void val_gen_init(WT_ITEM *);
void val_gen_teardown(WT_ITEM *);
void val_init(TABLE *, void *);
void wts_checkpoints(void);
void wts_close(WT_CONNECTION **, WT_SESSION **);
void wts_create_database(void);
void wts_create_home(void);
void wts_dump(const char *, bool);
void wts_load(TABLE *, void *);
void wts_open(const char *, WT_CONNECTION **, WT_SESSION **, bool);
void wts_read_scan(TABLE *, void *);
void wts_reopen(void);
void wts_salvage(TABLE *, void *);
void wts_stats(void);
void wts_verify(TABLE *, void *);

/* Backward compatibility to older versions of the WiredTiger library. */
#if !defined(CUR2S)
#define CUR2S(c) ((WT_SESSION_IMPL *)((WT_CURSOR *)c)->session)
#endif

#define WARN(fmt, ...) fprintf(stderr, "%s: WARNING: " fmt "\n", progname, __VA_ARGS__);

#include "format.i"
