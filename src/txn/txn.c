/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_txnid_cmp --
 *	Compare transaction IDs for sorting / searching.
 */
int
__wt_txnid_cmp(const void *v1, const void *v2)
{
	uint64_t id1, id2;

	id1 = *(uint64_t *)v1;
	id2 = *(uint64_t *)v2;

	return ((id1 == id2) ? 0 : TXNID_LT(id1, id2) ? -1 : 1);
}

/*
 * __txn_sort_snapshot --
 *	Sort a snapshot for faster searching and set the min/max bounds.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session, uint32_t n, uint64_t id)
{
	WT_TXN *txn;

	txn = &session->txn;

	if (n > 1)
		qsort(txn->snapshot, n, sizeof(uint64_t), __wt_txnid_cmp);
	txn->snapshot_count = n;
	txn->snap_max = id;
	txn->snap_min = (n == 0 || TXNID_LT(id, txn->snapshot[0])) ?
	    id : txn->snapshot[0];
	WT_ASSERT(session, n == 0 || txn->snap_min != WT_TXN_NONE);
}

/*
 * __wt_txn_release_snapshot --
 *	Release the snapshot in the current transaction.
 */
void
__wt_txn_release_snapshot(WT_SESSION_IMPL *session)
{
	WT_TXN_STATE *txn_state;

	txn_state = &S2C(session)->txn_global.states[session->id];
	txn_state->snap_min = WT_TXN_NONE;
}

/*
 * __wt_txn_refresh --
 *	Allocate a transaction ID and/or a snapshot.
 */
void
__wt_txn_refresh(WT_SESSION_IMPL *session, uint64_t max_id, int get_snapshot)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s, *txn_state;
	uint64_t current_id, id, snap_min, oldest_id;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	if (get_snapshot &&
	    txn->id == max_id &&
	    txn->last_id == txn_global->current &&
	    txn->last_gen == txn_global->gen &&
	    TXNID_LE(txn_global->oldest_id, txn->snap_min)) {
		/* If nothing has changed since last time, we're done. */
		txn_state->snap_min = txn->snap_min;
		return;
	}

	do {
		/* Take a copy of the current generation numbers. */
		txn->last_scan_gen = txn_global->scan_gen;
		txn->last_gen = txn_global->gen;
		txn->last_id = current_id = txn_global->current;
		snap_min = current_id + 1;

		/*
		 * Constrain the oldest ID we calculate to be less than the
		 * specified value.
		 */
		oldest_id = (max_id != WT_TXN_NONE) ? max_id : snap_min;

		/* Copy the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = n = 0, s = txn_global->states;
		    i < session_cnt;
		    i++, s++) {
			/*
			 * Ignore the ID if we are committing (indicated by
			 * max_id being set): it is about to be released.
			 */
			if ((id = s->id) != WT_TXN_NONE && id + 1 != max_id) {
				txn->snapshot[n++] = id;
				if (TXNID_LT(id, snap_min))
					snap_min = id;
			}
			/*
			 * Ignore the session's own snap_min if we are in the
			 * process of updating it.
			 */
			if (get_snapshot && s == txn_state)
				continue;
			if ((id = s->snap_min) != WT_TXN_NONE &&
			    TXNID_LT(id, oldest_id))
				oldest_id = id;
		}

		if (TXNID_LT(snap_min, oldest_id))
			oldest_id = snap_min;

		/*
		 * Ensure the snapshot reads are scheduled before re-checking
		 * the global generation.
		 */
		WT_READ_BARRIER();

		/*
		 * When getting an ordinary snapshot, it is sufficient to
		 * unconditionally bump the scan generation.  Otherwise, we're
		 * trying to update the oldest ID, so require that the scan
		 * generation has not changed while we have been scanning.
		 */
		if (get_snapshot) {
			txn_state->snap_min = snap_min;
			WT_ATOMIC_ADD(txn_global->scan_gen, 1);
		}
	} while (TXNID_LT(snap_min, txn_global->oldest_id) ||
	    (!get_snapshot && !WT_ATOMIC_CAS(txn_global->scan_gen,
		txn->last_scan_gen, txn->last_scan_gen + 1)));

	++txn->last_scan_gen;

	/* Update the oldest ID if another thread hasn't beat us to it. */
	do {
		id = txn_global->oldest_id;
	} while ((!get_snapshot ||
	    txn->last_scan_gen == txn_global->scan_gen) &&
	    TXNID_LT(id, oldest_id) &&
	    !WT_ATOMIC_CAS(txn_global->oldest_id, id, oldest_id));

	if (get_snapshot)
		__txn_sort_snapshot(session, n, current_id + 1);
}

/*
 * __wt_txn_get_evict_snapshot --
 *	Set up a snapshot in the current transaction for eviction.
 *	Only changes that visible to all active transactions can be evicted.
 */
void
__wt_txn_get_evict_snapshot(WT_SESSION_IMPL *session)
{
	WT_TXN_GLOBAL *txn_global;
	uint64_t oldest_id;

	txn_global = &S2C(session)->txn_global;

	/*
	 * The oldest active snapshot ID in the system that should *not* be
	 * visible to eviction.  Create a snapshot containing that ID.
	 */
	__wt_txn_refresh(session, WT_TXN_NONE, 0);
	oldest_id = txn_global->oldest_id;
	__txn_sort_snapshot(session, 0, oldest_id);

	/*
	 * Note that we carefully don't update the global table with this
	 * snap_min value: there may already be a running transaction in this
	 * session with its own value in the global table.
	 */
}

/*
 * __wt_txn_begin --
 *	Begin a transaction.
 */
int
__wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	uint64_t current_id;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	WT_ASSERT(session, txn_state->id == WT_TXN_NONE);

	WT_RET(__wt_config_gets_def(session, cfg, "isolation", 0, &cval));
	if (cval.len == 0)
		txn->isolation = session->isolation;
	else
		txn->isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    TXN_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-committed", cval.str, cval.len) ?
		    TXN_ISO_READ_COMMITTED : TXN_ISO_READ_UNCOMMITTED;

	/*
	 * Allocate a transaction ID.
	 *
	 * We use an atomic compare and swap to ensure that we get a
	 * unique ID that is published before the global counter is
	 * updated.
	 *
	 * If two threads race to allocate an ID, only the latest ID
	 * will proceed.  The winning thread can be sure its snapshot
	 * contains all of the earlier active IDs.  Threads that race
	 * and get an earlier ID may not appear in the snapshot, but
	 * they will loop and allocate a new ID before proceeding to
	 * make any updates.
	 *
	 * This potentially wastes transaction IDs when threads race to
	 * begin transactions: that is the price we pay to keep this
	 * path latch free.
	 */
	do {
		current_id = txn_global->current;
		txn_state->id = txn->id = current_id + 1;
	} while (
	    !WT_ATOMIC_CAS(txn_global->current, current_id, txn->id) ||
	    txn->id == WT_TXN_NONE);

	/*
	 * If we have use 64-bits of transaction IDs, there is nothing
	 * more we can do.
	 */
	if (txn->id == WT_TXN_ABORTED)
		WT_RET_MSG(session, ENOMEM, "Out of transaction IDs");

	F_SET(txn, TXN_RUNNING);
	if (txn->isolation == TXN_ISO_SNAPSHOT)
		__wt_txn_refresh(session, WT_TXN_NONE, 1);
	return (0);
}

/*
 * __wt_txn_release --
 *	Release the resources associated with the current transaction.
 */
void
__wt_txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn->mod_count = txn->modref_count = 0;
	txn_global = &S2C(session)->txn_global;
	txn_state = &txn_global->states[session->id];

	/* Clear the transaction's ID from the global table. */
	WT_ASSERT(session, txn_state->id != WT_TXN_NONE &&
	    txn->id != WT_TXN_NONE);
	WT_PUBLISH(txn_state->id, WT_TXN_NONE);
	txn->id = WT_TXN_NONE;

	/*
	 * Reset the transaction state to not running.
	 *
	 * Auto-commit transactions (identified by having active cursors)
	 * handle this at a higher level.
	 */
	if (session->ncursors == 0)
		__wt_txn_release_snapshot(session);
	txn->isolation = session->isolation;
	F_CLR(txn,
	    TXN_DATA_SOURCE |
	    TXN_ERROR | TXN_FORCE_EVICT | TXN_OLDEST | TXN_RUNNING);

	/* Update the global generation number. */
	++txn_global->gen;
}

/*
 * __wt_txn_commit --
 *	Commit the current transaction.
 */
int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_TXN *txn;

	WT_UNUSED(cfg);

	txn = &session->txn;
	WT_ASSERT(session, !F_ISSET(txn, TXN_ERROR));

	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/*
	 * If a data-source was involved in the transaction, let it know we're
	 * committing.
	 */
	if (F_ISSET(txn, TXN_DATA_SOURCE))
		WT_RET(__wt_curds_txn_commit(session));

	/*
	 * Auto-commit transactions need a new transaction snapshot so that the
	 * committed changes are visible to subsequent reads.  However, cursor
	 * keys and values will point to the data that was just modified, so
	 * the snapshot cannot be so new that updates could be freed underneath
	 * the cursor.  Get the new snapshot before releasing the ID for the
	 * commit.
	 */
	if (session->ncursors > 0 && txn->isolation != TXN_ISO_READ_UNCOMMITTED)
		__wt_txn_refresh(session, txn->id + 1, 1);
	__wt_txn_release(session);
	return (0);
}

/*
 * __wt_txn_rollback --
 *	Roll back the current transaction.
 */
int
__wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_REF **rp;
	WT_TXN *txn;
	uint64_t **m;
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/*
	 * If a data-source was involved in the transaction, let it know we're
	 * rolling back.
	 */
	if (F_ISSET(txn, TXN_DATA_SOURCE))
		WT_TRET(__wt_curds_txn_rollback(session));

	/* Rollback updates. */
	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_ABORTED;

	/* Rollback fast deletes. */
	for (i = 0, rp = txn->modref; i < txn->modref_count; i++, rp++)
		__wt_tree_walk_delete_rollback(*rp);

	__wt_txn_release(session);
	return (ret);
}

/*
 * __wt_txn_init --
 *	Initialize a session's transaction data.
 */
int
__wt_txn_init(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	txn->id = WT_TXN_NONE;

	WT_RET(__wt_calloc_def(session,
	    S2C(session)->session_size, &txn->snapshot));

	/*
	 * Take care to clean these out in case we are reusing the transaction
	 * for eviction.
	 */
	txn->mod = NULL;
	txn->modref = NULL;

	txn->isolation = session->isolation;
	return (0);
}

/*
 * __wt_txn_destroy --
 *	Destroy a session's transaction data.
 */
void
__wt_txn_destroy(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	__wt_free(session, txn->mod);
	__wt_free(session, txn->modref);
	__wt_free(session, txn->snapshot);
}

/*
 * __wt_txn_global_init --
 *	Initialize the global transaction state.
 */
int
__wt_txn_global_init(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_SESSION_IMPL *session;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	u_int i;

	WT_UNUSED(cfg);
	session = conn->default_session;
	txn_global = &conn->txn_global;
	txn_global->current = 1;

	WT_RET(__wt_calloc_def(
	    session, conn->session_size, &txn_global->states));
	for (i = 0, s = txn_global->states; i < conn->session_size; i++, s++)
		s->id = s->snap_min = WT_TXN_NONE;

	return (0);
}

/*
 * __wt_txn_global_destroy --
 *	Destroy the global transaction state.
 */
void
__wt_txn_global_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;
	WT_TXN_GLOBAL *txn_global;

	session = conn->default_session;
	txn_global = &conn->txn_global;

	if (txn_global != NULL)
		__wt_free(session, txn_global->states);
}
