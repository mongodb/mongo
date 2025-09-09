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
    BACKGROUND_COMPACT_CLEANUP_EXIT,      /* Cleanup when the thread exits */
    BACKGROUND_COMPACT_CLEANUP_OFF,       /* Cleanup when the thread is disabled */
    BACKGROUND_COMPACT_CLEANUP_STALE_STAT /* Cleanup stale stats only */
} WT_BACKGROUND_COMPACT_CLEANUP_STAT_TYPE;

/*
 * WT_BACKGROUND_COMPACT_STAT --
 *  List of tracking information for each file compact has worked on.
 */
struct __wt_background_compact_stat {
    const char *uri;
    uint32_t id;                /* File ID */
    bool prev_compact_success;  /* Last compact successfully reclaimed space */
    uint64_t prev_compact_time; /* Start time for last compact attempt */
    uint64_t skip_count;        /* Number of times we've skipped this file */
    uint64_t bytes_rewritten;   /* Bytes rewritten during last compaction call */

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
    wt_shared bool running;   /* Compaction supposed to run */
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
 * WT_LAYERED_TABLE_MANAGER_ENTRY --
 *      Structure containing information about a tracked layered table
 */
struct __wt_layered_table_manager_entry {
    uint32_t ingest_id;
    uint32_t stable_id;
    const char *ingest_uri;
    const char *stable_uri;
    WT_LAYERED_TABLE *layered_table;

    uint64_t checkpoint_txn_id;
};

/*
 * WT_LAYERED_TABLE_MANAGER --
 *      Structure containing information related to running the layered table manager.
 */
struct __wt_layered_table_manager {

#define WT_LAYERED_TABLE_MANAGER_OFF 0     /* The layered table manager is not running */
#define WT_LAYERED_TABLE_MANAGER_RUNNING 1 /* The layered table manager is running */
    uint32_t state;                        /* Indicating the manager is already running */

    WT_SPINLOCK
    layered_table_lock; /* Lock used for managing changes to global layered table state */

    uint32_t open_layered_table_count;
    /*
     * This is a sparsely populated array of layered tables - each fileid in the system gets an
     * entry in this table. A lookups checks for a valid manager entry at the file ID offset for the
     * ingest constituent in a layered table. It's done that way so that we can cheaply check
     * whether a log record belongs to a layered table and should be applied.
     */
    WT_LAYERED_TABLE_MANAGER_ENTRY **entries;
    size_t entries_allocated_bytes;

#define WT_LAYERED_TABLE_THREAD_COUNT 1
    WT_THREAD_GROUP threads;

    bool leader;
};

struct __wt_disagg_copy_metadata {
    char *stable_uri;                         /* The full URI of the stable component. */
    char *table_name;                         /* The table name without prefix or suffix. */
    TAILQ_ENTRY(__wt_disagg_copy_metadata) q; /* Linked list of entries. */
};

#define WT_DISAGG_LSN_NONE 0 /* The LSN is not set. */

/*
 * WT_DISAGGREGATED_CHECKPOINT_TRACK --
 *      A relationship between the checkpoint order number and the history timestamp.
 */
struct __wt_disaggregated_checkpoint_track {
    int64_t ckpt_order;
    wt_timestamp_t timestamp;
};

/*
 * WT_PAGE_DELTA_CONFIG --
 *      Metadata for tracking page deltas
 */
struct __wt_page_delta_config {
    wt_shared uint64_t max_internal_delta_count; /* The maximum number of internal deltas. */
    wt_shared uint64_t max_leaf_delta_count;     /* The maximum number of leaf deltas. */

    u_int delta_pct;             /* Delta page percent (of full page size) */
    u_int max_consecutive_delta; /* Max number of consecutive deltas */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_FLATTEN_LEAF_PAGE_DELTA 0x1u
#define WT_INTERNAL_PAGE_DELTA 0x2u
#define WT_LEAF_PAGE_DELTA 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/*
 * WT_DISAGGREGATED_STORAGE --
 *      Configuration and the current state for disaggregated storage, which tells the Block Manager
 *      how to find remote object storage. This is a separate configuration from layered tables.
 */
struct __wt_disaggregated_storage {
    char *page_log;

    /* Updates are protected by the checkpoint lock. */
    wt_shared uint64_t last_checkpoint_meta_lsn; /* The LSN of the last checkpoint metadata. */
    wt_shared uint64_t last_materialized_lsn;    /* The LSN of the last materialized page. */
    char *last_checkpoint_root;                  /* The root config of the last checkpoint. */

    wt_timestamp_t cur_checkpoint_timestamp; /* The timestamp of the in-progress checkpoint. */
    wt_shared wt_timestamp_t last_checkpoint_timestamp; /* The timestamp of the last checkpoint. */

    WT_NAMED_PAGE_LOG *npage_log;
    WT_PAGE_LOG_HANDLE *page_log_meta; /* The page log for the metadata. */

    wt_shared uint64_t num_meta_put;     /* The number metadata puts since connection open. */
    uint64_t num_meta_put_at_ckpt_begin; /* The number metadata puts at checkpoint begin. */
                                         /* Updates are protected by the checkpoint lock. */

    /* To copy at the next checkpoint. */
    TAILQ_HEAD(__wt_disagg_copy_metadata_qh, __wt_disagg_copy_metadata) copy_metadata_qh;
    WT_SPINLOCK copy_metadata_lock;

    WT_DISAGGREGATED_CHECKPOINT_TRACK *ckpt_track; /* Checkpoint info retained for GC. */
    size_t ckpt_track_alloc;                       /* Allocated bytes for checkpoint track. */
    uint32_t ckpt_track_cnt; /* Number of entries in use for checkpoint track. */
    int64_t ckpt_min_inuse;  /* The minimum checkpoint order in use. */

    /*
     * Ideally we'd have flags passed to the IO system, which could make it all the way to the
     * callers of posix_sync. But that's not possible because (1) posix_directory_sync also has no
     * way to change behavior because it doesn't have a file handle, and (2) the flags for a file
     * handle are all set up when we open the file, which can happen before disagg is set up and the
     * relevant option is parsed. The other unfortunate part is that the flags are all per-file
     * (really, per block-manager) so it's easy to accidentally miss a file when doing it that way,
     * e.g. if the config parsing does anything even slightly off the beaten track.
     */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_DISAGG_NO_SYNC 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 8 */
    uint8_t flags;
};

/*
 * WT_PAGE_HISTORY_KEY --
 *      A key in the page history.
 */
struct __wt_page_history_key {
    uint64_t page_id;
    uint32_t table_id;
};

/*
 * WT_PAGE_HISTORY_ITEM --
 *      An entry in the page history.
 */
struct __wt_page_history_item {
    WT_PAGE_HISTORY_KEY key;

    uint64_t first_global_read_count;
    uint64_t first_read_timestamp;

    uint64_t last_global_read_count;
    uint64_t last_read_timestamp;

    uint32_t num_evicts;
    uint32_t num_reads;

    uint8_t page_type;
};

/*
 * WT_PAGE_HISTORY --
 *      A page history for debugging issues with page lifetime and eviction. It currently requires
 *      page IDs, which are available if used in disaggregated storage.
 */
struct __wt_page_history {
    bool enabled;

    wt_shared uint64_t global_evict_count;
    wt_shared uint64_t global_read_count;
    wt_shared uint64_t global_reread_count;

    wt_shared uint64_t global_evict_count_local;
    wt_shared uint64_t global_evict_count_no_page_id;
    wt_shared uint64_t global_read_count_local;

    WT_HASH_MAP *pages;

    /* The reporting thread. */
    wt_thread_t report_tid;
    bool report_tid_set;
    WT_SESSION_IMPL *report_session;
    WT_CONDVAR *report_cond;
    wt_shared bool report_shutdown;
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
 * WT_HEURISTIC_CONTROLS --
 *  Heuristic controls configuration.
 */
struct __wt_heuristic_controls {
    /* Number of btrees processed in the current checkpoint. */
    wt_shared uint32_t obsolete_tw_btree_count;

    /*
     * The controls below deal with the cleanup of obsolete time window information. This process
     * can be configured based on two configuration items, one that is shared among the subsystems
     * which is the number of btrees and another one which is unique to each subsystem that is
     * the number of pages.
     *   - The maximum number of pages per btree to process in a single checkpoint by checkpoint
     * cleanup.
     *   - The maximum number of pages per btree to process in a single checkpoint by eviction
     * threads.
     *   - The maximum number of btrees to process in a single checkpoint.
     */

    /* Maximum number of pages that can be processed per btree by checkpoint cleanup. */
    uint32_t checkpoint_cleanup_obsolete_tw_pages_dirty_max;

    /* Maximum number of pages that can be processed per btree by eviction. */
    uint32_t eviction_obsolete_tw_pages_dirty_max;

    /* Maximum number of btrees that can be processed per checkpoint. */
    uint32_t obsolete_tw_btree_max;
};

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
 * WT_NAMED_PAGE_LOG --
 *	A page log list entry
 */
struct __wt_named_page_log {
    const char *name;      /* Name of page log */
    WT_PAGE_LOG *page_log; /* User supplied callbacks */
    /* Linked list of page logs */
    TAILQ_ENTRY(__wt_named_page_log) q;
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
#define WT_CONN_CHECK_PANIC(conn) (F_ISSET_ATOMIC_32(conn, WT_CONN_PANIC) ? WT_PANIC : 0)
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
        if (WT_DHANDLE_IS_CHECKPOINT(dhandle))                                                   \
            ++(conn)->dhandle_checkpoint_count;                                                  \
        WT_ASSERT(session, (dhandle)->type < WT_DHANDLE_TYPE_NUM);                               \
        ++(conn)->dhandle_types_count[(dhandle)->type];                                          \
    } while (0)

#define WT_CONN_DHANDLE_REMOVE(conn, dhandle, bucket)                                            \
    do {                                                                                         \
        WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HANDLE_LIST_WRITE)); \
        TAILQ_REMOVE(&(conn)->dhqh, dhandle, q);                                                 \
        TAILQ_REMOVE(&(conn)->dhhash[bucket], dhandle, hashq);                                   \
        --(conn)->dh_bucket_count[bucket];                                                       \
        --(conn)->dhandle_count;                                                                 \
        if (WT_DHANDLE_IS_CHECKPOINT(dhandle))                                                   \
            --(conn)->dhandle_checkpoint_count;                                                  \
        WT_ASSERT(session, (dhandle)->type < WT_DHANDLE_TYPE_NUM);                               \
        --(conn)->dhandle_types_count[(dhandle)->type];                                          \
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
 *	This macro must be called with the hot backup lock held for writing.
 */
#define WT_CONN_HOTBACKUP_START(conn)                                                          \
    do {                                                                                       \
        WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_HOTBACKUP_WRITE)); \
        (conn)->hot_backup_timestamp = (conn)->txn_global.last_ckpt_timestamp;                 \
        __wt_atomic_store64(&(conn)->hot_backup_start, (conn)->ckpt.most_recent);              \
        (conn)->hot_backup_list = NULL;                                                        \
    } while (0)

/*
 * Set all flags related to incremental backup in one macro. The flags do get individually cleared
 * at different times so there is no corresponding macro for clearing.
 */
#define WT_CONN_SET_INCR_BACKUP(conn)                     \
    do {                                                  \
        F_SET_ATOMIC_32((conn), WT_CONN_INCR_BACKUP);     \
        F_SET(&(conn)->log_mgr, WT_LOG_INCR_BACKUP);      \
        WT_STAT_CONN_SET(session, backup_incremental, 1); \
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
 * accesses, if you need to walk all sessions please call __wt_session_array_walk. For more details
 * on this usage pattern see the architecture guide.
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

    WTI_LIVE_RESTORE_SERVER *live_restore_server; /* Live restore server. */

    WT_HEURISTIC_CONTROLS heuristic_controls; /* Heuristic controls configuration */

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
    /* Locked: Tiered system work queue. */
    TAILQ_HEAD(__wt_tiered_qh, __wt_tiered_work_unit) tieredqh;

    WT_SPINLOCK block_lock; /* Locked: block manager list */
    TAILQ_HEAD(__wt_blockhash, __wt_block) * blockhash;
    TAILQ_HEAD(__wt_block_qh, __wt_block) blockqh;

    WT_BLKCACHE blkcache;             /* Block cache */
    WT_CHECKPOINT_CLEANUP cc_cleanup; /* Checkpoint cleanup */
    WT_CHUNKCACHE chunkcache;         /* Chunk cache */

    uint64_t *dh_bucket_count;         /* Locked: handles in each bucket */
    uint64_t dhandle_count;            /* Locked: handles in the queue */
    uint64_t dhandle_checkpoint_count; /* Locked: checkpoint handles in the queue */
    /* Locked: handles by type in the queue */
    uint64_t dhandle_types_count[WT_DHANDLE_TYPE_NUM];
    wt_shared u_int open_btree_count;        /* Locked: open writable btree count */
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
    WT_EVICT *evict;

    WT_TXN_GLOBAL txn_global; /* Global transaction state */

    /* Recovery checkpoint snapshot details saved in the metadata file */
    uint64_t recovery_ckpt_snap_min, recovery_ckpt_snap_max;
    uint64_t *recovery_ckpt_snapshot;
    uint32_t recovery_ckpt_snapshot_count;

    WT_RWLOCK hot_backup_lock; /* Hot backup serialization */
    wt_shared uint64_t
      hot_backup_start;            /* Clock value of most recent checkpoint needed by hot backup */
    uint64_t hot_backup_timestamp; /* Stable timestamp of checkpoint for the open backup */
    char **hot_backup_list;        /* Hot backup file list */
    uint32_t *partial_backup_remove_ids; /* Remove btree id list for partial backup */

    WT_CKPT_CONNECTION ckpt;

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

    uint32_t stat_flags; /* Options declared in flags.py */

    /* Connection statistics */
    uint64_t rec_maximum_hs_wrapup_milliseconds; /* Maximum milliseconds moving updates to history
                                                    store took. */
    uint64_t
      rec_maximum_image_build_milliseconds; /* Maximum milliseconds building disk image took. */
    uint64_t rec_maximum_milliseconds;      /* Maximum milliseconds reconciliation took. */
    WT_CONNECTION_STATS *stats[WT_STAT_CONN_COUNTER_SLOTS];
    WT_CONNECTION_STATS *stat_array;

    WT_CAPACITY capacity;              /* Capacity structure */
    WT_SESSION_IMPL *capacity_session; /* Capacity thread session */
    wt_thread_t capacity_tid;          /* Capacity thread */
    bool capacity_tid_set;             /* Capacity thread set */
    WT_CONDVAR *capacity_cond;         /* Capacity wait mutex */

#define WT_CONN_TIERED_STORAGE_ENABLED(conn) ((conn)->bstorage != NULL)
    WT_BUCKET_STORAGE *bstorage;     /* Bucket storage for the connection */
    WT_BUCKET_STORAGE bstorage_none; /* Bucket storage for "none" */

    WT_KEYED_ENCRYPTOR *kencryptor; /* Encryptor for metadata and log */

    bool evict_server_running; /* Eviction server operating */

    WT_THREAD_GROUP evict_threads;
    uint32_t evict_threads_max; /* Max eviction threads */
    uint32_t evict_threads_min; /* Min eviction threads */
    bool evict_sample_inmem;
    wt_shared bool evict_use_npos;
    bool evict_legacy_page_visit_strategy;

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

    WT_DISAGGREGATED_STORAGE disaggregated_storage;
    WT_PAGE_DELTA_CONFIG page_delta; /* Page delta configuration */
    WT_LAYERED_TABLE_MANAGER layered_table_manager;
    WT_PAGE_HISTORY page_history;

    bool preserve_prepared; /* Preserve prepared updates */

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

    WT_SESSION_IMPL *tiered_session;    /* Tiered thread session */
    wt_thread_t tiered_tid;             /* Tiered thread */
    bool tiered_tid_set;                /* Tiered thread set */
    WT_CONDVAR *flush_cond;             /* Flush wait mutex */
    WT_CONDVAR *tiered_cond;            /* Tiered wait mutex */
    uint64_t tiered_interval;           /* Tiered work interval */
    bool tiered_server_running;         /* Internal tiered server operating */
    wt_shared bool flush_ckpt_complete; /* Checkpoint after flush completed */
    uint64_t flush_most_recent;         /* Clock value of last flush_tier */
    uint32_t flush_state;               /* State of last flush tier */
    wt_timestamp_t flush_ts;            /* Timestamp of most recent flush_tier */

    WT_SESSION_IMPL *chunkcache_metadata_session; /* Chunk cache metadata server thread session */
    wt_thread_t chunkcache_metadata_tid;          /* Chunk cache metadata thread */
    bool chunkcache_metadata_tid_set;             /* Chunk cache metadata thread set */
    WT_CONDVAR *chunkcache_metadata_cond;         /* Chunk cache metadata wait mutex */

    WT_LOG_MANAGER log_mgr;

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

    /* Locked: page log list */
    WT_SPINLOCK page_log_lock; /* Page log list lock */
    TAILQ_HEAD(__wt_page_log_qh, __wt_named_page_log) pagelogqh;

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

    wt_shared uint64_t stashed_bytes; /* Atomic: stashed memory statistics */
    wt_shared uint64_t stashed_objects;

    /* Generations manager */
    wt_shared volatile uint64_t generations[WT_GENERATIONS];
    uint64_t gen_drain_timeout_ms; /* Maximum waiting time for a resource to drain in diagnostic
                                      mode before timing out */

    wt_off_t data_extend_len; /* file_extend data length */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_FILE_TYPE_DATA 0x1ull /* Data files */
#define WT_FILE_TYPE_LOG 0x2ull  /* Log files */
                                 /* AUTOMATIC FLAG VALUE GENERATION STOP 64 */
    uint64_t write_through;      /* FILE_FLAG_WRITE_THROUGH */

    bool mmap;     /* use mmap when reading checkpoints */
    bool mmap_all; /* use mmap for all I/O on data files */
    int page_size; /* OS page size for mmap alignment */

    /* Access to these fields is protected by the debug_log_retention_lock. */
    WT_LSN *debug_ckpt;                /* Debug mode checkpoint LSNs. */
    size_t debug_ckpt_alloc;           /* Checkpoint retention allocated. */
    wt_shared uint32_t debug_ckpt_cnt; /* Checkpoint retention number. */
    wt_shared uint32_t debug_log_cnt;  /* Log file retention count */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_DEBUG_CKPT_RETAIN 0x0001u
#define WT_CONN_DEBUG_CONFIGURATION 0x0002u
#define WT_CONN_DEBUG_CORRUPTION_ABORT 0x0004u
#define WT_CONN_DEBUG_CURSOR_COPY 0x0008u
#define WT_CONN_DEBUG_CURSOR_REPOSITION 0x0010u
#define WT_CONN_DEBUG_EVICTION_CKPT_TS_ORDERING 0x0020u
#define WT_CONN_DEBUG_EVICT_AGGRESSIVE_MODE 0x0040u
#define WT_CONN_DEBUG_REALLOC_EXACT 0x0080u
#define WT_CONN_DEBUG_REALLOC_MALLOC 0x0100u
#define WT_CONN_DEBUG_SLOW_CKPT 0x0200u
#define WT_CONN_DEBUG_STRESS_SKIPLIST 0x0400u
#define WT_CONN_DEBUG_TABLE_LOGGING 0x0800u
#define WT_CONN_DEBUG_TIERED_FLUSH_ERROR_CONTINUE 0x1000u
#define WT_CONN_DEBUG_UPDATE_RESTORE_EVICT 0x2000u
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
#define WT_TIMING_STRESS_AGGRESSIVE_STASH_FREE 0x000000001ull
#define WT_TIMING_STRESS_AGGRESSIVE_SWEEP 0x000000002ull
#define WT_TIMING_STRESS_BACKUP_RENAME 0x000000004ull
#define WT_TIMING_STRESS_CHECKPOINT_EVICT_PAGE 0x000000008ull
#define WT_TIMING_STRESS_CHECKPOINT_HANDLE 0x000000010ull
#define WT_TIMING_STRESS_CHECKPOINT_SLOW 0x000000020ull
#define WT_TIMING_STRESS_CHECKPOINT_STOP 0x000000040ull
#define WT_TIMING_STRESS_CLOSE_STRESS_LOG 0x000000080ull
#define WT_TIMING_STRESS_COMMIT_TRANSACTION_SLOW 0x000000100ull
#define WT_TIMING_STRESS_COMPACT_SLOW 0x000000200ull
#define WT_TIMING_STRESS_EVICT_REPOSITION 0x000000400ull
#define WT_TIMING_STRESS_FAILPOINT_EVICTION_SPLIT 0x000000800ull
#define WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS 0x000001000ull
#define WT_TIMING_STRESS_HS_CHECKPOINT_DELAY 0x000002000ull
#define WT_TIMING_STRESS_HS_SEARCH 0x000004000ull
#define WT_TIMING_STRESS_HS_SWEEP 0x000008000ull
#define WT_TIMING_STRESS_LIVE_RESTORE_CLEAN_UP 0x000010000ull
#define WT_TIMING_STRESS_OPEN_INDEX_SLOW 0x000020000ull
#define WT_TIMING_STRESS_PREFETCH_1 0x000040000ull
#define WT_TIMING_STRESS_PREFETCH_2 0x000080000ull
#define WT_TIMING_STRESS_PREFETCH_3 0x000100000ull
#define WT_TIMING_STRESS_PREFIX_COMPARE 0x000200000ull
#define WT_TIMING_STRESS_PREPARE_CHECKPOINT_DELAY 0x000400000ull
#define WT_TIMING_STRESS_PREPARE_RESOLUTION_1 0x000800000ull
#define WT_TIMING_STRESS_PREPARE_RESOLUTION_2 0x001000000ull
#define WT_TIMING_STRESS_SESSION_ALTER_SLOW 0x002000000ull
#define WT_TIMING_STRESS_SLEEP_BEFORE_READ_OVERFLOW_ONPAGE 0x004000000ull
#define WT_TIMING_STRESS_SPLIT_1 0x008000000ull
#define WT_TIMING_STRESS_SPLIT_2 0x010000000ull
#define WT_TIMING_STRESS_SPLIT_3 0x020000000ull
#define WT_TIMING_STRESS_SPLIT_4 0x040000000ull
#define WT_TIMING_STRESS_SPLIT_5 0x080000000ull
#define WT_TIMING_STRESS_SPLIT_6 0x100000000ull
#define WT_TIMING_STRESS_SPLIT_7 0x200000000ull
#define WT_TIMING_STRESS_SPLIT_8 0x400000000ull
#define WT_TIMING_STRESS_TIERED_FLUSH_FINISH 0x800000000ull
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
#define WT_CONN_SERVER_CAPACITY 0x0001u
#define WT_CONN_SERVER_CHECKPOINT 0x0002u
#define WT_CONN_SERVER_CHECKPOINT_CLEANUP 0x0004u
#define WT_CONN_SERVER_CHUNKCACHE_METADATA 0x0008u
#define WT_CONN_SERVER_COMPACT 0x0010u
#define WT_CONN_SERVER_EVICTION 0x0020u
#define WT_CONN_SERVER_LAYERED 0x0040u
#define WT_CONN_SERVER_LOG 0x0080u
#define WT_CONN_SERVER_PREFETCH 0x0100u
#define WT_CONN_SERVER_RTS 0x0200u
#define WT_CONN_SERVER_STATISTICS 0x0400u
#define WT_CONN_SERVER_SWEEP 0x0800u
#define WT_CONN_SERVER_TIERED 0x1000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t server_flags;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_BACKUP_PARTIAL_RESTORE 0x0001u
#define WT_CONN_CACHE_CURSORS 0x0002u
#define WT_CONN_CALL_LOG_ENABLED 0x0004u
#define WT_CONN_CKPT_CLEANUP_RECLAIM_SPACE 0x0008u
#define WT_CONN_CKPT_SYNC 0x0010u
#define WT_CONN_IN_MEMORY 0x0020u
#define WT_CONN_LIVE_RESTORE_FS 0x0040u
#define WT_CONN_PRECISE_CHECKPOINT 0x0080u
#define WT_CONN_PRESERVE_PREPARED 0x0100u
#define WT_CONN_READONLY 0x0200u
#define WT_CONN_RECOVERING 0x0400u
#define WT_CONN_RECOVERING_METADATA 0x0800u
#define WT_CONN_RECOVERY_COMPLETE 0x1000u
#define WT_CONN_SALVAGE 0x2000u
#define WT_CONN_WAS_BACKUP 0x4000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint32_t flags;

/* AUTOMATIC ATOMIC FLAG VALUE GENERATION START 0 */
#define WT_CONN_CACHE_POOL 0x00000001u
#define WT_CONN_CKPT_GATHER 0x00000002u
#define WT_CONN_CLOSING 0x00000004u
#define WT_CONN_CLOSING_CHECKPOINT 0x00000008u
#define WT_CONN_CLOSING_NO_MORE_OPENS 0x00000010u
#define WT_CONN_COMPATIBILITY 0x00000020u
#define WT_CONN_DATA_CORRUPTION 0x00000040u
#define WT_CONN_HS_OPEN 0x00000080u
#define WT_CONN_INCR_BACKUP 0x00000100u
#define WT_CONN_LEAK_MEMORY 0x00000200u
#define WT_CONN_MINIMAL 0x00000400u
#define WT_CONN_OPTRACK 0x00000800u
#define WT_CONN_PANIC 0x00001000u
#define WT_CONN_READY 0x00002000u
#define WT_CONN_RECONFIGURING 0x00004000u
#define WT_CONN_TIERED_FIRST_FLUSH 0x00008000u
    /* AUTOMATIC ATOMIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint32_t flags_atomic;
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

/*
 * WT_CONN_CLOSE_ABORT --
 *      Whenever conn->close encounters a non-zero return code, abort the process to track where it
 * came from. This is strictly to be used for debugging purposes.
 */
#define WT_CONN_CLOSE_ABORT(s, ret)                                                    \
    if (F_ISSET_ATOMIC_32(S2C(s), WT_CONN_CLOSING) && (ret != 0) && (ret != WT_PANIC)) \
        __wt_abort(s);
