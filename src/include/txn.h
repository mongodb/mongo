/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/* Transaction ID type. */
typedef uint32_t wt_txnid_t;

#define	WT_TXN_NONE	0		/* No txn running in a session. */
#define	WT_TXN_ABORTED	UINT32_MAX	/* Update rolled back, ignore. */

/*
 * Transaction ID comparison dealing with edge cases and wrapping.
 *
 * WT_TXN_ABORTED is the largest possible ID (never visible to a running
 * transaction), WT_TXN_NONE is smaller than any possible ID (visible to all
 * running transactions).
 */
#define	TXNID_LT(t1, t2)						\
	(((t1) == (t2) ||						\
	 (t1) == WT_TXN_NONE || (t2) == WT_TXN_ABORTED) ? 0 :	\
	 ((t2) == WT_TXN_NONE || (t1) == WT_TXN_ABORTED) ? 1 :	\
	 (t2) - (t1) < (UINT32_MAX / 2))

struct __wt_txn_global {
	volatile wt_txnid_t current;

	wt_txnid_t *ids;

	WT_TXN *checkpoint_txn;
};

struct __wt_txn {
	wt_txnid_t id;

	/*
	 * Snapshot data:
	 *     everything < snapshot[0] is visible,
	 *     everything > id is invisible
	 *     everything in between is visible unless it is in snap_overlap.
	 */
	wt_txnid_t snap_min;
	wt_txnid_t *snapshot;
	u_int snapshot_count;

	/* Array of txn IDs in items created or modified by this txn. */
	wt_txnid_t **mod;
	size_t mod_alloc;
	u_int mod_count;

	enum {
		TXN_ISO_READ_UNCOMMITTED,
		TXN_ISO_READ_COMMITTED,
		TXN_ISO_SNAPSHOT
	} isolation;

#define	TXN_RUNNING	0x01
	uint32_t flags;
};
