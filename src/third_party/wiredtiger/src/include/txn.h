/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

#define WT_TXN_NONE 0                /* Beginning of time */
#define WT_TXN_FIRST 1               /* First transaction to run */
#define WT_TXN_MAX (UINT64_MAX - 10) /* End of time */
#define WT_TXN_ABORTED UINT64_MAX    /* Update rolled back */
#define WT_PREPARED_ID_NONE 0        /* Empty prepared id */

#define WT_TS_NONE 0         /* Beginning of time */
#define WT_TS_MAX UINT64_MAX /* End of time */

/*
 * A list of reasons for returning a rollback error from the API. These reasons can be queried via
 * the session get rollback reason API call. Users of the API could have a dependency on the format
 * of these messages so changing them must be done with care.
 */
#define WT_TXN_ROLLBACK_REASON_CACHE_OVERFLOW "Cache capacity has overflown"
#define WT_TXN_ROLLBACK_REASON_CONFLICT "Write conflict between concurrent operations"
#define WT_TXN_ROLLBACK_REASON_OLDEST_FOR_EVICTION \
    "Transaction has the oldest pinned transaction ID"

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_LOG_CKPT_CLEANUP 0x01u
#define WT_TXN_LOG_CKPT_PREPARE 0x02u
#define WT_TXN_LOG_CKPT_START 0x04u
#define WT_TXN_LOG_CKPT_STOP 0x08u
#define WT_TXN_LOG_CKPT_SYNC 0x10u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_OLDEST_STRICT 0x1u
#define WT_TXN_OLDEST_WAIT 0x2u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_TS_ALREADY_LOCKED 0x1u
#define WT_TXN_TS_INCLUDE_CKPT 0x2u
#define WT_TXN_TS_INCLUDE_OLDEST 0x4u
/* AUTOMATIC FLAG VALUE GENERATION STOP 32 */

typedef enum {
    WT_VISIBLE_FALSE = 0,   /* Not a visible update */
    WT_VISIBLE_PREPARE = 1, /* Prepared update */
    WT_VISIBLE_TRUE = 2     /* A visible update */
} WT_VISIBLE_TYPE;

/*
 * Enumeration used to track the context of reconstructing modifies within a update list.
 */
typedef enum { WT_OPCTX_TRANSACTION, WT_OPCTX_RECONCILATION } WT_OP_CONTEXT;

/*
 * Transaction ID comparison dealing with edge cases.
 *
 * WT_TXN_ABORTED is the largest possible ID (never visible to a running transaction), WT_TXN_NONE
 * is smaller than any possible ID (visible to all running transactions).
 */

#define WT_TXNID_LT(t1, t2) ((t1) < (t2))

#define WT_SESSION_TXN_SHARED(s)                         \
    (S2C(s)->txn_global.txn_shared_list == NULL ? NULL : \
                                                  &S2C(s)->txn_global.txn_shared_list[(s)->id])

#define WT_SESSION_IS_CHECKPOINT(s) \
    (!WT_SESSION_IS_DEFAULT(s) && (s)->id == __wt_atomic_loadv32(&S2C(s)->txn_global.checkpoint_id))

/*
 * Perform an operation at the specified isolation level.
 *
 * This is fiddly: we can't cope with operations that begin transactions (leaving an ID allocated),
 * and operations must not move our published snap_min forwards (or updates we need could be freed
 * while this operation is in progress). Check for those cases: the bugs they cause are hard to
 * debug.
 */
#define WT_WITH_TXN_ISOLATION(s, iso, op)                                                        \
    do {                                                                                         \
        WT_TXN_ISOLATION saved_iso = (s)->isolation;                                             \
        WT_TXN_ISOLATION saved_txn_iso = (s)->txn->isolation;                                    \
        WT_TXN_SHARED *txn_shared = WT_SESSION_TXN_SHARED(s);                                    \
        WT_TXN_SHARED saved_txn_shared = *txn_shared;                                            \
        uint64_t txn_shared_id = __wt_atomic_loadv64(&txn_shared->id);                           \
        uint64_t txn_shared_metadata_pinned = __wt_atomic_loadv64(&txn_shared->metadata_pinned); \
        uint64_t txn_shared_pinned_id = __wt_atomic_loadv64(&txn_shared->pinned_id);             \
        uint64_t saved_txn_shared_id = __wt_atomic_loadv64(&saved_txn_shared.id);                \
        uint64_t saved_txn_shared_metadata_pinned =                                              \
          __wt_atomic_loadv64(&saved_txn_shared.metadata_pinned);                                \
        uint64_t saved_txn_shared_pinned_id = __wt_atomic_loadv64(&saved_txn_shared.pinned_id);  \
                                                                                                 \
        /* The following variables are only used inside an assert. */                            \
        WT_UNUSED(txn_shared_id);                                                                \
        WT_UNUSED(txn_shared_metadata_pinned);                                                   \
        WT_UNUSED(txn_shared_pinned_id);                                                         \
        WT_UNUSED(saved_txn_shared_id);                                                          \
                                                                                                 \
        (s)->txn->forced_iso++;                                                                  \
        (s)->isolation = (s)->txn->isolation = (iso);                                            \
        op;                                                                                      \
        (s)->isolation = saved_iso;                                                              \
        (s)->txn->isolation = saved_txn_iso;                                                     \
        WT_ASSERT((s), (s)->txn->forced_iso > 0);                                                \
        (s)->txn->forced_iso--;                                                                  \
        WT_ASSERT((s),                                                                           \
          txn_shared_id == saved_txn_shared_id &&                                                \
            (txn_shared_metadata_pinned == saved_txn_shared_metadata_pinned ||                   \
              saved_txn_shared_metadata_pinned == WT_TXN_NONE) &&                                \
            (txn_shared_pinned_id == saved_txn_shared_pinned_id ||                               \
              saved_txn_shared_pinned_id == WT_TXN_NONE));                                       \
        __wt_atomic_storev64(&txn_shared->metadata_pinned, saved_txn_shared_metadata_pinned);    \
        __wt_atomic_storev64(&txn_shared->pinned_id, saved_txn_shared_pinned_id);                \
    } while (0)

struct __wt_txn_shared {
    WT_CACHE_LINE_PAD_BEGIN
    wt_shared volatile uint64_t id;
    wt_shared volatile uint64_t pinned_id;
    wt_shared volatile uint64_t metadata_pinned;

    /*
     * The first commit or durable timestamp used for this transaction. Determines its position in
     * the durable queue and prevents the all_durable timestamp moving past this point.
     */
    wt_shared wt_timestamp_t pinned_durable_timestamp;

    /*
     * The read timestamp used for this transaction. Determines what updates can be read and
     * prevents the oldest timestamp moving past this point.
     */
    wt_shared wt_timestamp_t read_timestamp;

    wt_shared volatile uint8_t is_allocating;
    WT_CACHE_LINE_PAD_END
};

/*
 * WT_PENDING_PREPARED_ITEM --
 *	A structure to store the transactions prepared operations.
 */
struct __wt_pending_prepared_item {
    TAILQ_ENTRY(__wt_pending_prepared_item) hashq;
    uint64_t prepared_id;
    wt_timestamp_t prepare_timestamp;
    WT_TXN_OP *mod;
    size_t mod_alloc;
    uint32_t mod_count;
#ifdef HAVE_DIAGNOSTIC
    uint32_t prepare_count;
#endif
};

/*
 * WT_PENDING_PREPARED_MAP -- Hash map for pending prepared transactions that are available to be
 * claimed. Populated by a prepared transactions cursor, and cleaned up when the cursor is closed.
 * No need for concurrency control on making changes to the list.
 */
struct __wt_pending_prepared_map {
    TAILQ_HEAD(__wt_pending_prepared_hash, __wt_pending_prepared_item) * hash;
    uint64_t hash_size; /* Number of hash buckets */
};

struct __wt_txn_global {
    wt_shared volatile uint64_t current; /* Current transaction ID. */

    /* The oldest running transaction ID (may race). */
    wt_shared volatile uint64_t last_running;

    /*
     * The oldest transaction ID that is not yet visible to some transaction in the system.
     */
    wt_shared volatile uint64_t oldest_id;

    wt_shared wt_timestamp_t durable_timestamp;
    wt_shared wt_timestamp_t last_ckpt_timestamp;
    wt_timestamp_t meta_ckpt_timestamp;
    wt_shared wt_timestamp_t oldest_timestamp;
    wt_shared wt_timestamp_t pinned_timestamp;
    wt_timestamp_t recovery_timestamp;
    wt_shared wt_timestamp_t stable_timestamp;
    wt_shared wt_timestamp_t newest_seen_timestamp; /* Used by eviction to make guesses */
    wt_timestamp_t version_cursor_pinned_timestamp;
    bool has_durable_timestamp;
    wt_shared bool has_oldest_timestamp;
    bool has_pinned_timestamp;
    bool has_stable_timestamp;
    bool oldest_is_pinned;
    bool stable_is_pinned;

    /* Protects the active transaction states. */
    WT_RWLOCK rwlock;

    /* Protects logging, checkpoints and transaction visibility. */
    WT_RWLOCK visibility_rwlock;

    /*
     * Track information about the running checkpoint. The transaction snapshot used when
     * checkpointing are special. Checkpoints can run for a long time so we keep them out of regular
     * visibility checks. Eviction and checkpoint operations know when they need to be aware of
     * checkpoint transactions.
     *
     * We rely on the fact that (a) the only table a checkpoint updates is the metadata; and (b)
     * once checkpoint has finished reading a table, it won't revisit it.
     */
    wt_shared volatile bool checkpoint_running; /* Checkpoint running */
    wt_shared volatile bool
      checkpoint_running_hs;             /* Checkpoint running and processing history store file */
    volatile uint32_t checkpoint_id;     /* Checkpoint's session ID */
    WT_TXN_SHARED checkpoint_txn_shared; /* Checkpoint's txn shared state */
    wt_shared wt_timestamp_t checkpoint_timestamp; /* Checkpoint's timestamp */

    wt_shared volatile uint64_t debug_ops;       /* Debug mode op counter */
    uint64_t debug_rollback;                     /* Debug mode rollback */
    wt_shared volatile uint64_t metadata_pinned; /* Oldest ID for metadata */

    WT_TXN_SHARED *txn_shared_list; /* Per-session shared transaction states */

    WT_PENDING_PREPARED_MAP pending_prepare_items;
};

typedef enum __wt_txn_isolation {
    WT_ISO_READ_COMMITTED,
    WT_ISO_READ_UNCOMMITTED,
    WT_ISO_SNAPSHOT
} WT_TXN_ISOLATION;

typedef enum __wt_txn_type {
    WT_TXN_OP_NONE = 0,
    WT_TXN_OP_BASIC_COL,
    WT_TXN_OP_BASIC_ROW,
    WT_TXN_OP_INMEM_COL,
    WT_TXN_OP_INMEM_ROW,
    WT_TXN_OP_REF_DELETE,
    WT_TXN_OP_TRUNCATE_COL,
    WT_TXN_OP_TRUNCATE_ROW
} WT_TXN_TYPE;

typedef enum {
    WT_TXN_TRUNC_ALL,
    WT_TXN_TRUNC_BOTH,
    WT_TXN_TRUNC_START,
    WT_TXN_TRUNC_STOP
} WT_TXN_TRUNC_MODE;

/*
 * WT_TXN_OP --
 *	A transactional operation.  Each transaction builds an in-memory array
 *	of these operations as it runs, then uses the array to either write log
 *	records during commit or undo the operations during rollback.
 */
struct __wt_txn_op {
    WT_BTREE *btree;
    WT_TXN_TYPE type;
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
            WT_TXN_TRUNC_MODE mode;
        } truncate_row;
    } u;

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_OP_KEY_REPEATED 0x1u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    uint32_t flags;
};

/*
 * WT_TXN_SNAPSHOT --
 *	A structure to store the transactions snapshot details.
 */
struct __wt_txn_snapshot {
    /*
     * Snapshot data:
     *	txn_ids >= snap_max are invisible,
     *	txn_ids < snap_min are visible,
     *	everything else is visible unless it is in the snapshot.
     */
    uint64_t snap_max, snap_min;
    uint64_t *snapshot;
    uint32_t snapshot_count;
};

#define WT_TS_VERBOSE_PREFIX "unexpected timestamp usage: "

struct __wt_txn_log {
    uint32_t txn_logsync; /* Log sync configuration */

    /* Scratch buffer for in-memory log records. */
    WT_ITEM *logrec;
};

/*
 * WT_TXN --
 *	Per-session transaction context.
 */
struct __wt_txn {
    uint64_t id;

    uint64_t prepared_id;

    WT_TXN_ISOLATION isolation;

    uint32_t forced_iso; /* Isolation is currently forced. */

    WT_TXN_LOG txn_log;

    /* Snapshot data. */
    WT_TXN_SNAPSHOT snapshot_data;

    /* Backup snapshot data. */
    WT_TXN_SNAPSHOT *backup_snapshot_data;

    /*
     * Timestamp copied into updates created by this transaction.
     *
     * In some use cases, this can be updated while the transaction is running.
     */
    wt_timestamp_t commit_timestamp;

    /*
     * Durable timestamp copied into updates created by this transaction. It is used to decide
     * whether to consider this update to be persisted or not by stable checkpoint.
     */
    wt_timestamp_t durable_timestamp;

    /*
     * Set to the first commit timestamp used in the transaction and fixed while the transaction is
     * on the public list of committed timestamps.
     */
    wt_timestamp_t first_commit_timestamp;

    /*
     * Timestamp copied into updates created by this transaction, when this transaction is prepared.
     */
    wt_timestamp_t prepare_timestamp;

    /*
     * Timestamp copied into updates created by this transaction, when this transaction is rolled
     * back. Only valid for prepared transactions under the preserve_prepared config.
     */
    wt_timestamp_t rollback_timestamp;

    /*
     * Timestamps used for reading via a checkpoint cursor instead of txn_shared->read_timestamp and
     * the current oldest/pinned timestamp, respectively.
     */
    wt_timestamp_t checkpoint_read_timestamp;
    wt_timestamp_t checkpoint_stable_timestamp;
    wt_timestamp_t checkpoint_oldest_timestamp;

    /* Array of modifications by this transaction. */
    WT_TXN_OP *mod;
    size_t mod_alloc;
    uint32_t mod_count;
#ifdef HAVE_DIAGNOSTIC
    u_int prepare_count;
#endif

    /* Checkpoint status. */
    WT_LSN ckpt_lsn;
    uint32_t ckpt_nsnapshot;
    WT_ITEM *ckpt_snapshot;
    bool full_ckpt;

    /* Timeout */
    uint64_t operation_timeout_us;

/*
 * WT_TXN_HAS_TS_COMMIT --
 *	The transaction has a set commit timestamp.
 * WT_TXN_HAS_TS_DURABLE --
 *	The transaction has an explicitly set durable timestamp (that is, it
 *	hasn't been mirrored from its commit timestamp value).
 * WT_TXN_SHARED_TS_DURABLE --
 *	The transaction has been published to the durable queue. Setting this
 *	flag lets us know that, on release, we need to mark the transaction for
 *	clearing.
 */

/* AUTOMATIC FLAG VALUE GENERATION START 0 */
#define WT_TXN_AUTOCOMMIT 0x000001u
#define WT_TXN_ERROR 0x000002u
#define WT_TXN_HAS_ID 0x000004u
#define WT_TXN_HAS_PREPARED_ID 0x000008u
#define WT_TXN_HAS_SNAPSHOT 0x000010u
#define WT_TXN_HAS_TS_COMMIT 0x000020u
#define WT_TXN_HAS_TS_DURABLE 0x000040u
#define WT_TXN_HAS_TS_PREPARE 0x000080u
#define WT_TXN_HAS_TS_ROLLBACK 0x000100u
#define WT_TXN_IGNORE_PREPARE 0x000200u
#define WT_TXN_IS_CHECKPOINT 0x000400u
#define WT_TXN_PREPARE 0x000800u
#define WT_TXN_PREPARE_IGNORE_API_CHECK 0x001000u
#define WT_TXN_READONLY 0x002000u
#define WT_TXN_REFRESH_SNAPSHOT 0x004000u
#define WT_TXN_RUNNING 0x008000u
#define WT_TXN_SHARED_TS_DURABLE 0x010000u
#define WT_TXN_SHARED_TS_READ 0x020000u
#define WT_TXN_SYNC_SET 0x040000u
#define WT_TXN_TS_NOT_SET 0x080000u
#define WT_TXN_TS_ROUND_PREPARED 0x100000u
#define WT_TXN_TS_ROUND_READ 0x200000u
#define WT_TXN_UPDATE 0x400000u
    /* AUTOMATIC FLAG VALUE GENERATION STOP 32 */
    wt_shared uint32_t flags;

    uint16_t modify_block_count;

    /*
     * Zero or more bytes of value (the payload) immediately follows the WT_TXN structure. We use a
     * C99 flexible array member which has the semantics we want.
     */
    uint64_t __snapshot[];
};
