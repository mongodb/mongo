/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_TXN_NONE	0		/* No txn running in a session. */
#define	WT_TXN_ABORTED	UINT64_MAX	/* Update rolled back, ignore. */

/*
 * Transaction ID comparison dealing with edge cases.
 *
 * WT_TXN_ABORTED is the largest possible ID (never visible to a running
 * transaction), WT_TXN_NONE is smaller than any possible ID (visible to all
 * running transactions).
 */
#define	TXNID_LE(t1, t2)						\
	((t1) <= (t2))

#define	TXNID_LT(t1, t2)						\
	((t1) != (t2) && TXNID_LE(t1, t2))

struct __wt_txn_state {
	volatile uint64_t id;
	volatile uint64_t snap_min;
} WT_GCC_ATTRIBUTE((aligned(WT_CACHE_LINE_ALIGNMENT)));

struct __wt_txn_global {
	volatile uint64_t current;	/* Current transaction ID. */

	/* The oldest running transaction ID (may race). */
	uint64_t last_running;

	/*
	 * The oldest transaction ID that is not yet visible to some
	 * transaction in the system.
	 */
	volatile uint64_t oldest_id;

	/* Count of scanning threads, or -1 for exclusive access. */
	volatile int32_t scan_count;

	WT_TXN_STATE *states;		/* Per-session transaction states */
};

enum __wt_txn_isolation {
	TXN_ISO_EVICTION,		/* Internal: eviction context */
	TXN_ISO_READ_UNCOMMITTED,
	TXN_ISO_READ_COMMITTED,
	TXN_ISO_SNAPSHOT
};

/*
 * WT_TXN_OP --
 *	A transactional operation.  Each transaction builds an in-memory array
 *	of these operations as it runs, then uses the array to either write log
 *	records during commit or undo the operations during rollback.
 */
struct __wt_txn_op {
	uint32_t fileid;
	enum {
		TXN_OP_BASIC,
		TXN_OP_INMEM,
		TXN_OP_REF,
		TXN_OP_TRUNCATE_COL,
		TXN_OP_TRUNCATE_ROW
	} type;
	union {
		/* TXN_OP_BASIC, TXN_OP_INMEM */
		struct {
			WT_INSERT *ins;
			WT_UPDATE *upd;
			WT_ITEM key;
		} op;
		/* TXN_OP_REF */
		WT_REF *ref;
		/* TXN_OP_TRUNCATE_COL */
		struct {
			uint64_t start, stop;
		} truncate_col;
		/* TXN_OP_TRUNCATE_ROW */
		struct {
			WT_ITEM start, stop;
			enum {
				TXN_TRUNC_ALL,
				TXN_TRUNC_BOTH,
				TXN_TRUNC_START,
				TXN_TRUNC_STOP
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

	/*
	 * Snapshot data:
	 *	ids < snap_min are visible,
	 *	ids > snap_max are invisible,
	 *	everything else is visible unless it is in the snapshot.
	 */
	uint64_t snap_min, snap_max;
	uint64_t *snapshot;
	uint32_t snapshot_count;

	/* Array of modifications by this transaction. */
	WT_TXN_OP      *mod;
	size_t		mod_alloc;
	u_int		mod_count;

	/* Requested notification when transactions are resolved. */
	WT_TXN_NOTIFY *notify;

	/* Checkpoint status. */
	WT_LSN		ckpt_lsn;
	int		full_ckpt;
	uint32_t	ckpt_nsnapshot;
	WT_ITEM		*ckpt_snapshot;

#define	TXN_AUTOCOMMIT	0x01
#define	TXN_ERROR	0x02
#define	TXN_OLDEST	0x04
#define	TXN_RUNNING	0x08
	uint32_t flags;
};
