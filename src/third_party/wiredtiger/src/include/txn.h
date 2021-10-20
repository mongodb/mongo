/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define WT_TXN_NONE 0             /* No txn running in a session. */
#define WT_TXN_FIRST 1            /* First transaction to run. */
#define WT_TXN_ABORTED UINT64_MAX /* Update rolled back, ignore. */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TXN_LOG_CKPT_CLEANUP 0x01u
#define WT_TXN_LOG_CKPT_PREPARE 0x02u
#define WT_TXN_LOG_CKPT_START 0x04u
#define WT_TXN_LOG_CKPT_STOP 0x08u
#define WT_TXN_LOG_CKPT_SYNC 0x10u
/* AUTOMATIC FLAG VALUE GENERATION STOP */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TXN_OLDEST_STRICT 0x1u
#define WT_TXN_OLDEST_WAIT 0x2u
/* AUTOMATIC FLAG VALUE GENERATION STOP */

/*
 * Transaction ID comparison dealing with edge cases.
 *
 * WT_TXN_ABORTED is the largest possible ID (never visible to a running
 * transaction), WT_TXN_NONE is smaller than any possible ID (visible to all
 * running transactions).
 */
#define WT_TXNID_LE(t1, t2) ((t1) <= (t2))

#define WT_TXNID_LT(t1, t2) ((t1) < (t2))

#define WT_SESSION_TXN_STATE(s) (&S2C(s)->txn_global.states[(s)->id])

#define WT_SESSION_IS_CHECKPOINT(s) ((s)->id != 0 && (s)->id == S2C(s)->txn_global.checkpoint_id)

#define WT_TS_NONE 0         /* Beginning of time */
#define WT_TS_MAX UINT64_MAX /* End of time */

#define WT_TS_HEX_SIZE (2 * sizeof(wt_timestamp_t) + 1)

/*
 * Perform an operation at the specified isolation level.
 *
 * This is fiddly: we can't cope with operations that begin transactions
 * (leaving an ID allocated), and operations must not move our published
 * snap_min forwards (or updates we need could be freed while this operation is
 * in progress).  Check for those cases: the bugs they cause are hard to debug.
 */
#define WT_WITH_TXN_ISOLATION(s, iso, op)                                 \
    do {                                                                  \
        WT_TXN_ISOLATION saved_iso = (s)->isolation;                      \
        WT_TXN_ISOLATION saved_txn_iso = (s)->txn.isolation;              \
        WT_TXN_STATE *txn_state = WT_SESSION_TXN_STATE(s);                \
        WT_TXN_STATE saved_state = *txn_state;                            \
        (s)->txn.forced_iso++;                                            \
        (s)->isolation = (s)->txn.isolation = (iso);                      \
        op;                                                               \
        (s)->isolation = saved_iso;                                       \
        (s)->txn.isolation = saved_txn_iso;                               \
        WT_ASSERT((s), (s)->txn.forced_iso > 0);                          \
        (s)->txn.forced_iso--;                                            \
        WT_ASSERT((s), txn_state->id == saved_state.id &&                 \
            (txn_state->metadata_pinned == saved_state.metadata_pinned || \
                         saved_state.metadata_pinned == WT_TXN_NONE) &&   \
            (txn_state->pinned_id == saved_state.pinned_id ||             \
                         saved_state.pinned_id == WT_TXN_NONE));          \
        txn_state->metadata_pinned = saved_state.metadata_pinned;         \
        txn_state->pinned_id = saved_state.pinned_id;                     \
    } while (0)

struct __wt_named_snapshot {
    const char *name;

    TAILQ_ENTRY(__wt_named_snapshot) q;

    uint64_t id, pinned_id, snap_min, snap_max;
    uint64_t *snapshot;
    uint32_t snapshot_count;
};

struct __wt_txn_state {
    WT_CACHE_LINE_PAD_BEGIN
    volatile uint64_t id;
    volatile uint64_t pinned_id;
    volatile uint64_t metadata_pinned;

    WT_CACHE_LINE_PAD_END
};

struct __wt_txn_global {
    volatile uint64_t current; /* Current transaction ID. */

    /* The oldest running transaction ID (may race). */
    volatile uint64_t last_running;

    /*
     * The oldest transaction ID that is not yet visible to some transaction in the system.
     */
    volatile uint64_t oldest_id;

    wt_timestamp_t commit_timestamp;
    wt_timestamp_t last_ckpt_timestamp;
    wt_timestamp_t meta_ckpt_timestamp;
    wt_timestamp_t oldest_timestamp;
    wt_timestamp_t pinned_timestamp;
    wt_timestamp_t recovery_timestamp;
    wt_timestamp_t stable_timestamp;
    bool has_commit_timestamp;
    bool has_oldest_timestamp;
    bool has_pinned_timestamp;
    bool has_stable_timestamp;
    bool oldest_is_pinned;
    bool stable_is_pinned;

    WT_SPINLOCK id_lock;

    /* Protects the active transaction states. */
    WT_RWLOCK rwlock;

    /* Protects logging, checkpoints and transaction visibility. */
    WT_RWLOCK visibility_rwlock;

    /* List of transactions sorted by commit timestamp. */
    WT_RWLOCK commit_timestamp_rwlock;
    TAILQ_HEAD(__wt_txn_cts_qh, __wt_txn) commit_timestamph;
    uint32_t commit_timestampq_len;

    /* List of transactions sorted by read timestamp. */
    WT_RWLOCK read_timestamp_rwlock;
    TAILQ_HEAD(__wt_txn_rts_qh, __wt_txn) read_timestamph;
    uint32_t read_timestampq_len;

    /*
     * Track information about the running checkpoint. The transaction
     * snapshot used when checkpointing are special. Checkpoints can run
     * for a long time so we keep them out of regular visibility checks.
     * Eviction and checkpoint operations know when they need to be aware
     * of checkpoint transactions.
     *
     * We rely on the fact that (a) the only table a checkpoint updates is
     * the metadata; and (b) once checkpoint has finished reading a table,
     * it won't revisit it.
     */
    volatile bool checkpoint_running;    /* Checkpoint running */
    volatile uint32_t checkpoint_id;     /* Checkpoint's session ID */
    WT_TXN_STATE checkpoint_state;       /* Checkpoint's txn state */
    wt_timestamp_t checkpoint_timestamp; /* Checkpoint's timestamp */

    volatile uint64_t debug_ops;       /* Debug mode op counter */
    uint64_t debug_rollback;           /* Debug mode rollback */
    volatile uint64_t metadata_pinned; /* Oldest ID for metadata */

    /* Named snapshot state. */
    WT_RWLOCK nsnap_rwlock;
    volatile uint64_t nsnap_oldest_id;
    TAILQ_HEAD(__wt_nsnap_qh, __wt_named_snapshot) nsnaph;

    WT_TXN_STATE *states; /* Per-session transaction states */
};

typedef enum __wt_txn_isolation {
    WT_ISO_READ_COMMITTED,
    WT_ISO_READ_UNCOMMITTED,
    WT_ISO_SNAPSHOT
} WT_TXN_ISOLATION;

/*
 * WT_TXN_OP --
 *	A transactional operation.  Each transaction builds an in-memory array
 *	of these operations as it runs, then uses the array to either write log
 *	records during commit or undo the operations during rollback.
 */
struct __wt_txn_op {
    WT_BTREE *btree;
    enum {
        WT_TXN_OP_NONE = 0,
        WT_TXN_OP_BASIC_COL,
        WT_TXN_OP_BASIC_ROW,
        WT_TXN_OP_INMEM_COL,
        WT_TXN_OP_INMEM_ROW,
        WT_TXN_OP_REF_DELETE,
        WT_TXN_OP_TRUNCATE_COL,
        WT_TXN_OP_TRUNCATE_ROW
    } type;
    union {
        /* WT_TXN_OP_BASIC_ROW, WT_TXN_OP_INMEM_ROW */
        struct {
            WT_UPDATE *upd;
            WT_ITEM key;
        } op_row;

        /* WT_TXN_OP_BASIC_COL, WT_TXN_OP_INMEM_COL */
        struct {
            WT_UPDATE *upd;
            uint64_t recno;
        } op_col;
/*
 * upd is pointing to same memory in both op_row and op_col, so for simplicity just chose op_row upd
 */
#undef op_upd
#define op_upd op_row.upd

        /* WT_TXN_OP_REF_DELETE */
        WT_REF *ref;
        /* WT_TXN_OP_TRUNCATE_COL */
        struct {
            uint64_t start, stop;
        } truncate_col;
        /* WT_TXN_OP_TRUNCATE_ROW */
        struct {
            WT_ITEM start, stop;
            enum {
                WT_TXN_TRUNC_ALL,
                WT_TXN_TRUNC_BOTH,
                WT_TXN_TRUNC_START,
                WT_TXN_TRUNC_STOP
            } mode;
        } truncate_row;
    } u;
};

/*
 * WT_TXN --
 *	Per-session transaction context.
 */
struct __wt_txn {
    uint64_t id;

    WT_TXN_ISOLATION isolation;

    uint32_t forced_iso; /* Isolation is currently forced. */

    /*
     * Snapshot data:
     *	ids < snap_min are visible,
     *	ids > snap_max are invisible,
     *	everything else is visible unless it is in the snapshot.
     */
    uint64_t snap_min, snap_max;
    uint64_t *snapshot;
    uint32_t snapshot_count;
    uint32_t txn_logsync; /* Log sync configuration */

    /*
     * Timestamp copied into updates created by this transaction.
     *
     * In some use cases, this can be updated while the transaction is
     * running.
     */
    wt_timestamp_t commit_timestamp;

    /*
     * Set to the first commit timestamp used in the transaction and fixed while the transaction is
     * on the public list of committed timestamps.
     */
    wt_timestamp_t first_commit_timestamp;

    /*
     * Timestamp copied into updates created by this transaction, when this transaction is prepared.
     */
    wt_timestamp_t prepare_timestamp;

    /* Read updates committed as of this timestamp. */
    wt_timestamp_t read_timestamp;

    TAILQ_ENTRY(__wt_txn) commit_timestampq;
    TAILQ_ENTRY(__wt_txn) read_timestampq;
    bool clear_commit_q; /* Set if need to clear from the commit queue */
    bool clear_read_q;   /* Set if need to clear from the read queue */

    /* Array of modifications by this transaction. */
    WT_TXN_OP *mod;
    size_t mod_alloc;
    u_int mod_count;
#ifdef HAVE_DIAGNOSTIC
    /*
     * Reference count of multiple updates processed, as part of a single transaction operation
     * processing for resolving the indirect update references in a prepared transaction as part of
     * commit.
     */
    u_int multi_update_count;
#endif

    /* Scratch buffer for in-memory log records. */
    WT_ITEM *logrec;

    /* Requested notification when transactions are resolved. */
    WT_TXN_NOTIFY *notify;

    /* Checkpoint status. */
    WT_LSN ckpt_lsn;
    uint32_t ckpt_nsnapshot;
    WT_ITEM *ckpt_snapshot;
    bool full_ckpt;

    /* Timeout */
    uint64_t operation_timeout_us;

    const char *rollback_reason; /* If rollback, the reason */

/* AUTOMATIC FLAG VALUE GENERATION START */
#define WT_TXN_AUTOCOMMIT 0x00001u
#define WT_TXN_ERROR 0x00002u
#define WT_TXN_HAS_ID 0x00004u
#define WT_TXN_HAS_SNAPSHOT 0x00008u
#define WT_TXN_HAS_TS_COMMIT 0x00010u
#define WT_TXN_HAS_TS_READ 0x00020u
#define WT_TXN_IGNORE_PREPARE 0x00040u
#define WT_TXN_NAMED_SNAPSHOT 0x00080u
#define WT_TXN_PREPARE 0x00100u
#define WT_TXN_PUBLIC_TS_COMMIT 0x00200u
#define WT_TXN_PUBLIC_TS_READ 0x00400u
#define WT_TXN_READONLY 0x00800u
#define WT_TXN_RUNNING 0x01000u
#define WT_TXN_SYNC_SET 0x02000u
#define WT_TXN_TS_COMMIT_ALWAYS 0x04000u
#define WT_TXN_TS_COMMIT_KEYS 0x08000u
#define WT_TXN_TS_COMMIT_NEVER 0x10000u
#define WT_TXN_UPDATE 0x20000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP */
    uint32_t flags;
};
