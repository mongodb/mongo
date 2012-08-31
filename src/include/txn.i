/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_txn_getid --
 *	Get a transaction ID for a non-transactional operation.
 */
static inline void
__wt_txn_getid(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_txnid_t id;

	txn = &session->txn;

	if (F_ISSET(txn, TXN_RUNNING))
		return;

	/* If we already have the latest ID, keep using it. */
	id = txn->id;
	txn_global = &S2C(session)->txn_global;
	if (id + 1 == txn_global->current &&
	    id != WT_TXN_NONE && id != WT_TXN_ABORTED)
		return;

	do {
		id = txn_global->current;
	} while (!WT_ATOMIC_CAS(txn_global->current, id, id + 1) ||
	    id == WT_TXN_NONE || id == WT_TXN_ABORTED);

	txn->id = id;
}

/*
 * __wt_txn_modify --
 *	Mark a WT_UPDATE object modified by the current transaction.
 */
static inline int
__wt_txn_modify(WT_SESSION_IMPL *session, wt_txnid_t *id)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_RUNNING)) {
		if (txn->mod_count * sizeof(wt_txnid_t *) == txn->mod_alloc)
			WT_RET(__wt_realloc(session, &txn->mod_alloc,
			    WT_MAX(10, 2 * txn->mod_count) *
			    sizeof(wt_txnid_t *), &txn->mod));
		txn->mod[txn->mod_count++] = id;
	} else
		__wt_txn_getid(session);

	*id = txn->id;
	return (0);
}

/*
 * __wt_txn_modify_ref --
 *	Mark a WT_REF object modified by the current transaction.
 */
static inline int
__wt_txn_modify_ref(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_RUNNING)) {
		if (txn->modref_count *
		    sizeof(WT_REF *) == txn->modref_alloc)
			WT_RET(__wt_realloc(session, &txn->modref_alloc,
			    WT_MAX(10, 2 * txn->modref_count) *
			    sizeof(WT_REF *), &txn->modref));
		txn->modref[txn->modref_count++] = ref;
	} else
		__wt_txn_getid(session);

	ref->txnid = txn->id;
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

	/* Changes with no associated transaction are always visible. */
	if (id == WT_TXN_NONE)
		return (1);

	/* Transactions see their own changes. */
	txn = &session->txn;
	if (id == txn->id)
		return (1);

	/* Read-uncommitted transactions see all other changes. */
	if (txn->isolation == TXN_ISO_READ_UNCOMMITTED)
		return (1);

	/*
	 * TXN_ISO_SNAPSHOT, TXN_ISO_READ_COMMITTED: the ID is visible if it is
	 * not the result of a concurrent transaction, that is, if was
	 * committed before the snapshot was taken.
	 *
	 * The order here is important: anything newer than the maximum ID we
	 * saw when taking the snapshot should be invisible, even if the
	 * snapshot is empty.
	 */
	if (TXNID_LT(txn->snap_max, id))
		return (0);
	if (txn->snapshot_count == 0 || TXNID_LT(id, txn->snap_min))
		return (1);

	return (bsearch(&id, txn->snapshot, txn->snapshot_count,
	    sizeof(wt_txnid_t), __wt_txnid_cmp) == NULL);
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

	if (id != WT_TXN_NONE && TXNID_LT(id, current - TXN_WINDOW)) {
		WT_CSTAT_INCR(session, txn_ancient);
		return (1);
	}
	return (0);
}

/*
 * __wt_txn_read_first --
 *	Called for the first page read for a session.
 */
static inline void
__wt_txn_read_first(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/*
	 * If there is no transaction running, put an ID in the global table so
	 * the oldest reader in the system can be tracked.  This prevents any
	 * update the we are reading from being trimmed to save memory.
	 */
	if (!F_ISSET(txn, TXN_RUNNING)) {
		WT_ASSERT(session, txn_state->id == WT_TXN_NONE &&
		    !F_ISSET(txn_state, TXN_STATE_RUNNING));
		txn_state->id = txn_global->current;
	}

	if (txn->isolation == TXN_ISO_READ_COMMITTED ||
	    (!F_ISSET(txn, TXN_RUNNING) &&
	    txn->isolation == TXN_ISO_SNAPSHOT))
		__wt_txn_get_snapshot(session, WT_TXN_NONE);
}

/*
 * __wt_txn_read_last --
 *	Called when the last page for a session is released.
 */
static inline void
__wt_txn_read_last(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_state = &S2C(session)->txn_global.states[session->id];

	/*
	 * If there is no transaction running, release the ID we put in the
	 * global table.
	 */
	if (!F_ISSET(txn, TXN_RUNNING)) {
		WT_ASSERT(session, txn_state->id != WT_TXN_NONE &&
		    !F_ISSET(txn_state, TXN_STATE_RUNNING));
		txn_state->id = WT_TXN_NONE;
	}

	if (txn->isolation == TXN_ISO_READ_COMMITTED ||
	    (!F_ISSET(txn, TXN_RUNNING) &&
	    txn->isolation == TXN_ISO_SNAPSHOT))
		__wt_txn_release_snapshot(session);
}
