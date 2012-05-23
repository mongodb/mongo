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
	if (id == WT_TXN_NONE || txn->isolation != TXN_ISO_SNAPSHOT)
		return (1);

	/*
	 * The snapshot test.
	 */
	if (TXNID_LT(id, txn->snap_min))
		return (1);
	if (TXNID_LT(txn->id, id))
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
			if (upd->txnid != WT_TXN_ABORTED)
				return (WT_DEADLOCK);
			upd = upd->next;
		}

	return (0);
}
