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
	wt_txnid_t id1, id2;

	id1 = *(wt_txnid_t *)v1;
	id2 = *(wt_txnid_t *)v2;

	return ((id1 == id2) ? 0 : TXNID_LT(id1, id2) ? -1 : 1);
}

/*
 * __txn_sort_snapshot --
 *	Sort a snapshot for faster searching and set the min/max bounds.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session,
    uint32_t n, wt_txnid_t id, wt_txnid_t oldest_snap_min)
{
	WT_TXN *txn;

	txn = &session->txn;

	if (n > 1)
		qsort(txn->snapshot, n, sizeof(wt_txnid_t), __wt_txnid_cmp);
	txn->snapshot_count = n;
	txn->snap_min = (n == 0) ? id : txn->snapshot[0];
	txn->snap_max = id;
	WT_ASSERT(session, n == 0 || txn->snap_min != WT_TXN_NONE);
	txn->oldest_snap_min = TXNID_LT(oldest_snap_min, txn->snap_min) ?
	    oldest_snap_min : txn->snap_min;
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
 * __wt_txn_get_oldest --
 *	Update the current transaction's cached copy of the oldest snap_min
 *	value.
 */
void
__wt_txn_get_oldest(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	wt_txnid_t id, oldest_snap_min;
	uint32_t i, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	oldest_snap_min =
	    (txn->id != WT_TXN_NONE) ? txn->id : txn_global->current;

	/* If nothing has changed since last time, we're done. */
	if (txn->last_oldest_gen == txn_global->gen &&
	    txn->last_oldest_id == oldest_snap_min)
		return;
	txn->last_oldest_gen = txn_global->gen;
	txn->last_oldest_id = oldest_snap_min;

	WT_ORDERED_READ(session_cnt, conn->session_cnt);
	for (i = 0, s = txn_global->states;
	    i < session_cnt;
	    i++, s++) {
		if ((id = s->snap_min) != WT_TXN_NONE &&
		    TXNID_LT(id, oldest_snap_min))
			oldest_snap_min = id;
	}

	txn->oldest_snap_min = oldest_snap_min;
}

/*
 * __wt_txn_get_snapshot --
 *	Set up a snapshot in the current transaction, without allocating an ID.
 */
void
__wt_txn_get_snapshot(
    WT_SESSION_IMPL *session, wt_txnid_t my_id, wt_txnid_t max_id, int force)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s, *txn_state;
	wt_txnid_t current_id, id, oldest_snap_min;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	/* If nothing has changed since last time, we're done. */
	if (!force && txn->last_id == txn_global->current &&
	    txn->last_gen == txn_global->gen) {
		txn_state->snap_min = txn->snap_min;
		return;
	}

	do {
		/* Take a copy of the current session ID. */
		txn->last_gen = txn->last_oldest_gen = txn_global->gen;
		txn->last_id = oldest_snap_min = current_id =
		    txn_global->current;

		/* Copy the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = n = 0, s = txn_global->states;
		    i < session_cnt;
		    i++, s++) {
			/* Ignore the session's own transaction. */
			if (i == session->id)
				continue;
			if ((id = s->snap_min) != WT_TXN_NONE)
				if (TXNID_LT(id, oldest_snap_min))
					oldest_snap_min = id;
			if ((id = s->id) == WT_TXN_NONE)
				continue;
			else if (max_id == WT_TXN_NONE || TXNID_LT(id, max_id))
				txn->snapshot[n++] = id;
		}

		/*
		 * Ensure the snapshot reads are scheduled before re-checking
		 * the global current ID.
		 */
		WT_READ_BARRIER();
	} while (current_id != txn_global->current);

	__txn_sort_snapshot(session, n,
	    (max_id != WT_TXN_NONE) ? max_id : current_id,
	    oldest_snap_min);
	txn_state->snap_min =
	    (my_id == WT_TXN_NONE || TXNID_LT(txn->snap_min, my_id)) ?
	    txn->snap_min : my_id;
}

/*
 * __wt_txn_get_evict_snapshot --
 *	Set up a snapshot in the current transaction for eviction.
 *	Only changes that visible to all active transactions can be evicted.
 */
void
__wt_txn_get_evict_snapshot(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;

	__wt_txn_get_oldest(session);
	__txn_sort_snapshot(
	    session, 0, txn->oldest_snap_min, txn->oldest_snap_min);

	/*
	 * Note that we carefully don't update the global table with this
	 * snap_min value: there is already a running transaction in this
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
	WT_TXN_STATE *s, *txn_state;
	wt_txnid_t id, oldest_snap_min;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];

	WT_ASSERT(session, txn_state->id == WT_TXN_NONE);

	WT_RET(__wt_config_gets_defno(session, cfg, "isolation", &cval));
	if (cval.len == 0)
		txn->isolation = session->isolation;
	else
		txn->isolation =
		    WT_STRING_MATCH("snapshot", cval.str, cval.len) ?
		    TXN_ISO_SNAPSHOT :
		    WT_STRING_MATCH("read-committed", cval.str, cval.len) ?
		    TXN_ISO_READ_COMMITTED : TXN_ISO_READ_UNCOMMITTED;

	F_SET(txn, TXN_RUNNING);

	do {
		/*
		 * Allocate a transaction ID.
		 *
		 * We use an atomic increment to ensure that we get a unique
		 * ID, then publish that to the global state table.
		 *
		 * If two threads race to allocate an ID, only the latest ID
		 * will proceed.  The winning thread can be sure its snapshot
		 * contains all of the earlier active IDs.  Threads that race
		 * and get an earlier ID may not appear in the snapshot,
		 * but they will loop and allocate a new ID before proceeding
		 * to make any updates.
		 *
		 * This potentially wastes transaction IDs when threads race to
		 * begin transactions, but that is the price we pay to keep
		 * this path latch free.
		 */
		do {
			txn->id = WT_ATOMIC_ADD(txn_global->current, 1);
		} while (txn->id == WT_TXN_NONE || txn->id == WT_TXN_ABORTED);
		WT_PUBLISH(txn_state->id, txn->id);

		/*
		 * If we are starting a snapshot isolation transaction, get
		 * a snapshot of the running transactions.
		 *
		 * If we already have a snapshot (e.g., for an auto-commit
		 * operation), update it so that the newly-allocated ID is
		 * visible.
		 */
		if (txn->isolation == TXN_ISO_SNAPSHOT) {
			txn->last_gen = txn->last_oldest_gen = txn_global->gen;
			oldest_snap_min = txn->id;

			/* Copy the array of concurrent transactions. */
			WT_ORDERED_READ(session_cnt, conn->session_cnt);
			for (i = n = 0, s = txn_global->states;
			    i < session_cnt;
			    i++, s++) {
				if ((id = s->snap_min) != WT_TXN_NONE)
					if (TXNID_LT(id, oldest_snap_min))
						oldest_snap_min = id;
				if ((id = s->id) == WT_TXN_NONE)
					continue;
				else
					txn->snapshot[n++] = id;
			}

			__txn_sort_snapshot(
			    session, n, txn->id, oldest_snap_min);
			txn_state->snap_min = txn->snap_min;
		}

		/*
		 * Ensure the snapshot reads are complete before re-checking
		 * the global current ID.
		 */
		WT_READ_BARRIER();
	} while (txn->id != txn_global->current);

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
	F_CLR(txn, TXN_ERROR | TXN_OLDEST | TXN_RUNNING);

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
	 * Auto-commit transactions need a new transaction snapshot so that the
	 * committed changes are visible to subsequent reads.  However, cursor
	 * keys and values will point to the data that was just modified, so
	 * the snapshot cannot be so new that updates could be freed underneath
	 * the cursor.  Get the new snapshot before releasing the ID for the
	 * commit.
	 */
	if (session->ncursors > 0)
		__wt_txn_get_snapshot(session, txn->id, WT_TXN_NONE, 1);
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
	WT_TXN *txn;
	wt_txnid_t **m;
	WT_REF **rp;
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/* Rollback updates. */
	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_ABORTED;

	/* Rollback fast deletes. */
	for (i = 0, rp = txn->modref; i < txn->modref_count; i++, rp++)
		__wt_tree_walk_delete_rollback(*rp);

	__wt_txn_release(session);
	return (0);
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

	/* The default isolation level is read-committed. */
	txn->isolation = session->isolation = TXN_ISO_READ_COMMITTED;

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
