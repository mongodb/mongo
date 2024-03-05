/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*******************************************
 * Global per-process structure.
 *******************************************/
/*
 * WT_PROCESS --
 *	Per-process information for the library.
 */
struct __wt_process {
    WT_SPINLOCK spinlock; /* Per-process spinlock */

    /* Locked: connection queue */
    TAILQ_HEAD(__wt_connection_impl_qh, __wt_connection_impl) connqh;

/* Checksum functions */
#define __wt_checksum(chunk, len) __wt_process.checksum(chunk, len)
    uint32_t (*checksum)(const void *, size_t);
#define __wt_checksum_with_seed(seed, chunk, len) __wt_process.checksum_with_seed(seed, chunk, len)
    uint32_t (*checksum_with_seed)(uint32_t, const void *, size_t);

#define WT_TSC_DEFAULT_RATIO 1.0
    double tsc_nsec_ratio; /* rdtsc ticks to nanoseconds */
    bool use_epochtime;    /* use expensive time */

    bool fast_truncate_2022; /* fast-truncate fix run-time configuration */
    bool tiered_shared_2023; /* tiered shared run-time configuration */

    WT_CACHE_POOL *cache_pool; /* shared cache information */

    /*
     * WT_CURSOR.modify operations set unspecified bytes to space in 'S' format and to a nul byte in
     * all other formats. It makes it easier to debug format test program stress failures if strings
     * are printable and don't require encoding to trace them in the log; this is a hook that allows
     * format to set the modify pad byte to a printable character.
     */
    uint8_t modify_pad_byte;
};
extern WT_PROCESS __wt_process;

typedef enum __wt_background_compact_cleanup_stat_type {
    BACKGROUND_CLEANUP_ALL_STAT,
    BACKGROUND_CLEANUP_STALE_STAT
} WT_BACKGROUND_COMPACT_CLEANUP_STAT_TYPE;

/*
 * WT_BACKGROUND_COMPACT_STAT --
 *  List of tracking information for each file compact has worked on.
 */
struct __wt_background_compact_stat {
    const char *uri;
    uint32_t id;                                /* File ID */
    bool prev_compact_success;                  /* Last compact successfully reclaimed space */
    uint64_t prev_compact_time;                 /* Start time for last compact attempt */
    uint64_t skip_count;                        /* Number of times we've skipped this file */
    uint64_t consecutive_unsuccessful_attempts; /* Number of failed attempts since last success */
    uint64_t bytes_rewritten;                   /* Bytes rewritten during last compaction call */

    wt_off_t start_size; /* File size before compact last started */
    wt_off_t end_size;   /* File size after compact last ended */

    /* Hash of files background compact has worked on */
    TAILQ_ENTRY(__wt_background_compact_stat) hashq;
};

/*
 * WT_BACKGROUND_COMPACT_EXCLUDE --
 *	An entry indicating this file should be excluded from background compaction.
 */
struct __wt_background_compact_exclude {
    const char *name; /* File name */

    TAILQ_ENTRY(__wt_background_compact_exclude) hashq; /* internal hash queue */
};

/*
 * WT_BACKGROUND_COMPACT --
 *	Structure dedicated to the background compaction server
 */
struct __wt_background_compact {
    bool running;             /* Compaction supposed to run */
    bool run_once;            /* Background compaction is executed once */
    bool signalled;           /* Compact signalled */
    bool tid_set;             /* Thread set */
    wt_thread_t tid;          /* Thread */
    const char *config;       /* Configuration */
    WT_CONDVAR *cond;         /* Wait mutex */
    WT_SPINLOCK lock;         /* Compact lock */
    WT_SESSION_IMPL *session; /* Thread session */

    uint64_t files_skipped;       /* Number of times background server has skipped a file */
    uint64_t files_compacted;     /* Number of times background server has compacted a file */
    uint64_t file_count;          /* Number of files in the tracking list */
    uint64_t bytes_rewritten_ema; /* Exponential moving average for the bytes rewritten */

    uint64_t max_file_idle_time;       /* File compact idle time */
    uint64_t max_file_skip_time;       /* File compact skip time */
    uint64_t full_iteration_wait_time; /* Time in seconds to wait after a full iteration */

    /* List of files to track compaction statistics across background server iterations. */
    TAILQ_HEAD(__wt_background_compactstathash, __wt_background_compact_stat) * stat_hash;
    /* List of files excluded from background compaction. */
    TAILQ_HEAD(__wt_background_compactexcludelisthash, __wt_background_compact_exclude) *
      exclude_list_hash;
};

/*
 * WT_BUCKET_STORAGE --
 *	A list entry for a storage source with a unique name (bucket, prefix).
 */
struct __wt_bucket_storage {
    const char *bucket;                /* Bucket name */
    const char *bucket_prefix;         /* Bucket prefix */
    const char *cache_directory;       /* Locally cached file location */
    int owned;                         /* Storage needs to be terminated */
    uint64_t retain_secs;              /* Tiered period */
    const char *auth_token;            /* Tiered authentication cookie */
    bool tiered_shared;                /* Tiered shared */
    WT_FILE_SYSTEM *file_system;       /* File system for bucket */
    WT_STORAGE_SOURCE *storage_source; /* Storage source callbacks */
    /* Linked list of bucket storage entries */
    TAILQ_ENTRY(__wt_bucket_storage) hashq;
    TAILQ_ENTRY(__wt_bucket_storage) q;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_BUCKET_FREE 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/* Call a function with the bucket storage and its associated file system. */
#define WT_WITH_BUCKET_STORAGE(bsto, s, e)                                  \
    do {                                                                    \
        WT_BUCKET_STORAGE *__saved_bstorage = (s)->bucket_storage;          \
        (s)->bucket_storage = ((bsto) == NULL ? S2C(s)->bstorage : (bsto)); \
        e;                                                                  \
        (s)->bucket_storage = __saved_bstorage;                             \
    } while (0)

/*
 * WT_KEYED_ENCRYPTOR --
 *	A list entry for an encryptor with a unique (name, keyid).
 */
struct __wt_keyed_encryptor {
    const char *keyid;       /* Key id of encryptor */
    int owned;               /* Encryptor needs to be terminated */
    size_t size_const;       /* The result of the sizing callback */
    WT_ENCRYPTOR *encryptor; /* User supplied callbacks */
    /* Linked list of encryptors */
    TAILQ_ENTRY(__wt_keyed_encryptor) hashq;
    TAILQ_ENTRY(__wt_keyed_encryptor) q;
};

/*
 * WT_NAMED_COLLATOR --
 *	A collator list entry
 */
struct __wt_named_collator {
    const char *name;                   /* Name of collator */
    WT_COLLATOR *collator;              /* User supplied object */
    TAILQ_ENTRY(__wt_named_collator) q; /* Linked list of collators */
};

/*
 * WT_NAMED_COMPRESSOR --
 *	A compressor list entry
 */
struct __wt_named_compressor {
    const char *name;          /* Name of compressor */
    WT_COMPRESSOR *compressor; /* User supplied callbacks */
    /* Linked list of compressors */
    TAILQ_ENTRY(__wt_named_compressor) q;
};

/*
 * WT_NAMED_DATA_SOURCE --
 *	A data source list entry
 */
struct __wt_named_data_source {
    const char *prefix;   /* Name of data source */
    WT_DATA_SOURCE *dsrc; /* User supplied callbacks */
    /* Linked list of data sources */
    TAILQ_ENTRY(__wt_named_data_source) q;
};

/*
 * WT_NAMED_ENCRYPTOR --
 *	An encryptor list entry
 */
struct __wt_named_encryptor {
    const char *name;        /* Name of encryptor */
    WT_ENCRYPTOR *encryptor; /* User supplied callbacks */
    /* Locked: list of encryptors by key */
    TAILQ_HEAD(__wt_keyedhash, __wt_keyed_encryptor) * keyedhashqh;
    TAILQ_HEAD(__wt_keyed_qh, __wt_keyed_encryptor) keyedqh;
    /* Linked list of encryptors */
    TAILQ_ENTRY(__wt_named_encryptor) q;
};

/*
 * WT_NAMED_EXTRACTOR --
 *	An extractor list entry
 */
struct __wt_named_extractor {
    const char *name;                    /* Name of extractor */
    WT_EXTRACTOR *extractor;             /* User supplied object */
    TAILQ_ENTRY(__wt_named_extractor) q; /* Linked list of extractors */
};

/*
 * WT_NAMED_STORAGE_SOURCE --
 *	A storage source list entry
 */
struct __wt_named_storage_source {
    const char *name;                  /* Name of storage source */
    WT_STORAGE_SOURCE *storage_source; /* User supplied callbacks */
    TAILQ_HEAD(__wt_buckethash, __wt_bucket_storage) * buckethashqh;
    TAILQ_HEAD(__wt_bucket_qh, __wt_bucket_storage) bucketqh;
    /* Linked list of storage sources */
    TAILQ_ENTRY(__wt_named_storage_source) q;
};

/*
 * WT_NAME_FLAG --
 *	Simple structure for name and flag configuration searches
 */
struct __wt_name_flag {
    const char *name;
    uint64_t flag;
};

/*
 * WT_CONN_CHECK_PANIC --
 *	Check if we've panicked and return the appropriate error.
 */
#define WT_CONN_CHECK_PANIC(conn) (F_ISSET(conn, WT_CONN_PANIC) ? WT_PANIC : 0)
#define WT_SESSION_CHECK_PANIC(session) WT_CONN_CHECK_PANIC(S2C(session))

/*
 * Macros to ensure the dhandle is inserted or removed from both the main queue and the hashed
 * queue.
 */
#define WT_CONN_DHANDLE_INSERT(conn, dhandle, bucket)                                            \
    do {                                                                                         \
        WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE)); \
        TAILQ_INSERT_HEAD(&(conn)->dhqh, dhandle, q);                                            \
        TAILQ_INSERT_HEAD(&(conn)->dhhash[bucket], dhandle, hashq);                              \
        ++(conn)->dh_bucket_count[bucket];                                                       \
        ++(conn)->dhandle_count;                                                                 \
    } while (0)

#define WT_CONN_DHANDLE_REMOVE(conn, dhandle, bucket)                                            \
    do {                                                                                         \
        WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE)); \
        TAILQ_REMOVE(&(conn)->dhqh, dhandle, q);                                                 \
        TAILQ_REMOVE(&(conn)->dhhash[bucket], dhandle, hashq);                                   \
        --(conn)->dh_bucket_count[bucket];                                                       \
        --(conn)->dhandle_count;                                                                 \
    } while (0)

/*
 * Macros to ensure the block is inserted or removed from both the main queue and the hashed queue.
 */
#define WT_CONN_BLOCK_INSERT(conn, block, bucket)                    \
    do {                                                             \
        TAILQ_INSERT_HEAD(&(conn)->blockqh, block, q);               \
        TAILQ_INSERT_HEAD(&(conn)->blockhash[bucket], block, hashq); \
    } while (0)

#define WT_CONN_BLOCK_REMOVE(conn, block, bucket)               \
    do {                                                        \
        TAILQ_REMOVE(&(conn)->blockqh, block, q);               \
        TAILQ_REMOVE(&(conn)->blockhash[bucket], block, hashq); \
    } while (0)

/*
 * WT_CONN_HOTBACKUP_START --
 *	Macro to set connection data appropriately for when we commence hot backup.
 */
#define WT_CONN_HOTBACKUP_START(conn)                        \
    do {                                                     \
        (conn)->hot_backup_start = (conn)->ckpt_most_recent; \
        (conn)->hot_backup_list = NULL;                      \
    } while (0)

/*
 * Set all flags related to incremental backup in one macro. The flags do get individually cleared
 * at different times so there is no corresponding macro for clearing.
 */
#define WT_CONN_SET_INCR_BACKUP(conn)                        \
    do {                                                     \
        F_SET((conn), WT_CONN_INCR_BACKUP);                  \
        FLD_SET((conn)->log_flags, WT_CONN_LOG_INCR_BACKUP); \
    } while (0)

/*
 * WT_BACKUP_TARGET --
 *	A target URI entry indicating this URI should be restored during a partial backup.
 */
struct __wt_backup_target {
    const char *name; /* File name */

    uint64_t name_hash;                    /* hash of name */
    TAILQ_ENTRY(__wt_backup_target) hashq; /* internal hash queue */
};
typedef TAILQ_HEAD(__wt_backuphash, __wt_backup_target) WT_BACKUPHASH;

extern const WT_NAME_FLAG __wt_stress_types[];

/*
 * Access the array of all sessions. This field uses the Slotted Array pattern to managed shared
 * accesses, if you are looking to walk all sessions please consider using the existing session walk
 * logic. FIXME-WT-10946 - Add link to Slotted Array docs.
 */
#define WT_CONN_SESSIONS_GET(conn) ((conn)->session_array.__array)

/*
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
    WT_CONNECTION iface;

    /* For operations without an application-supplied session */
    wt_shared WT_SESSION_IMPL *default_session;
    WT_SESSION_IMPL dummy_session;

    const char *cfg; /* Connection configuration */

    WT_SPINLOCK api_lock;                 /* Connection API spinlock */
    WT_SPINLOCK checkpoint_lock;          /* Checkpoint spinlock */
    WT_SPINLOCK chunkcache_metadata_lock; /* Chunk cache metadata spinlock */
    WT_RWLOCK debug_log_retention_lock;   /* Log retention reconfiguration lock */
    WT_SPINLOCK fh_lock;                  /* File handle queue spinlock */
    WT_SPINLOCK flush_tier_lock;          /* Flush tier spinlock */
    WT_SPINLOCK metadata_lock;            /* Metadata update spinlock */
    WT_SPINLOCK reconfig_lock;            /* Single thread reconfigure */
    WT_SPINLOCK schema_lock;              /* Schema operation spinlock */
    WT_RWLOCK table_lock;                 /* Table list lock */
    WT_SPINLOCK tiered_lock;              /* Tiered work queue spinlock */
    WT_SPINLOCK turtle_lock;              /* Turtle file spinlock */
    WT_RWLOCK dhandle_lock;               /* Data handle list lock */

    /* Connection queue */
    TAILQ_ENTRY(__wt_connection_impl) q;
    /* Cache pool queue */
    TAILQ_ENTRY(__wt_connection_impl) cpq;

    const char *home;         /* Database home */
    const char *error_prefix; /* Database error prefix */
    uint64_t dh_hash_size;    /* Data handle hash bucket array size */
    uint64_t hash_size;       /* General hash bucket array size */
    int is_new;               /* Connection created database */

    WT_VERSION recovery_version; /* Version of the database being recovered */

#ifndef WT_STANDALONE_BUILD
    bool unclean_shutdown; /* Flag to indicate the earlier shutdown status */
#endif

    WT_VERSION compat_version; /* WiredTiger version for compatibility checks */
    WT_VERSION compat_req_max; /* Maximum allowed version of WiredTiger for compatibility checks */
    WT_VERSION compat_req_min; /* Minimum allowed version of WiredTiger for compatibility checks */

    WT_EXTENSION_API extension_api; /* Extension API */

    /* Configuration */
    wt_shared const WT_CONFIG_ENTRY **config_entries;

    WT_BACKGROUND_COMPACT background_compact; /* Background compaction server */

    uint64_t operation_timeout_us; /* Maximum operation period before rollback */

    const char *optrack_path;         /* Directory for operation logs */
    WT_FH *optrack_map_fh;            /* Name to id translation file. */
    WT_SPINLOCK optrack_map_spinlock; /* Translation file spinlock. */
    uintmax_t optrack_pid;            /* Cache the process ID. */

#ifdef HAVE_CALL_LOG
    /* File stream used for writing to the call log. */
    WT_FSTREAM *call_log_fst;
#endif

    void **foc;      /* Free-on-close array */
    size_t foc_cnt;  /* Array entries */
    size_t foc_size; /* Array size */

    WT_FH *lock_fh; /* Lock file handle */

    /* Locked: chunk cache metadata work queue (and length counter). */
    TAILQ_HEAD(__wt_chunkcache_metadata_qh, __wt_chunkcache_metadata_work_unit)
    chunkcache_metadataqh;
    int chunkcache_queue_len;

    /*
     * The connection keeps a cache of data handles. The set of handles can grow quite large so we
     * maintain both a simple list and a hash table of lists. The hash table key is based on a hash
     * of the table URI.
     */
    /* Locked: data handle hash array */
    TAILQ_HEAD(__wt_dhhash, __wt_data_handle) * dhhash;
    /* Locked: data handle list */
    TAILQ_HEAD(__wt_dhandle_qh, __wt_data_handle) dhqh;
    /* Locked: dynamic library handle list */
    TAILQ_HEAD(__wt_dlh_qh, __wt_dlh) dlhqh;
    /* Locked: file list */
    TAILQ_HEAD(__wt_fhhash, __wt_fh) * fhhash;
    TAILQ_HEAD(__wt_fh_qh, __wt_fh) fhqh;
    /* Locked: LSM handle list. */
    TAILQ_HEAD(__wt_lsm_qh, __wt_lsm_tree) lsmqh;
    /* Locked: Tiered system work queue. */
    TAILQ_HEAD(__wt_tiered_qh, __wt_tiered_work_unit) tieredqh;

    WT_SPINLOCK block_lock; /* Locked: block manager list */
    TAILQ_HEAD(__wt_blockhash, __wt_block) * blockhash;
    TAILQ_HEAD(__wt_block_qh, __wt_block) blockqh;

    WT_BLKCACHE blkcache;     /* Block cache */
    WT_CHUNKCACHE chunkcache; /* Chunk cache */

    /* Locked: handles in each bucket */
    uint64_t *dh_bucket_count;
    uint64_t dhandle_count;                  /* Locked: handles in the queue */
    u_int open_btree_count;                  /* Locked: open writable btree count */
    uint32_t next_file_id;                   /* Locked: file ID counter */
    wt_shared uint32_t open_file_count;      /* Atomic: open file handle count */
    wt_shared uint32_t open_cursor_count;    /* Atomic: open cursor handle count */
    wt_shared uint32_t version_cursor_count; /* Atomic: open version cursor count */

    /*
     * WiredTiger allocates space for 50 simultaneous sessions (threads of control) by default.
     * Growing the number of threads dynamically is possible, but tricky since server threads are
     * walking the array without locking it.
     *
     * There's an array of WT_SESSION_IMPL pointers that reference the allocated array; we do it
     * that way because we want an easy way for the server thread code to avoid walking the entire
     * array when only a few threads are running.
     */
    struct {
        WT_SESSION_IMPL *__array; /* Session reference. Do not use this field directly. */
        uint32_t size;            /* Session array size */
        wt_shared uint32_t cnt;   /* Session count */
    } session_array;

    size_t session_scratch_max; /* Max scratch memory per session */

    WT_CACHE *cache;                        /* Page cache */
    wt_shared volatile uint64_t cache_size; /* Cache size (either statically
                                     configured or the current size
                                     within a cache pool). */

    WT_TXN_GLOBAL txn_global; /* Global transaction state */

    /* Recovery checkpoint snapshot details saved in the metadata file */
    uint64_t recovery_ckpt_snap_min, recovery_ckpt_snap_max;
    uint64_t *recovery_ckpt_snapshot;
    uint32_t recovery_ckpt_snapshot_count;

    WT_RWLOCK hot_backup_lock; /* Hot backup serialization */
    uint64_t hot_backup_start; /* Clock value of most recent checkpoint needed by hot backup */
    char **hot_backup_list;    /* Hot backup file list */
    uint32_t *partial_backup_remove_ids; /* Remove btree id list for partial backup */

    WT_SESSION_IMPL *ckpt_session;       /* Checkpoint thread session */
    wt_thread_t ckpt_tid;                /* Checkpoint thread */
    bool ckpt_tid_set;                   /* Checkpoint thread set */
    WT_CONDVAR *ckpt_cond;               /* Checkpoint wait mutex */
    wt_shared uint64_t ckpt_most_recent; /* Clock value of most recent checkpoint */
#define WT_CKPT_LOGSIZE(conn) ((conn)->ckpt_logsize != 0)
    wt_off_t ckpt_logsize; /* Checkpoint log size period */
    bool ckpt_signalled;   /* Checkpoint signalled */

    uint64_t ckpt_apply;           /* Checkpoint handles applied */
    uint64_t ckpt_apply_time;      /* Checkpoint applied handles gather time */
    uint64_t ckpt_drop;            /* Checkpoint handles drop */
    uint64_t ckpt_drop_time;       /* Checkpoint handles drop time */
    uint64_t ckpt_lock;            /* Checkpoint handles lock */
    uint64_t ckpt_lock_time;       /* Checkpoint handles lock time */
    uint64_t ckpt_meta_check;      /* Checkpoint handles metadata check */
    uint64_t ckpt_meta_check_time; /* Checkpoint handles metadata check time */
    uint64_t ckpt_skip;            /* Checkpoint handles skipped */
    uint64_t ckpt_skip_time;       /* Checkpoint skipped handles gather time */
    uint64_t ckpt_usecs;           /* Checkpoint timer */

    uint64_t ckpt_scrub_max; /* Checkpoint scrub time min/max */
    uint64_t ckpt_scrub_min;
    uint64_t ckpt_scrub_recent; /* Checkpoint scrub time recent/total */
    uint64_t ckpt_scrub_total;

    uint64_t ckpt_prep_max; /* Checkpoint prepare time min/max */
    uint64_t ckpt_prep_min;
    uint64_t ckpt_prep_recent; /* Checkpoint prepare time recent/total */
    uint64_t ckpt_prep_total;
    uint64_t ckpt_time_max; /* Checkpoint time min/max */
    uint64_t ckpt_time_min;
    uint64_t ckpt_time_recent; /* Checkpoint time recent/total */
    uint64_t ckpt_time_total;

    /* Checkpoint stats and verbosity timers */
    struct timespec ckpt_prep_end;
    struct timespec ckpt_prep_start;
    struct timespec ckpt_timer_start;
    struct timespec ckpt_timer_scrub_end;

    /* Checkpoint progress message data */
    uint64_t ckpt_progress_msg_count;
    uint64_t ckpt_write_bytes;
    uint64_t ckpt_write_pages;

    /* Record the important timestamps of each stage in recovery. */
    struct __wt_recovery_timeline {
        uint64_t log_replay_ms;
        uint64_t rts_ms;
        uint64_t checkpoint_ms;
        uint64_t recovery_ms;
    } recovery_timeline;

    /* Record the important timestamps of each stage in shutdown. */
    struct __wt_shutdown_timeline {
        uint64_t rts_ms;
        uint64_t checkpoint_ms;
        uint64_t shutdown_ms;
    } shutdown_timeline;
    /* Checkpoint and incremental backup data */
    uint64_t incr_granularity;
    WT_BLKINCR incr_backups[WT_BLKINCR_MAX];

    /* Connection's base write generation. */
    uint64_t base_write_gen;

    /* Last checkpoint connection's base write generation */
    uint64_t last_ckpt_base_write_gen;

    uint32_t stat_flags; /* Options declared in flags.py */

    /* Connection statistics */
    uint64_t rec_maximum_hs_wrapup_milliseconds; /* Maximum milliseconds moving updates to history
                                                    store took. */
    uint64_t
      rec_maximum_image_build_milliseconds; /* Maximum milliseconds building disk image took. */
    uint64_t rec_maximum_milliseconds;      /* Maximum milliseconds reconciliation took. */
    WT_CONNECTION_STATS *stats[WT_COUNTER_SLOTS];
    WT_CONNECTION_STATS *stat_array;

    WT_CAPACITY capacity;              /* Capacity structure */
    WT_SESSION_IMPL *capacity_session; /* Capacity thread session */
    wt_thread_t capacity_tid;          /* Capacity thread */
    bool capacity_tid_set;             /* Capacity thread set */
    WT_CONDVAR *capacity_cond;         /* Capacity wait mutex */

    wt_shared WT_LSM_MANAGER lsm_manager; /* LSM worker thread information */

#define WT_CONN_TIERED_STORAGE_ENABLED(conn) ((conn)->bstorage != NULL)
    WT_BUCKET_STORAGE *bstorage;     /* Bucket storage for the connection */
    WT_BUCKET_STORAGE bstorage_none; /* Bucket storage for "none" */

    WT_KEYED_ENCRYPTOR *kencryptor; /* Encryptor for metadata and log */

    bool evict_server_running; /* Eviction server operating */

    WT_THREAD_GROUP evict_threads;
    uint32_t evict_threads_max; /* Max eviction threads */
    uint32_t evict_threads_min; /* Min eviction threads */

#define WT_MAX_PREFETCH_QUEUE 120
#define WT_PREFETCH_QUEUE_PER_TRIGGER 30
#define WT_PREFETCH_THREAD_COUNT 8
    WT_SPINLOCK prefetch_lock;
    WT_THREAD_GROUP prefetch_threads;
    uint64_t prefetch_queue_count;
    /* Queue of refs to pre-fetch from */
    TAILQ_HEAD(__wt_pf_qh, __wt_prefetch_queue_entry) pfqh; /* Locked: prefetch_lock */
    bool prefetch_auto_on;
    bool prefetch_available;

#define WT_STATLOG_FILENAME "WiredTigerStat.%d.%H"
    WT_SESSION_IMPL *stat_session; /* Statistics log session */
    wt_thread_t stat_tid;          /* Statistics log thread */
    bool stat_tid_set;             /* Statistics log thread set */
    WT_CONDVAR *stat_cond;         /* Statistics log wait mutex */
    const char *stat_format;       /* Statistics log timestamp format */
    WT_FSTREAM *stat_fs;           /* Statistics log stream */
    /* Statistics log json table printing state flag */
    bool stat_json_tables;
    char *stat_path;        /* Statistics log path format */
    char **stat_sources;    /* Statistics log list of objects */
    const char *stat_stamp; /* Statistics log entry timestamp */
    uint64_t stat_usecs;    /* Statistics log period */

    WT_SESSION_IMPL *tiered_session; /* Tiered thread session */
    wt_thread_t tiered_tid;          /* Tiered thread */
    bool tiered_tid_set;             /* Tiered thread set */
    WT_CONDVAR *flush_cond;          /* Flush wait mutex */
    WT_CONDVAR *tiered_cond;         /* Tiered wait mutex */
    uint64_t tiered_interval;        /* Tiered work interval */
    bool tiered_server_running;      /* Internal tiered server operating */
    bool flush_ckpt_complete;        /* Checkpoint after flush completed */
    uint64_t flush_most_recent;      /* Clock value of last flush_tier */
    uint32_t flush_state;            /* State of last flush tier */
    wt_timestamp_t flush_ts;         /* Timestamp of most recent flush_tier */

    WT_SESSION_IMPL *chunkcache_metadata_session; /* Chunk cache metadata server thread session */
    wt_thread_t chunkcache_metadata_tid;          /* Chunk cache metadata thread */
    bool chunkcache_metadata_tid_set;             /* Chunk cache metadata thread set */
    WT_CONDVAR *chunkcache_metadata_cond;         /* Chunk cache metadata wait mutex */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_LOG_CONFIG_ENABLED 0x001u  /* Logging is configured */
#define WT_CONN_LOG_DOWNGRADED 0x002u      /* Running older version */
#define WT_CONN_LOG_ENABLED 0x004u         /* Logging is enabled */
#define WT_CONN_LOG_EXISTED 0x008u         /* Log files found */
#define WT_CONN_LOG_FORCE_DOWNGRADE 0x010u /* Force downgrade */
#define WT_CONN_LOG_INCR_BACKUP 0x020u     /* Incremental backup log required */
#define WT_CONN_LOG_RECOVER_DIRTY 0x040u   /* Recovering unclean */
#define WT_CONN_LOG_RECOVER_DONE 0x080u    /* Recovery completed */
#define WT_CONN_LOG_RECOVER_ERR 0x100u     /* Error if recovery required */
#define WT_CONN_LOG_RECOVER_FAILED 0x200u  /* Recovery failed */
#define WT_CONN_LOG_REMOVE 0x400u          /* Removal is enabled */
#define WT_CONN_LOG_ZERO_FILL 0x800u       /* Manually zero files */
                                           /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t log_flags;                    /* Global logging configuration */
    WT_CONDVAR *log_cond;                  /* Log server wait mutex */
    WT_SESSION_IMPL *log_session;          /* Log server session */
    wt_thread_t log_tid;                   /* Log server thread */
    bool log_tid_set;                      /* Log server thread set */
    WT_CONDVAR *log_file_cond;             /* Log file thread wait mutex */
    WT_SESSION_IMPL *log_file_session;     /* Log file thread session */
    wt_thread_t log_file_tid;              /* Log file thread */
    bool log_file_tid_set;                 /* Log file thread set */
    WT_CONDVAR *log_wrlsn_cond;            /* Log write lsn thread wait mutex */
    WT_SESSION_IMPL *log_wrlsn_session;    /* Log write lsn thread session */
    wt_thread_t log_wrlsn_tid;             /* Log write lsn thread */
    bool log_wrlsn_tid_set;                /* Log write lsn thread set */
    WT_LOG *log;                           /* Logging structure */
    WT_COMPRESSOR *log_compressor;         /* Logging compressor */
    wt_shared uint32_t log_cursors;        /* Log cursor count */
    wt_off_t log_dirty_max;                /* Log dirty system cache max size */
    wt_off_t log_file_max;                 /* Log file max size */
    uint32_t log_force_write_wait;         /* Log force write wait configuration */
    const char *log_path;                  /* Logging path format */
    uint32_t log_prealloc;                 /* Log file pre-allocation */
    uint16_t log_req_max;                  /* Max required log version */
    uint16_t log_req_min;                  /* Min required log version */
    wt_shared uint32_t txn_logsync;        /* Log sync configuration */

    WT_ROLLBACK_TO_STABLE *rts, _rts;   /* Rollback to stable subsystem */
    WT_SESSION_IMPL *meta_ckpt_session; /* Metadata checkpoint session */

    /*
     * Is there a data/schema change that needs to be the part of a checkpoint.
     */
    bool modified;

    WT_SESSION_IMPL *sweep_session; /* Handle sweep session */
    wt_thread_t sweep_tid;          /* Handle sweep thread */
    int sweep_tid_set;              /* Handle sweep thread set */
    WT_CONDVAR *sweep_cond;         /* Handle sweep wait mutex */
    uint64_t sweep_idle_time;       /* Handle sweep idle time */
    uint64_t sweep_interval;        /* Handle sweep interval */
    uint64_t sweep_handles_min;     /* Handle sweep minimum open */

    /* Locked: collator list */
    TAILQ_HEAD(__wt_coll_qh, __wt_named_collator) collqh;

    /* Locked: compressor list */
    TAILQ_HEAD(__wt_comp_qh, __wt_named_compressor) compqh;

    /* Locked: data source list */
    TAILQ_HEAD(__wt_dsrc_qh, __wt_named_data_source) dsrcqh;

    /* Locked: encryptor list */
    WT_SPINLOCK encryptor_lock; /* Encryptor list lock */
    TAILQ_HEAD(__wt_encrypt_qh, __wt_named_encryptor) encryptqh;

    /* Locked: extractor list */
    TAILQ_HEAD(__wt_extractor_qh, __wt_named_extractor) extractorqh;

    /* Locked: storage source list */
    WT_SPINLOCK storage_lock; /* Storage source list lock */
    TAILQ_HEAD(__wt_storage_source_qh, __wt_named_storage_source) storagesrcqh;

    void *lang_private; /* Language specific private storage */

    /* Compiled configuration */
    char *conf_dummy;         /* Dummy strings used by caller */
    WT_CONF **conf_api_array; /* The array of standard API pre-compiled configurations */
    WT_CONF **conf_array;     /* The array of user compiled configurations */
    uint32_t conf_size;       /* In use size of user compiled configuration array */
    uint32_t conf_max;        /* Allocated size of user compiled configuration array */

    /* If non-zero, all buffers used for I/O will be aligned to this. */
    size_t buffer_alignment;

    wt_shared uint64_t stashed_bytes; /* Atomic: stashed memory statistics */
    wt_shared uint64_t stashed_objects;

    /* Generations manager */
    wt_shared volatile uint64_t generations[WT_GENERATIONS];
    uint64_t gen_drain_timeout_ms; /* Maximum waiting time for a resource to drain in diagnostic
                                      mode before timing out */

    wt_off_t data_extend_len; /* file_extend data length */
    wt_off_t log_extend_len;  /* file_extend log length */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DIRECT_IO_CHECKPOINT 0x1ull /* Checkpoints */
#define WT_DIRECT_IO_DATA 0x2ull       /* Data files */
#define WT_DIRECT_IO_LOG 0x4ull        /* Log files */
                                       /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t direct_io;                /* O_DIRECT, FILE_FLAG_NO_BUFFERING */
    uint64_t write_through;            /* FILE_FLAG_WRITE_THROUGH */

    bool mmap;     /* use mmap when reading checkpoints */
    bool mmap_all; /* use mmap for all I/O on data files */
    int page_size; /* OS page size for mmap alignment */

    /* Access to these fields is protected by the debug_log_retention_lock. */
    WT_LSN *debug_ckpt;                /* Debug mode checkpoint LSNs. */
    size_t debug_ckpt_alloc;           /* Checkpoint retention allocated. */
    wt_shared uint32_t debug_ckpt_cnt; /* Checkpoint retention number. */
    wt_shared uint32_t debug_log_cnt;  /* Log file retention count */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_DEBUG_CKPT_RETAIN 0x001u
#define WT_CONN_DEBUG_CORRUPTION_ABORT 0x002u
#define WT_CONN_DEBUG_CURSOR_COPY 0x004u
#define WT_CONN_DEBUG_CURSOR_REPOSITION 0x008u
#define WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE 0x010u
#define WT_CONN_DEBUG_REALLOC_EXACT 0x020u
#define WT_CONN_DEBUG_REALLOC_MALLOC 0x040u
#define WT_CONN_DEBUG_SLOW_CKPT 0x080u
#define WT_CONN_DEBUG_STRESS_SKIPLIST 0x100u
#define WT_CONN_DEBUG_TABLE_LOGGING 0x200u
#define WT_CONN_DEBUG_TIERED_FLUSH_ERROR_CONTINUE 0x400u
#define WT_CONN_DEBUG_UPDATE_RESTORE_EVICT 0x800u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 16 */
    uint16_t debug_flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DIAGNOSTIC_ALL 0x001ull
#define WT_DIAGNOSTIC_CHECKPOINT_VALIDATE 0x002ull
#define WT_DIAGNOSTIC_CURSOR_CHECK 0x004ull
#define WT_DIAGNOSTIC_DISK_VALIDATE 0x008ull
#define WT_DIAGNOSTIC_EVICTION_CHECK 0x010ull
#define WT_DIAGNOSTIC_HS_VALIDATE 0x020ull
#define WT_DIAGNOSTIC_KEY_OUT_OF_ORDER 0x040ull
#define WT_DIAGNOSTIC_LOG_VALIDATE 0x080ull
#define WT_DIAGNOSTIC_PREPARED 0x100ull
#define WT_DIAGNOSTIC_SLOW_OPERATION 0x200ull
#define WT_DIAGNOSTIC_TXN_VISIBILITY 0x400ull
    /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    /* Categories of assertions that can be runtime enabled. */
    uint64_t extra_diagnostics_flags;

    /* Verbose settings for our various categories. */
    WT_VERBOSE_LEVEL verbose[WT_VERB_NUM_CATEGORIES];

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_JSON_OUTPUT_ERROR 0x1ull
#define WT_JSON_OUTPUT_MESSAGE 0x2ull
    /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t json_output; /* Output event handler messages in JSON format. */

/*
 * Variable with flags for which subsystems the diagnostic stress timing delays have been requested.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIMING_STRESS_AGGRESSIVE_STASH_FREE 0x0000001ull
#define WT_TIMING_STRESS_AGGRESSIVE_SWEEP 0x0000002ull
#define WT_TIMING_STRESS_BACKUP_RENAME 0x0000004ull
#define WT_TIMING_STRESS_CHECKPOINT_EVICT_PAGE 0x0000008ull
#define WT_TIMING_STRESS_CHECKPOINT_HANDLE 0x0000010ull
#define WT_TIMING_STRESS_CHECKPOINT_SLOW 0x0000020ull
#define WT_TIMING_STRESS_CHECKPOINT_STOP 0x0000040ull
#define WT_TIMING_STRESS_COMPACT_SLOW 0x0000080ull
#define WT_TIMING_STRESS_EVICT_REPOSITION 0x0000100ull
#define WT_TIMING_STRESS_FAILPOINT_EVICTION_SPLIT 0x0000200ull
#define WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS 0x0000400ull
#define WT_TIMING_STRESS_HS_CHECKPOINT_DELAY 0x0000800ull
#define WT_TIMING_STRESS_HS_SEARCH 0x0001000ull
#define WT_TIMING_STRESS_HS_SWEEP 0x0002000ull
#define WT_TIMING_STRESS_PREFIX_COMPARE 0x0004000ull
#define WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY 0x0008000ull
#define WT_TIMING_STRESS_PREPARE_RESOLUTION_1 0x0010000ull
#define WT_TIMING_STRESS_PREPARE_RESOLUTION_2 0x0020000ull
#define WT_TIMING_STRESS_SLEEP_BEFORE_READ_OVERFLOW_ONPAGE 0x0040000ull
#define WT_TIMING_STRESS_SPLIT_1 0x0080000ull
#define WT_TIMING_STRESS_SPLIT_2 0x0100000ull
#define WT_TIMING_STRESS_SPLIT_3 0x0200000ull
#define WT_TIMING_STRESS_SPLIT_4 0x0400000ull
#define WT_TIMING_STRESS_SPLIT_5 0x0800000ull
#define WT_TIMING_STRESS_SPLIT_6 0x1000000ull
#define WT_TIMING_STRESS_SPLIT_7 0x2000000ull
#define WT_TIMING_STRESS_SPLIT_8 0x4000000ull
#define WT_TIMING_STRESS_TIERED_FLUSH_FINISH 0x8000000ull
    /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t timing_stress_flags;

#define WT_STDERR(s) (&S2C(s)->wt_stderr)
#define WT_STDOUT(s) (&S2C(s)->wt_stdout)
    WT_FSTREAM wt_stderr, wt_stdout;

    /*
     * File system interface abstracted to support alternative file system implementations.
     */
    WT_FILE_SYSTEM *file_system;

/*
 * Server subsystem flags.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_SERVER_CAPACITY 0x001u
#define WT_CONN_SERVER_CHECKPOINT 0x002u
#define WT_CONN_SERVER_CHUNKCACHE_METADATA 0x004u
#define WT_CONN_SERVER_COMPACT 0x008u
#define WT_CONN_SERVER_LOG 0x010u
#define WT_CONN_SERVER_LSM 0x020u
#define WT_CONN_SERVER_STATISTICS 0x040u
#define WT_CONN_SERVER_SWEEP 0x080u
#define WT_CONN_SERVER_TIERED 0x100u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t server_flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_BACKUP_PARTIAL_RESTORE 0x00000001u
#define WT_CONN_CACHE_CURSORS 0x00000002u
#define WT_CONN_CACHE_POOL 0x00000004u
#define WT_CONN_CALL_LOG_ENABLED 0x00000008u
#define WT_CONN_CKPT_CLEANUP_SKIP_INT 0x00000010u
#define WT_CONN_CKPT_GATHER 0x00000020u
#define WT_CONN_CKPT_SYNC 0x00000040u
#define WT_CONN_CLOSING 0x00000080u
#define WT_CONN_CLOSING_CHECKPOINT 0x00000100u
#define WT_CONN_CLOSING_NO_MORE_OPENS 0x00000200u
#define WT_CONN_COMPATIBILITY 0x00000400u
#define WT_CONN_DATA_CORRUPTION 0x00000800u
#define WT_CONN_EVICTION_RUN 0x00001000u
#define WT_CONN_HS_OPEN 0x00002000u
#define WT_CONN_INCR_BACKUP 0x00004000u
#define WT_CONN_IN_MEMORY 0x00008000u
#define WT_CONN_LEAK_MEMORY 0x00010000u
#define WT_CONN_LSM_MERGE 0x00020000u
#define WT_CONN_MINIMAL 0x00040000u
#define WT_CONN_OPTRACK 0x00080000u
#define WT_CONN_PANIC 0x00100000u
#define WT_CONN_PREFETCH_RUN 0x00200000u
#define WT_CONN_READONLY 0x00400000u
#define WT_CONN_READY 0x00800000u
#define WT_CONN_RECONFIGURING 0x01000000u
#define WT_CONN_RECOVERING 0x02000000u
#define WT_CONN_RECOVERY_COMPLETE 0x04000000u
#define WT_CONN_RTS_THREAD_RUN 0x08000000u
#define WT_CONN_SALVAGE 0x10000000u
#define WT_CONN_TIERED_FIRST_FLUSH 0x20000000u
#define WT_CONN_WAS_BACKUP 0x40000000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint32_t flags;
};

/*
 * WT_VERBOSE_DUMP_COOKIE --
 *   State passed through to callbacks during the session walk logic when dumping all sessions.
 */
struct __wt_verbose_dump_cookie {
    uint32_t internal_session_count;
    bool show_cursors;
};

/*
 * WT_SWEEP_COOKIE --
 *   State passed through to callbacks during the session walk logic when checking for sessions that
 *   haven't performed a sweep in a long time.
 */
struct __wt_sweep_cookie {
    uint64_t now;
};
