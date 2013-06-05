/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Transaction ID type: transaction IDs are 32-bit integers that wrap after
 * 4 billion transactions are executed.
 */
typedef uint64_t wt_txnid_t;

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
	volatile wt_txnid_t id;
	volatile wt_txnid_t snap_min;
};

struct __wt_txn_global {
	volatile wt_txnid_t current;	/* Current transaction ID. */

	/*
	 * The oldest transaction ID that is not yet visible to some
	 * transaction in the system.
	 */
	volatile wt_txnid_t oldest_id;

	volatile uint32_t gen;		/* Completed transaction generation */
	volatile uint32_t scan_gen;	/* Snapshot scan generation */

	WT_TXN_STATE *states;		/* Per-session transaction states */
};

enum __wt_txn_isolation {
	TXN_ISO_READ_UNCOMMITTED,
	TXN_ISO_READ_COMMITTED,
	TXN_ISO_SNAPSHOT
};

struct __wt_txn {
	wt_txnid_t id;

	WT_TXN_ISOLATION isolation;

	/*
	 * Snapshot data:
	 *	ids < snap_min are visible,
	 *	ids > snap_max are invisible,
	 *	everything else is visible unless it is in the snapshot.
	 */
	wt_txnid_t snap_min, snap_max;
	wt_txnid_t *snapshot;
	uint32_t snapshot_count;

	/* Saved global state, to avoid repeating scans. */
	wt_txnid_t last_id;
	uint32_t last_gen;
	uint32_t last_scan_gen;

	/*
	 * Arrays of txn IDs in WT_UPDATE or WT_REF structures created or
	 * modified by this transaction.
	 */
	wt_txnid_t    **mod;
	size_t		mod_alloc;
	u_int		mod_count;

	WT_REF	      **modref;
	size_t		modref_alloc;
	u_int		modref_count;

#define	TXN_AUTOCOMMIT	0x01
#define	TXN_ERROR	0x02
#define	TXN_FORCE_EVICT	0x04
#define	TXN_OLDEST	0x08
#define	TXN_RUNNING	0x10
	uint32_t flags;
};
