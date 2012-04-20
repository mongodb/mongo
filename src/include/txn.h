/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#define	WT_TXN_INVALID	0
#define	WT_TXN_NONE	1
typedef uint32_t wt_txnid_t;

/* Transaction ID comparison dealing with edge cases and wrapping. */
#define	TXNID_LT(t1, t2)						\
	(((t1) == (t2)) ? 0 :						\
	 ((t2) == WT_TXN_INVALID || (t1) == WT_TXN_NONE) ? 1 :		\
	 (t2) - (t1) < (UINT32_MAX / 2))

struct __wt_txn_global {
	wt_txnid_t current;

	wt_txnid_t *ids;
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
	size_t snapshot_alloc;
	u_int snapshot_count;

	/* Array of txn IDs in items created or modified by this txn. */
	wt_txnid_t **mod;
	size_t mod_alloc;
	u_int mod_count;

#define	TXN_RUNNING	0x01
#define	TXN_SNAPSHOT	0x02
	uint32_t flags;
};
