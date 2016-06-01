/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_TXN_NONE	0		/* No txn running in a session. */
#define	WT_TXN_FIRST	1		/* First transaction to run. */
#define	WT_TXN_ABORTED	UINT64_MAX	/* Update rolled back, ignore. */

/*
 * Transaction ID comparison dealing with edge cases.
 *
 * WT_TXN_ABORTED is the largest possible ID (never visible to a running
 * transaction), WT_TXN_NONE is smaller than any possible ID (visible to all
 * running transactions).
 */
#define	WT_TXNID_LE(t1, t2)						\
	((t1) <= (t2))

#define	WT_TXNID_LT(t1, t2)						\
	((t1) < (t2))

#define	WT_SESSION_TXN_STATE(s) (&S2C(s)->txn_global.states[(s)->id])

#define	WT_SESSION_IS_CHECKPOINT(s)					\
	((s)->id != 0 && (s)->id == S2C(s)->txn_global.checkpoint_id)

/*
 * Perform an operation at the specified isolation level.
 *
 * This is fiddly: we can't cope with operations that begin transactions
 * (leaving an ID allocated), and operations must not move our published
 * snap_min forwards (or updates we need could be freed while this operation is
 * in progress).  Check for those cases: the bugs they cause are hard to debug.
 */
#define	WT_WITH_TXN_ISOLATION(s, iso, op) do {				\
	WT_TXN_ISOLATION saved_iso = (s)->isolation;		        \
	WT_TXN_ISOLATION saved_txn_iso = (s)->txn.isolation;		\
	WT_TXN_STATE *txn_state = WT_SESSION_TXN_STATE(s);		\
	WT_TXN_STATE saved_state = *txn_state;				\
	(s)->txn.forced_iso++;						\
	(s)->isolation = (s)->txn.isolation = (iso);			\
	op;								\
	(s)->isolation = saved_iso;					\
	(s)->txn.isolation = saved_txn_iso;				\
	WT_ASSERT((s), (s)->txn.forced_iso > 0);                        \
	(s)->txn.forced_iso--;						\
	WT_ASSERT((s), txn_state->id == saved_state.id &&		\
	    (txn_state->snap_min == saved_state.snap_min ||		\
	    saved_state.snap_min == WT_TXN_NONE));			\
	txn_state->snap_min = saved_state.snap_min;			\
} while (0)

struct __wt_named_snapshot {
	const char *name;

	TAILQ_ENTRY(__wt_named_snapshot) q;

	uint64_t snap_min, snap_max;
	uint64_t *snapshot;
	uint32_t snapshot_count;
};

struct WT_COMPILER_TYPE_ALIGN(WT_CACHE_LINE_ALIGNMENT) __wt_txn_state {
	volatile uint64_t id;
	volatile uint64_t snap_min;
};

struct __wt_txn_global {
	WT_SPINLOCK id_lock;
	volatile uint64_t current;	/* Current transaction ID. */

	/* The oldest running transaction ID (may race). */
	volatile uint64_t last_running;

	/*
	 * The oldest transaction ID that is not yet visible to some
	 * transaction in the system.
	 */
	volatile uint64_t oldest_id;

	/*
	 * Prevents the oldest ID moving forwards while threads are scanning
	 * the global transaction state.
	 */
	WT_RWLOCK *scan_rwlock;

	/*
	 * Track information about the running checkpoint. The transaction
	 * snapshot used when checkpointing are special. Checkpoints can run
	 * for a long time so we keep them out of regular visibility checks.
	 * Eviction and checkpoint operations know when they need to be aware
	 * of checkpoint transactions.
	 */
	volatile uint32_t checkpoint_id;	/* Checkpoint's session ID */
	volatile uint64_t checkpoint_gen;
	volatile uint64_t checkpoint_pinned;

	/* Named snapshot state. */
	WT_RWLOCK *nsnap_rwlock;
	volatile uint64_t nsnap_oldest_id;
	TAILQ_HEAD(__wt_nsnap_qh, __wt_named_snapshot) nsnaph;

	WT_TXN_STATE *states;		/* Per-session transaction states */
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
	uint32_t fileid;
	enum {
		WT_TXN_OP_BASIC,
		WT_TXN_OP_INMEM,
		WT_TXN_OP_REF,
		WT_TXN_OP_TRUNCATE_COL,
		WT_TXN_OP_TRUNCATE_ROW
	} type;
	union {
		/* WT_TXN_OP_BASIC, WT_TXN_OP_INMEM */
		WT_UPDATE *upd;
		/* WT_TXN_OP_REF */
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

	uint32_t forced_iso;	/* Isolation is currently forced. */

	/*
	 * Snapshot data:
	 *	ids < snap_min are visible,
	 *	ids > snap_max are invisible,
	 *	everything else is visible unless it is in the snapshot.
	 */
	uint64_t snap_min, snap_max;
	uint64_t *snapshot;
	uint32_t snapshot_count;
	uint32_t txn_logsync;	/* Log sync configuration */

	/* Array of modifications by this transaction. */
	WT_TXN_OP      *mod;
	size_t		mod_alloc;
	u_int		mod_count;

	/* Scratch buffer for in-memory log records. */
	WT_ITEM	       *logrec;

	/* Requested notification when transactions are resolved. */
	WT_TXN_NOTIFY *notify;

	/* Checkpoint status. */
	WT_LSN		ckpt_lsn;
	uint32_t	ckpt_nsnapshot;
	WT_ITEM		*ckpt_snapshot;
	bool		full_ckpt;

#define	WT_TXN_AUTOCOMMIT	0x01
#define	WT_TXN_ERROR		0x02
#define	WT_TXN_HAS_ID		0x04
#define	WT_TXN_HAS_SNAPSHOT	0x08
#define	WT_TXN_NAMED_SNAPSHOT	0x10
#define	WT_TXN_READONLY		0x20
#define	WT_TXN_RUNNING		0x40
#define	WT_TXN_SYNC_SET		0x80
	uint32_t flags;
};
