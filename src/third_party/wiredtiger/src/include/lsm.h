/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_LSM_WORKER_COOKIE --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_cookie {
    WT_LSM_CHUNK **chunk_array;
    size_t chunk_alloc;
    u_int nchunks;
};

/*
 * WT_LSM_WORKER_ARGS --
 *	State for an LSM worker thread.
 */
struct __wt_lsm_worker_args {
    WT_SESSION_IMPL *session; /* Session */
    WT_CONDVAR *work_cond;    /* Owned by the manager */

    wt_thread_t tid; /* Thread id */
    bool tid_set;    /* Thread id set */

    u_int id;      /* My manager slot id */
    uint32_t type; /* Types of operations handled */

    volatile bool running; /* Worker is running */
};

/*
 * WT_LSM_CURSOR_CHUNK --
 *	Iterator struct containing all the LSM cursor access points for a chunk.
 */
struct __wt_lsm_cursor_chunk {
    WT_BLOOM *bloom;     /* Bloom filter handle for each chunk.*/
    WT_CURSOR *cursor;   /* Cursor handle for each chunk. */
    uint64_t count;      /* Number of items in chunk */
    uint64_t switch_txn; /* Switch txn for each chunk */
};

/*
 * WT_CURSOR_LSM --
 *	An LSM cursor.
 */
struct __wt_cursor_lsm {
    WT_CURSOR iface;

    WT_LSM_TREE *lsm_tree;
    uint64_t dsk_gen;

    u_int nchunks;               /* Number of chunks in the cursor */
    u_int nupdates;              /* Updates needed (including
                                    snapshot isolation checks). */
    WT_CURSOR *current;          /* The current cursor for iteration */
    WT_LSM_CHUNK *primary_chunk; /* The current primary chunk */

    WT_LSM_CURSOR_CHUNK **chunks; /* Array of LSM cursor units */
    size_t chunks_alloc;          /* Current size iterators array */
    size_t chunks_count;          /* Current number of iterators */

    u_int update_count; /* Updates performed. */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_CLSM_ACTIVE 0x001u        /* Incremented the session count */
#define WT_CLSM_BULK 0x002u          /* Open for snapshot isolation */
#define WT_CLSM_ITERATE_NEXT 0x004u  /* Forward iteration */
#define WT_CLSM_ITERATE_PREV 0x008u  /* Backward iteration */
#define WT_CLSM_MERGE 0x010u         /* Merge cursor, don't update */
#define WT_CLSM_MINOR_MERGE 0x020u   /* Minor merge, include tombstones */
#define WT_CLSM_MULTIPLE 0x040u      /* Multiple cursors have values */
#define WT_CLSM_OPEN_READ 0x080u     /* Open for reads */
#define WT_CLSM_OPEN_SNAPSHOT 0x100u /* Open for snapshot isolation */
                                     /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_LSM_CHUNK --
 *	A single chunk (file) in an LSM tree.
 */
struct __wt_lsm_chunk {
    const char *uri;             /* Data source for this chunk */
    const char *bloom_uri;       /* URI of Bloom filter, if any */
    struct timespec create_time; /* Creation time (for rate limiting) */
    uint64_t count;              /* Approximate count of records */
    uint64_t size;               /* Final chunk size */

    uint64_t switch_txn;             /*
                                      * Largest transaction that can write
                                      * to this chunk, set by a worker
                                      * thread when the chunk is switched
                                      * out, or by compact to get the most
                                      * recent chunk flushed.
                                      */
    wt_timestamp_t switch_timestamp; /*
                                      * The timestamp used to decide when
                                      * updates need to detect conflicts.
                                      */
    WT_SPINLOCK timestamp_spinlock;

    uint32_t id;            /* ID used to generate URIs */
    uint32_t generation;    /* Merge generation */
    uint32_t refcnt;        /* Number of worker thread references */
    uint32_t bloom_busy;    /* Currently creating bloom filter */
    uint32_t evict_enabled; /* Eviction allowed on the chunk */

    int8_t empty;     /* 1/0: checkpoint missing */
    int8_t evicted;   /* 1/0: in-memory chunk was evicted */
    uint8_t flushing; /* 1/0: chunk flush in progress */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_CHUNK_BLOOM 0x01u
#define WT_LSM_CHUNK_HAS_TIMESTAMP 0x02u
#define WT_LSM_CHUNK_MERGING 0x04u
#define WT_LSM_CHUNK_ONDISK 0x08u
#define WT_LSM_CHUNK_STABLE 0x10u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * Different types of work units. Used by LSM worker threads to choose which type of work they will
 * execute, and by work units to define which action is required.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_WORK_BLOOM 0x01u        /* Create a bloom filter */
#define WT_LSM_WORK_DROP 0x02u         /* Drop unused chunks */
#define WT_LSM_WORK_ENABLE_EVICT 0x04u /* Allow eviction of pinned chunk */
#define WT_LSM_WORK_FLUSH 0x08u        /* Flush a chunk to disk */
#define WT_LSM_WORK_MERGE 0x10u        /* Look for a tree merge */
#define WT_LSM_WORK_SWITCH 0x20u       /* Switch the in-memory chunk */
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* Work units that are serviced by general worker threads. */
#define WT_LSM_WORK_GENERAL_OPS                                                            \
    (WT_LSM_WORK_BLOOM | WT_LSM_WORK_DROP | WT_LSM_WORK_ENABLE_EVICT | WT_LSM_WORK_FLUSH | \
      WT_LSM_WORK_SWITCH)

/*
 * WT_LSM_WORK_UNIT --
 *	A definition of maintenance that an LSM tree needs done.
 */
struct __wt_lsm_work_unit {
    TAILQ_ENTRY(__wt_lsm_work_unit) q; /* Worker unit queue */
    uint32_t type;                     /* Type of operation */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_WORK_FORCE 0x1u /* Force operation */
                               /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;            /* Flags for operation */
    WT_LSM_TREE *lsm_tree;
};

/*
 * WT_LSM_MANAGER --
 *	A structure that holds resources used to manage any LSM trees in a
 *	database.
 */
struct __wt_lsm_manager {
    /*
     * Queues of work units for LSM worker threads. We maintain three
     * queues, to allow us to keep each queue FIFO, rather than needing
     * to manage the order of work by shuffling the queue order.
     * One queue for switches - since switches should never wait for other
     *   work to be done.
     * One queue for application requested work. For example flushing
     *   and creating bloom filters.
     * One queue that is for longer running operations such as merges.
     */
    TAILQ_HEAD(__wt_lsm_work_switch_qh, __wt_lsm_work_unit) switchqh;
    TAILQ_HEAD(__wt_lsm_work_app_qh, __wt_lsm_work_unit) appqh;
    TAILQ_HEAD(__wt_lsm_work_manager_qh, __wt_lsm_work_unit) managerqh;
    WT_SPINLOCK switch_lock;  /* Lock for switch queue */
    WT_SPINLOCK app_lock;     /* Lock for application queue */
    WT_SPINLOCK manager_lock; /* Lock for manager queue */
    WT_CONDVAR *work_cond;    /* Used to notify worker of activity */
    uint32_t lsm_workers;     /* Current number of LSM workers */
    uint32_t lsm_workers_max;
#define WT_LSM_MAX_WORKERS 20
#define WT_LSM_MIN_WORKERS 3
    WT_LSM_WORKER_ARGS lsm_worker_cookies[WT_LSM_MAX_WORKERS];

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_MANAGER_SHUTDOWN 0x1u /* Manager has shut down */
                                     /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * The value aggressive needs to get to before it influences how merges are chosen. The default
 * value translates to enough level 0 chunks being generated to create a second level merge.
 */
#define WT_LSM_AGGRESSIVE_THRESHOLD 2

/*
 * The minimum size for opening a tree: three chunks, plus one page for each participant in up to
 * three concurrent merges.
 */
#define WT_LSM_TREE_MINIMUM_SIZE(chunk_size, merge_max, maxleafpage) \
    (3 * (chunk_size) + 3 * ((merge_max) * (maxleafpage)))

/*
 * WT_LSM_TREE --
 *	An LSM tree.
 */
struct __wt_lsm_tree {
    const char *name, *config, *filename;
    const char *key_format, *value_format;
    const char *bloom_config, *file_config;

    uint32_t custom_generation; /* Level at which a custom data source
                                   should be used for merges. */
    const char *custom_prefix;  /* Prefix for custom data source */
    const char *custom_suffix;  /* Suffix for custom data source */

    WT_COLLATOR *collator;
    const char *collator_name;
    int collator_owned;

    uint32_t refcnt;               /* Number of users of the tree */
    WT_SESSION_IMPL *excl_session; /* Session has exclusive lock */

#define LSM_TREE_MAX_QUEUE 100
    uint32_t queue_ref;
    WT_RWLOCK rwlock;
    TAILQ_ENTRY(__wt_lsm_tree) q;

    uint64_t dsk_gen;

    uint64_t ckpt_throttle;                /* Rate limiting due to checkpoints */
    uint64_t merge_throttle;               /* Rate limiting due to merges */
    uint64_t chunk_fill_ms;                /* Estimate of time to fill a chunk */
    struct timespec last_flush_time;       /* Time last flush finished */
    uint64_t chunks_flushed;               /* Count of chunks flushed since open */
    struct timespec merge_aggressive_time; /* Time for merge aggression */
    uint64_t merge_progressing;            /* Bumped when merges are active */
    uint32_t merge_syncing;                /* Bumped when merges are syncing */
    struct timespec last_active;           /* Time last work unit added */
    uint64_t mgr_work_count;               /* Manager work count */
    uint64_t work_count;                   /* Work units added */

    /* Configuration parameters */
    uint32_t bloom_bit_count;
    uint32_t bloom_hash_count;
    uint32_t chunk_count_limit; /* Limit number of chunks */
    uint64_t chunk_size;
    uint64_t chunk_max; /* Maximum chunk a merge creates */
    u_int merge_min, merge_max;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_BLOOM_MERGED 0x1u
#define WT_LSM_BLOOM_OFF 0x2u
#define WT_LSM_BLOOM_OLDEST 0x4u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t bloom; /* Bloom creation policy */

    WT_LSM_CHUNK **chunk; /* Array of active LSM chunks */
    size_t chunk_alloc;   /* Space allocated for chunks */
    uint32_t nchunks;     /* Number of active chunks */
    uint32_t last;        /* Last allocated ID */
    bool modified;        /* Have there been updates? */

    WT_LSM_CHUNK **old_chunks;     /* Array of old LSM chunks */
    size_t old_alloc;              /* Space allocated for old chunks */
    u_int nold_chunks;             /* Number of old chunks */
    uint32_t freeing_old_chunks;   /* Whether chunks are being freed */
    uint32_t merge_aggressiveness; /* Increase amount of work per merge */

/*
 * We maintain a set of statistics outside of the normal statistics area, copying them into place
 * when a statistics cursor is created.
 */
#define WT_LSM_TREE_STAT_INCR(session, fld) \
    do {                                    \
        if (WT_STAT_ENABLED(session))       \
            ++(fld);                        \
    } while (0)
#define WT_LSM_TREE_STAT_INCRV(session, fld, v) \
    do {                                        \
        if (WT_STAT_ENABLED(session))           \
            (fld) += (int64_t)(v);              \
    } while (0)
    int64_t bloom_false_positive;
    int64_t bloom_hit;
    int64_t bloom_miss;
    int64_t lsm_checkpoint_throttle;
    int64_t lsm_lookup_no_bloom;
    int64_t lsm_merge_throttle;

    /*
     * Following fields used to be flags but are susceptible to races. Don't merge them with flags.
     */
    bool active;                   /* The tree is open for business */
    bool aggressive_timer_enabled; /* Timer for merge aggression enabled */
    bool need_switch;              /* New chunk needs creating */

/*
 * flags here are not protected for concurrent access, don't put anything here that is susceptible
 * to races.
 */
/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_LSM_TREE_COMPACTING 0x1u /* Tree being compacted */
#define WT_LSM_TREE_MERGES 0x2u     /* Tree should run merges */
#define WT_LSM_TREE_OPEN 0x4u       /* The tree is open */
#define WT_LSM_TREE_THROTTLE 0x8u   /* Throttle updates */
                                    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_LSM_DATA_SOURCE --
 *	Implementation of the WT_DATA_SOURCE interface for LSM.
 */
struct __wt_lsm_data_source {
    WT_DATA_SOURCE iface;

    WT_RWLOCK *rwlock;
};
