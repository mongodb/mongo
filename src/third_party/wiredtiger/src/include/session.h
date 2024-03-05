/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * WT_DATA_HANDLE_CACHE --
 *	Per-session cache of handles to avoid synchronization when opening
 *	cursors.
 */
struct __wt_data_handle_cache {
    WT_DATA_HANDLE *dhandle;

    TAILQ_ENTRY(__wt_data_handle_cache) q;
    TAILQ_ENTRY(__wt_data_handle_cache) hashq;
};

/*
 * WT_HAZARD --
 *	A hazard pointer.
 */
struct __wt_hazard {
    wt_shared WT_REF *ref; /* Page reference */
#ifdef HAVE_DIAGNOSTIC
    const char *func; /* Function/line hazard acquired */
    int line;
#endif
};

/*
 * WT_HAZARD_ARRAY --
 *   An array of all hazard pointers held by the session.
 *   New hazard pointers are added on a first-fit basis, and on removal their entry
 *   in the array is set to null. As such this array may contain holes.
 */
struct __wt_hazard_array {
/* The hazard pointer array grows as necessary, initialize with 250 slots. */
#define WT_SESSION_INITIAL_HAZARD_SLOTS 250

    wt_shared WT_HAZARD *arr; /* The hazard pointer array */
    wt_shared uint32_t inuse; /* Number of array slots potentially in-use. We only need to iterate
                                 this many slots to find all active pointers */
    wt_shared uint32_t num_active; /* Number of array slots containing an active hazard pointer */
    uint32_t size;                 /* Allocated size of the array */
};

/*
 * WT_PREFETCH --
 *	Pre-fetch structure containing useful information for pre-fetch.
 */
struct __wt_prefetch {
    WT_PAGE *prefetch_prev_ref_home;
    uint64_t prefetch_disk_read_count; /* Sequential cache requests that caused a leaf read */
    uint64_t prefetch_skipped_with_parent;
};

/* Get the connection implementation for a session */
#define S2C(session) ((WT_CONNECTION_IMPL *)((WT_SESSION_IMPL *)(session))->iface.connection)

/* Get the btree for a session */
#define S2BT(session) ((WT_BTREE *)(session)->dhandle->handle)
#define S2BT_SAFE(session) ((session)->dhandle == NULL ? NULL : S2BT(session))

/* Get the file system for a session */
#define S2FS(session)                                                \
    ((session)->bucket_storage == NULL ? S2C(session)->file_system : \
                                         (session)->bucket_storage->file_system)

typedef TAILQ_HEAD(__wt_cursor_list, __wt_cursor) WT_CURSOR_LIST;

/* Number of cursors cached to trigger cursor sweep. */
#define WT_SESSION_CURSOR_SWEEP_COUNTDOWN 40

/* Minimum number of buckets to visit during a regular cursor sweep. */
#define WT_SESSION_CURSOR_SWEEP_MIN 5

/* Maximum number of buckets to visit during a regular cursor sweep. */
#define WT_SESSION_CURSOR_SWEEP_MAX 64

/* Invalid session ID. */
#define WT_SESSION_ID_INVALID 0xffffffff

/* A fake session ID for when we need to refer to a session that is actually NULL. */
#define WT_SESSION_ID_NULL 0xfffffffe

/*
 * WT_SESSION_IMPL --
 *	Implementation of WT_SESSION.
 */
struct __wt_session_impl {
    WT_SESSION iface;
    WT_EVENT_HANDLER *event_handler; /* Application's event handlers */

    void *lang_private; /* Language specific private storage */

    void (*format_private)(WT_CURSOR *, int, void *); /* Format test program private callback. */
    void *format_private_arg;

    u_int active; /* Non-zero if the session is in-use */

    const char *name;   /* Name */
    const char *lastop; /* Last operation */
    uint32_t id;        /* UID, offset in session array */

    uint64_t cache_wait_us;        /* Wait time for cache for current operation */
    uint64_t operation_start_us;   /* Operation start */
    uint64_t operation_timeout_us; /* Maximum operation period before rollback */
    u_int api_call_counter;        /* Depth of api calls */

    wt_shared WT_DATA_HANDLE *dhandle; /* Current data handle */
    WT_BUCKET_STORAGE *bucket_storage; /* Current bucket storage and file system */

    /*
     * Each session keeps a cache of data handles. The set of handles can grow quite large so we
     * maintain both a simple list and a hash table of lists. The hash table key is based on a hash
     * of the data handle's URI. Though all hash entries are discarded on session close, the hash
     * table list itself is kept in allocated memory that lives across session close - so it is
     * declared further down.
     */
    /* Session handle reference list */
    TAILQ_HEAD(__dhandles, __wt_data_handle_cache) dhandles;
    uint64_t last_sweep;        /* Last sweep for dead handles */
    struct timespec last_epoch; /* Last epoch time returned */

    WT_CURSOR_LIST cursors;          /* Cursors closed with the session */
    u_int ncursors;                  /* Count of active file cursors. */
    uint32_t cursor_sweep_countdown; /* Countdown to cursor sweep */
    uint32_t cursor_sweep_position;  /* Position in cursor_cache for sweep */
    uint64_t last_cursor_big_sweep;  /* Last big sweep for dead cursors */
    uint64_t last_cursor_sweep;      /* Last regular sweep for dead cursors */
    u_int sweep_warning_5min;        /* Whether the session was without sweep for 5 min. */
    u_int sweep_warning_60min;       /* Whether the session was without sweep for 60 min. */

    WT_CURSOR_BACKUP *bkp_cursor; /* Hot backup cursor */

    WT_COMPACT_STATE *compact; /* Compaction information */
    enum { WT_COMPACT_NONE = 0, WT_COMPACT_RUNNING, WT_COMPACT_SUCCESS } compact_state;

    WT_IMPORT_LIST *import_list; /* List of metadata entries to import from file. */

    u_int hs_cursor_counter; /* Number of open history store cursors */

    WT_CURSOR *meta_cursor;  /* Metadata file */
    void *meta_track;        /* Metadata operation tracking */
    void *meta_track_next;   /* Current position */
    void *meta_track_sub;    /* Child transaction / save point */
    size_t meta_track_alloc; /* Currently allocated */
    int meta_track_nest;     /* Nesting level of meta transaction */
#define WT_META_TRACKING(session) ((session)->meta_track_next != NULL)

    /* Current rwlock for callback. */
    WT_RWLOCK *current_rwlock;
    uint8_t current_rwticket;

    WT_ITEM **scratch;     /* Temporary memory for any function */
    u_int scratch_alloc;   /* Currently allocated */
    size_t scratch_cached; /* Scratch bytes cached */
#ifdef HAVE_DIAGNOSTIC

    /* Enforce the contract that a session is only used by a single thread at a time. */
    struct __wt_thread_check {
        WT_SPINLOCK lock;
        uintmax_t owning_thread;
        uint32_t entry_count;
    } thread_check;

    /*
     * It's hard to figure out from where a buffer was allocated after it's leaked, so in diagnostic
     * mode we track them; DIAGNOSTIC can't simply add additional fields to WT_ITEM structures
     * because they are visible to applications, create a parallel structure instead.
     */
    struct __wt_scratch_track {
        const char *func; /* Allocating function, line */
        int line;
    } * scratch_track;
#endif

    /* Record the important timestamps of each stage in an reconciliation. */
    struct __wt_reconcile_timeline {
        uint64_t reconcile_start;
        uint64_t image_build_start;
        uint64_t image_build_finish;
        uint64_t hs_wrapup_start;
        uint64_t hs_wrapup_finish;
        uint64_t reconcile_finish;
        uint64_t total_reentry_hs_eviction_time;
    } reconcile_timeline;

    /*
     * Record the important timestamps of each stage in an eviction. If an eviction takes a long
     * time and times out, we can trace the time usage of each stage from this information.
     */
    struct __wt_evict_timeline {
        uint64_t evict_start;
        uint64_t reentry_hs_evict_start;
        uint64_t reentry_hs_evict_finish;
        uint64_t evict_finish;
        bool reentry_hs_eviction;
    } evict_timeline;

    WT_ITEM err; /* Error buffer */

    WT_TXN_ISOLATION isolation;
    WT_TXN *txn; /* Transaction state */

    WT_PREFETCH pf; /* Pre-fetch structure */

    void *block_manager; /* Block-manager support */
    int (*block_manager_cleanup)(WT_SESSION_IMPL *);

    const char *hs_checkpoint;     /* History store checkpoint name, during checkpoint cursor ops */
    uint64_t checkpoint_write_gen; /* Write generation override, during checkpoint cursor ops */

    /* Checkpoint handles */
    WT_DATA_HANDLE **ckpt_handle; /* Handle list */
    u_int ckpt_handle_next;       /* Next empty slot */
    size_t ckpt_handle_allocated; /* Bytes allocated */

    /* Named checkpoint drop list, during a checkpoint */
    WT_ITEM *ckpt_drop_list;

    /* Checkpoint time of current checkpoint, during a checkpoint */
    uint64_t current_ckpt_sec;

    /*
     * Operations acting on handles.
     *
     * The preferred pattern is to gather all of the required handles at the beginning of an
     * operation, then drop any other locks, perform the operation, then release the handles. This
     * cannot be easily merged with the list of checkpoint handles because some operations (such as
     * compact) do checkpoints internally.
     */
    WT_DATA_HANDLE **op_handle; /* Handle list */
    u_int op_handle_next;       /* Next empty slot */
    size_t op_handle_allocated; /* Bytes allocated */

    void *reconcile; /* Reconciliation support */
    int (*reconcile_cleanup)(WT_SESSION_IMPL *);

    /* Salvage support. */
    void *salvage_track;

    /* Sessions have an associated statistics bucket based on its ID. */
    u_int stat_bucket;          /* Statistics bucket offset */
    uint64_t cache_max_wait_us; /* Maximum time an operation waits for space in cache */

#ifdef HAVE_DIAGNOSTIC
    uint8_t dump_raw; /* Configure debugging page dump */
#endif

#ifdef HAVE_UNITTEST_ASSERTS
/*
 * Unit testing assertions requires overriding abort logic and instead capturing this information to
 * be checked by the unit test.
 */
#define WT_SESSION_UNITTEST_BUF_LEN 100
    bool unittest_assert_hit;
    char unittest_assert_msg[WT_SESSION_UNITTEST_BUF_LEN];
#endif

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_SESSION_LOCKED_CHECKPOINT 0x0001u
#define WT_SESSION_LOCKED_HANDLE_LIST_READ 0x0002u
#define WT_SESSION_LOCKED_HANDLE_LIST_WRITE 0x0004u
#define WT_SESSION_LOCKED_HOTBACKUP_READ 0x0008u
#define WT_SESSION_LOCKED_HOTBACKUP_WRITE 0x0010u
#define WT_SESSION_LOCKED_METADATA 0x0020u
#define WT_SESSION_LOCKED_PASS 0x0040u
#define WT_SESSION_LOCKED_SCHEMA 0x0080u
#define WT_SESSION_LOCKED_SLOT 0x0100u
#define WT_SESSION_LOCKED_TABLE_READ 0x0200u
#define WT_SESSION_LOCKED_TABLE_WRITE 0x0400u
#define WT_SESSION_LOCKED_TURTLE 0x0800u
#define WT_SESSION_NO_SCHEMA_LOCK 0x1000u
    /*AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t lock_flags;

/*
 * Note: The WT_SESSION_PREFETCH_THREAD flag is set for prefetch server threads whereas the
 * WT_SESSION_PREFETCH_THREAD flag is set when prefetch has been enabled on the session.
 */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_SESSION_BACKUP_CURSOR 0x000001u
#define WT_SESSION_BACKUP_DUP 0x000002u
#define WT_SESSION_CACHE_CURSORS 0x000004u
#define WT_SESSION_CAN_WAIT 0x000008u
#define WT_SESSION_DEBUG_CHECKPOINT_FAIL_BEFORE_TURTLE_UPDATE 0x000010u
#define WT_SESSION_DEBUG_DO_NOT_CLEAR_TXN_ID 0x000020u
#define WT_SESSION_DEBUG_RELEASE_EVICT 0x000040u
#define WT_SESSION_EVICTION 0x000080u
#define WT_SESSION_IGNORE_CACHE_SIZE 0x000100u
#define WT_SESSION_IMPORT 0x000200u
#define WT_SESSION_IMPORT_REPAIR 0x000400u
#define WT_SESSION_INTERNAL 0x000800u
#define WT_SESSION_LOGGING_INMEM 0x001000u
#define WT_SESSION_NO_DATA_HANDLES 0x002000u
#define WT_SESSION_NO_RECONCILE 0x004000u
#define WT_SESSION_PREFETCH_ENABLED 0x008000u
#define WT_SESSION_PREFETCH_THREAD 0x010000u
#define WT_SESSION_QUIET_CORRUPT_FILE 0x020000u
#define WT_SESSION_READ_WONT_NEED 0x040000u
#define WT_SESSION_RESOLVING_TXN 0x080000u
#define WT_SESSION_ROLLBACK_TO_STABLE 0x100000u
#define WT_SESSION_SCHEMA_TXN 0x200000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;

/*
 * All of the following fields live at the end of the structure so it's easier to clear everything
 * but the fields that persist.
 */
#define WT_SESSION_CLEAR_SIZE (offsetof(WT_SESSION_IMPL, rnd))

    /*
     * The random number state persists past session close because we don't want to repeatedly use
     * the same values for skiplist depth when the application isn't caching sessions.
     */
    wt_shared WT_RAND_STATE rnd; /* Random number generation state */

    /*
     * Hash tables are allocated lazily as sessions are used to keep the size of this structure from
     * growing too large.
     */
    WT_CURSOR_LIST *cursor_cache; /* Hash table of cached cursors */

    /* Hashed handle reference list array */
    TAILQ_HEAD(__dhandles_hash, __wt_data_handle_cache) * dhhash;

/* Generations manager */
#define WT_GEN_CHECKPOINT 0   /* Checkpoint generation */
#define WT_GEN_EVICT 1        /* Eviction generation */
#define WT_GEN_HAS_SNAPSHOT 2 /* Snapshot generation */
#define WT_GEN_HAZARD 3       /* Hazard pointer */
#define WT_GEN_SPLIT 4        /* Page splits */
#define WT_GENERATIONS 5      /* Total generation manager entries */
    wt_shared volatile uint64_t generations[WT_GENERATIONS];

    /*
     * Bindings for compiled configurations.
     */
    WT_CONF_BINDINGS conf_bindings;

    /*
     * Session memory persists past session close because it's accessed by threads of control other
     * than the thread owning the session. For example, btree splits and hazard pointers can "free"
     * memory that's still in use. In order to eventually free it, it's stashed here with its
     * generation number; when no thread is reading in generation, the memory can be freed for real.
     */
    struct __wt_session_stash {
        struct __wt_stash {
            void *p; /* Memory, length */
            size_t len;
            uint64_t gen; /* Generation */
        } * list;
        size_t cnt;   /* Array entries */
        size_t alloc; /* Allocated bytes */
    } stash[WT_GENERATIONS];

/*
 * Hazard pointers.
 *
 * Hazard information persists past session close because it's accessed by threads of control other
 * than the thread owning the session.
 *
 * Use the non-NULL state of the hazard array to know if the session has previously been
 * initialized.
 */
#define WT_SESSION_FIRST_USE(s) ((s)->hazards.arr == NULL)
    WT_HAZARD_ARRAY hazards;

    /*
     * Operation tracking.
     */
    WT_OPTRACK_RECORD *optrack_buf;
    u_int optrackbuf_ptr;
    uint64_t optrack_offset;
    WT_FH *optrack_fh;

    WT_SESSION_STATS stats;
};

/* Consider moving this to session_inline.h if it ever appears. */
#define WT_READING_CHECKPOINT(s)                                       \
    ((s)->dhandle != NULL && F_ISSET((s)->dhandle, WT_DHANDLE_OPEN) && \
      WT_DHANDLE_IS_CHECKPOINT((s)->dhandle))
