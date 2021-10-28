/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

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

#define WT_TSC_DEFAULT_RATIO 1.0
    double tsc_nsec_ratio; /* rdtsc ticks to nanoseconds */
    bool use_epochtime;    /* use expensive time */

    WT_CACHE_POOL *cache_pool; /* shared cache information */
};
extern WT_PROCESS __wt_process;

/*
 * WT_BUCKET_STORAGE --
 *	A list entry for a storage source with a unique name (bucket, prefix).
 */
struct __wt_bucket_storage {
    const char *bucket;                /* Bucket name */
    const char *bucket_prefix;         /* Bucket prefix */
    const char *cache_directory;       /* Locally cached file location */
    int owned;                         /* Storage needs to be terminated */
    uint64_t object_size;              /* Tiered object size */
    uint64_t retain_secs;              /* Tiered period */
    const char *auth_token;            /* Tiered authentication cookie */
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
 * WT_CONNECTION_IMPL --
 *	Implementation of WT_CONNECTION
 */
struct __wt_connection_impl {
    WT_CONNECTION iface;

    /* For operations without an application-supplied session */
    WT_SESSION_IMPL *default_session;
    WT_SESSION_IMPL dummy_session;

    const char *cfg; /* Connection configuration */

    WT_SPINLOCK api_lock;        /* Connection API spinlock */
    WT_SPINLOCK checkpoint_lock; /* Checkpoint spinlock */
    WT_SPINLOCK fh_lock;         /* File handle queue spinlock */
    WT_SPINLOCK flush_tier_lock; /* Flush tier spinlock */
    WT_SPINLOCK metadata_lock;   /* Metadata update spinlock */
    WT_SPINLOCK reconfig_lock;   /* Single thread reconfigure */
    WT_SPINLOCK schema_lock;     /* Schema operation spinlock */
    WT_RWLOCK table_lock;        /* Table list lock */
    WT_SPINLOCK tiered_lock;     /* Tiered work queue spinlock */
    WT_SPINLOCK turtle_lock;     /* Turtle file spinlock */
    WT_RWLOCK dhandle_lock;      /* Data handle list lock */

    /* Connection queue */
    TAILQ_ENTRY(__wt_connection_impl) q;
    /* Cache pool queue */
    TAILQ_ENTRY(__wt_connection_impl) cpq;

    const char *home;         /* Database home */
    const char *error_prefix; /* Database error prefix */
    uint64_t dh_hash_size;    /* Data handle hash bucket array size */
    uint64_t hash_size;       /* General hash bucket array size */
    int is_new;               /* Connection created database */

    uint16_t compat_major; /* Compatibility major version */
    uint16_t compat_minor; /* Compatibility minor version */
#define WT_CONN_COMPAT_NONE UINT16_MAX
    uint16_t req_max_major; /* Compatibility maximum major */
    uint16_t req_max_minor; /* Compatibility maximum minor */
    uint16_t req_min_major; /* Compatibility minimum major */
    uint16_t req_min_minor; /* Compatibility minimum minor */

    WT_EXTENSION_API extension_api; /* Extension API */

    /* Configuration */
    const WT_CONFIG_ENTRY **config_entries;

    uint64_t operation_timeout_us; /* Maximum operation period before rollback */

    const char *optrack_path;         /* Directory for operation logs */
    WT_FH *optrack_map_fh;            /* Name to id translation file. */
    WT_SPINLOCK optrack_map_spinlock; /* Translation file spinlock. */
    uintmax_t optrack_pid;            /* Cache the process ID. */

    void **foc;      /* Free-on-close array */
    size_t foc_cnt;  /* Array entries */
    size_t foc_size; /* Array size */

    WT_FH *lock_fh; /* Lock file handle */

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

    WT_BLKCACHE blkcache; /* Block cache */

    /* Locked: handles in each bucket */
    uint64_t *dh_bucket_count;
    uint64_t dhandle_count;     /* Locked: handles in the queue */
    u_int open_btree_count;     /* Locked: open writable btree count */
    uint32_t next_file_id;      /* Locked: file ID counter */
    uint32_t open_file_count;   /* Atomic: open file handle count */
    uint32_t open_cursor_count; /* Atomic: open cursor handle count */

    /*
     * WiredTiger allocates space for 50 simultaneous sessions (threads of control) by default.
     * Growing the number of threads dynamically is possible, but tricky since server threads are
     * walking the array without locking it.
     *
     * There's an array of WT_SESSION_IMPL pointers that reference the allocated array; we do it
     * that way because we want an easy way for the server thread code to avoid walking the entire
     * array when only a few threads are running.
     */
    WT_SESSION_IMPL *sessions; /* Session reference */
    uint32_t session_size;     /* Session array size */
    uint32_t session_cnt;      /* Session count */

    size_t session_scratch_max; /* Max scratch memory per session */

    WT_CACHE *cache;              /* Page cache */
    volatile uint64_t cache_size; /* Cache size (either statically
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

    WT_SESSION_IMPL *ckpt_session; /* Checkpoint thread session */
    wt_thread_t ckpt_tid;          /* Checkpoint thread */
    bool ckpt_tid_set;             /* Checkpoint thread set */
    WT_CONDVAR *ckpt_cond;         /* Checkpoint wait mutex */
    uint64_t ckpt_most_recent;     /* Clock value of most recent checkpoint */
#define WT_CKPT_LOGSIZE(conn) ((conn)->ckpt_logsize != 0)
    wt_off_t ckpt_logsize; /* Checkpoint log size period */
    bool ckpt_signalled;   /* Checkpoint signalled */

    uint64_t ckpt_apply;      /* Checkpoint handles applied */
    uint64_t ckpt_apply_time; /* Checkpoint applied handles gather time */
    uint64_t ckpt_skip;       /* Checkpoint handles skipped */
    uint64_t ckpt_skip_time;  /* Checkpoint skipped handles gather time */
    uint64_t ckpt_usecs;      /* Checkpoint timer */
    uint64_t ckpt_prep_max;   /* Checkpoint prepare time min/max */
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

    /* Checkpoint and incremental backup data */
    uint64_t incr_granularity;
    WT_BLKINCR incr_backups[WT_BLKINCR_MAX];

    /* Connection's base write generation. */
    uint64_t base_write_gen;

    /* Last checkpoint connection's base write generation */
    uint64_t last_ckpt_base_write_gen;

    uint32_t stat_flags; /* Options declared in flags.py */

    /* Connection statistics */
    uint64_t rec_maximum_seconds; /* Maximum seconds reconciliation took. */
    WT_CONNECTION_STATS *stats[WT_COUNTER_SLOTS];
    WT_CONNECTION_STATS *stat_array;

    WT_CAPACITY capacity;              /* Capacity structure */
    WT_SESSION_IMPL *capacity_session; /* Capacity thread session */
    wt_thread_t capacity_tid;          /* Capacity thread */
    bool capacity_tid_set;             /* Capacity thread set */
    WT_CONDVAR *capacity_cond;         /* Capacity wait mutex */

    WT_LSM_MANAGER lsm_manager; /* LSM worker thread information */

    WT_BUCKET_STORAGE *bstorage;     /* Bucket storage for the connection */
    WT_BUCKET_STORAGE bstorage_none; /* Bucket storage for "none" */

    WT_KEYED_ENCRYPTOR *kencryptor; /* Encryptor for metadata and log */

    bool evict_server_running; /* Eviction server operating */

    WT_THREAD_GROUP evict_threads;
    uint32_t evict_threads_max; /* Max eviction threads */
    uint32_t evict_threads_min; /* Min eviction threads */

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

    uint64_t tiered_retention;       /* Earliest time to check to remove local overlap copies */
    WT_SESSION_IMPL *tiered_session; /* Tiered thread session */
    wt_thread_t tiered_tid;          /* Tiered thread */
    bool tiered_tid_set;             /* Tiered thread set */
    WT_CONDVAR *flush_cond;          /* Flush wait mutex */
    WT_CONDVAR *tiered_cond;         /* Tiered wait mutex */
    bool tiered_server_running;      /* Internal tiered server operating */
    uint32_t flush_state;            /* State of last flush tier */

    WT_TIERED_MANAGER tiered_mgr;        /* Tiered manager thread information */
    WT_SESSION_IMPL *tiered_mgr_session; /* Tiered manager thread session */
    wt_thread_t tiered_mgr_tid;          /* Tiered manager thread */
    bool tiered_mgr_tid_set;             /* Tiered manager thread set */
    WT_CONDVAR *tiered_mgr_cond;         /* Tiered manager wait mutex */

    uint32_t tiered_threads_max; /* Max tiered threads */
    uint32_t tiered_threads_min; /* Min tiered threads */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_LOG_ARCHIVE 0x001u         /* Archive is enabled */
#define WT_CONN_LOG_CONFIG_ENABLED 0x002u  /* Logging is configured */
#define WT_CONN_LOG_DEBUG_MODE 0x004u      /* Debug-mode logging enabled */
#define WT_CONN_LOG_DOWNGRADED 0x008u      /* Running older version */
#define WT_CONN_LOG_ENABLED 0x010u         /* Logging is enabled */
#define WT_CONN_LOG_EXISTED 0x020u         /* Log files found */
#define WT_CONN_LOG_FORCE_DOWNGRADE 0x040u /* Force downgrade */
#define WT_CONN_LOG_RECOVER_DIRTY 0x080u   /* Recovering unclean */
#define WT_CONN_LOG_RECOVER_DONE 0x100u    /* Recovery completed */
#define WT_CONN_LOG_RECOVER_ERR 0x200u     /* Error if recovery required */
#define WT_CONN_LOG_RECOVER_FAILED 0x400u  /* Recovery failed */
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
    uint32_t log_cursors;                  /* Log cursor count */
    wt_off_t log_dirty_max;                /* Log dirty system cache max size */
    wt_off_t log_file_max;                 /* Log file max size */
    const char *log_path;                  /* Logging path format */
    uint32_t log_prealloc;                 /* Log file pre-allocation */
    uint16_t log_req_max;                  /* Max required log version */
    uint16_t log_req_min;                  /* Min required log version */
    uint32_t txn_logsync;                  /* Log sync configuration */

    WT_SESSION_IMPL *meta_ckpt_session;     /* Metadata checkpoint session */
    WT_SESSION_IMPL *ckpt_reserved_session; /* Checkpoint reserved session */

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

    /* If non-zero, all buffers used for I/O will be aligned to this. */
    size_t buffer_alignment;

    uint64_t stashed_bytes; /* Atomic: stashed memory statistics */
    uint64_t stashed_objects;
    /* Generations manager */
    volatile uint64_t generations[WT_GENERATIONS];

    wt_off_t data_extend_len; /* file_extend data length */
    wt_off_t log_extend_len;  /* file_extend log length */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DIRECT_IO_CHECKPOINT 0x1u /* Checkpoints */
#define WT_DIRECT_IO_DATA 0x2u       /* Data files */
#define WT_DIRECT_IO_LOG 0x4u        /* Log files */
                                     /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t direct_io;              /* O_DIRECT, FILE_FLAG_NO_BUFFERING */
    uint64_t write_through;          /* FILE_FLAG_WRITE_THROUGH */

    bool mmap;     /* use mmap when reading checkpoints */
    bool mmap_all; /* use mmap for all I/O on data files */
    int page_size; /* OS page size for mmap alignment */

    WT_LSN *debug_ckpt;      /* Debug mode checkpoint LSNs. */
    size_t debug_ckpt_alloc; /* Checkpoint retention allocated. */
    uint32_t debug_ckpt_cnt; /* Checkpoint retention number. */
    uint32_t debug_log_cnt;  /* Log file retention count */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_DEBUG_CKPT_RETAIN 0x01u
#define WT_CONN_DEBUG_CORRUPTION_ABORT 0x02u
#define WT_CONN_DEBUG_CURSOR_COPY 0x04u
#define WT_CONN_DEBUG_REALLOC_EXACT 0x08u
#define WT_CONN_DEBUG_SLOW_CKPT 0x10u
#define WT_CONN_DEBUG_UPDATE_RESTORE_EVICT 0x20u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t debug_flags;

    /* Verbose settings for our various categories. */
    WT_VERBOSE_LEVEL verbose[WT_VERB_NUM_CATEGORIES];

/*
 * Variable with flags for which subsystems the diagnostic stress timing delays have been requested.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TIMING_STRESS_AGGRESSIVE_SWEEP 0x00001u
#define WT_TIMING_STRESS_BACKUP_RENAME 0x00002u
#define WT_TIMING_STRESS_CHECKPOINT_RESERVED_TXNID_DELAY 0x00004u
#define WT_TIMING_STRESS_CHECKPOINT_SLOW 0x00008u
#define WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS 0x00010u
#define WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_INSERT_1 0x00020u
#define WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_INSERT_2 0x00040u
#define WT_TIMING_STRESS_HS_CHECKPOINT_DELAY 0x00080u
#define WT_TIMING_STRESS_HS_SEARCH 0x00100u
#define WT_TIMING_STRESS_HS_SWEEP 0x00200u
#define WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY 0x00400u
#define WT_TIMING_STRESS_SPLIT_1 0x00800u
#define WT_TIMING_STRESS_SPLIT_2 0x01000u
#define WT_TIMING_STRESS_SPLIT_3 0x02000u
#define WT_TIMING_STRESS_SPLIT_4 0x04000u
#define WT_TIMING_STRESS_SPLIT_5 0x08000u
#define WT_TIMING_STRESS_SPLIT_6 0x10000u
#define WT_TIMING_STRESS_SPLIT_7 0x20000u
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
#define WT_CONN_SERVER_CAPACITY 0x01u
#define WT_CONN_SERVER_CHECKPOINT 0x02u
#define WT_CONN_SERVER_LOG 0x04u
#define WT_CONN_SERVER_LSM 0x08u
#define WT_CONN_SERVER_STATISTICS 0x10u
#define WT_CONN_SERVER_SWEEP 0x20u
#define WT_CONN_SERVER_TIERED 0x40u
#define WT_CONN_SERVER_TIERED_MGR 0x80u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t server_flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_CACHE_CURSORS 0x000001u
#define WT_CONN_CACHE_POOL 0x000002u
#define WT_CONN_CKPT_GATHER 0x000004u
#define WT_CONN_CKPT_SYNC 0x000008u
#define WT_CONN_CLOSING 0x000010u
#define WT_CONN_CLOSING_NO_MORE_OPENS 0x000020u
#define WT_CONN_CLOSING_TIMESTAMP 0x000040u
#define WT_CONN_COMPATIBILITY 0x000080u
#define WT_CONN_DATA_CORRUPTION 0x000100u
#define WT_CONN_EVICTION_RUN 0x000200u
#define WT_CONN_FILE_CLOSE_SYNC 0x000400u
#define WT_CONN_HS_OPEN 0x000800u
#define WT_CONN_INCR_BACKUP 0x001000u
#define WT_CONN_IN_MEMORY 0x002000u
#define WT_CONN_LEAK_MEMORY 0x004000u
#define WT_CONN_LSM_MERGE 0x008000u
#define WT_CONN_OPTRACK 0x010000u
#define WT_CONN_PANIC 0x020000u
#define WT_CONN_READONLY 0x040000u
#define WT_CONN_RECONFIGURING 0x080000u
#define WT_CONN_RECOVERING 0x100000u
#define WT_CONN_SALVAGE 0x200000u
#define WT_CONN_WAS_BACKUP 0x400000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};
