/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_txn_modify --
 *      Mark an object modified by the current transaction.
 */
static inline int
__wt_txn_modify(WT_SESSION_IMPL *session, wt_txnid_t *id)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_RUNNING)) {
		*id = txn->id;
		if (txn->mod_count * sizeof(wt_txnid_t *) == txn->mod_alloc)
			WT_RET(__wt_realloc(session, &txn->mod_alloc,
			    WT_MAX(10, 2 * txn->mod_count) *
			    sizeof(wt_txnid_t *), &txn->mod));
		txn->mod[txn->mod_count++] = id;
	} else
		*id = WT_TXN_NONE;

	return (0);
}

/*
 * __wt_txn_unmodify --
 *	If threads race making updates, they may discard the last referenced
 *	WT_UPDATE item while the transaction is still active.  This function
 *	removes the last update item from the "log".
 */
static inline void
__wt_txn_unmodify(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_RUNNING)) {
		WT_ASSERT(session, txn->mod_count > 0);
		txn->mod_count--;
	}
}

/*
 * __wt_txn_visible --
 *	Can the current transaction see the given ID?
 */
static inline int
__wt_txn_visible(WT_SESSION_IMPL *session, wt_txnid_t id)
{
	WT_TXN *txn;

	/* Nobody sees the results of aborted transactions. */
	if (id == WT_TXN_ABORTED)
		return (0);

	/*
	 * Changes with no associated transaction are always visible, and
	 * non-snapshot transactions see all other changes.
	 */
	txn = &session->txn;
	if (id == WT_TXN_NONE || id == txn->id ||
	    txn->isolation != TXN_ISO_SNAPSHOT)
		return (1);

	/*
	 * The snapshot test.
	 */
	if (TXNID_LT(id, txn->snap_min))
		return (1);
	if (TXNID_LT(txn->snap_max, id))
		return (0);

	/*
	 * Otherwise, the ID is visible if it is not the result of a concurrent
	 * transaction.  That is, if it is not in the snapshot list.  Fast path
	 * the single-threaded case where there are no concurrent transactions.
	 */
	return (txn->snapshot_count == 0 || bsearch(&id, txn->snapshot,
	    txn->snapshot_count, sizeof(wt_txnid_t), __wt_txnid_cmp) == NULL);
}

/*
 * __wt_txn_read_skip --
 *	Get the first visible update in a list (or NULL if none are visible),
 *	and report whether uncommitted changes were skipped.
 */
static inline WT_UPDATE *
__wt_txn_read_skip(WT_SESSION_IMPL *session, WT_UPDATE *upd, int *skipp)
{
	while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
		if (upd->txnid != WT_TXN_ABORTED)
			*skipp = 1;
		upd = upd->next;
	}

	return (upd);
}

/*
 * __wt_txn_read --
 *	Get the first visible update in a list (or NULL if none are visible).
 */
static inline WT_UPDATE *
__wt_txn_read(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	while (upd != NULL && !__wt_txn_visible(session, upd->txnid))
		upd = upd->next;

	return (upd);
}

/*
 * __wt_txn_update_check --
 *	Check if the current transaction can update an item.
 */
static inline int
__wt_txn_update_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (txn->isolation == TXN_ISO_SNAPSHOT)
		while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
			if (upd->txnid != WT_TXN_ABORTED) {
				WT_BSTAT_INCR(session, update_conflict);
				return (WT_DEADLOCK);
			}
			upd = upd->next;
		}

	return (0);
}

/*
 * __wt_txn_ancient --
 *	Check if a given transaction ID is "ancient".
 *
 *	Transaction IDs are 32 bit integers, and we use a 31 bit window when
 *	comparing them (in TXNID_LT).  As a result, updates from a transaction
 *	more than 2 billion transactions older than the current ID appear to be
 *	in the future and are no longer be visible to running transactions.
 *
 *	Call an update "ancient" if it will become invisible in under a million
 *	transactions, to give eviction time to write it.  Eviction is forced on
 *	pages with ancient transactions before they can be read.
 */
static inline int
__wt_txn_ancient(WT_SESSION_IMPL *session, wt_txnid_t id)
{
	WT_TXN_GLOBAL *txn_global;
	wt_txnid_t current;

	txn_global = &S2C(session)->txn_global;
	current = txn_global->current;

#define	TXN_WRAP_BUFFER	1000000
#define	TXN_WINDOW	((UINT32_MAX / 2) - TXN_WRAP_BUFFER)

	return (id != WT_TXN_NONE && TXNID_LT(id, current - TXN_WINDOW));
}
