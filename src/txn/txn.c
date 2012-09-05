/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
	WT_ASSERT(session, txn->snap_min != WT_TXN_NONE);
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
	WT_TXN *txn;

	txn = &session->txn;
	txn->snapshot_count = 0;
}

/*
 * __wt_txn_get_snapshot --
 *	Set up a snapshot in the current transaction, without allocating an ID.
 */
void
__wt_txn_get_snapshot(WT_SESSION_IMPL *session, wt_txnid_t max_id)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	wt_txnid_t current_id, id, oldest_snap_min;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	n = 0;
	txn = &session->txn;
	txn_global = &conn->txn_global;
	oldest_snap_min = WT_TXN_ABORTED;

	do {
		/* Take a copy of the current session ID. */
		current_id = txn_global->current;

		/* Copy the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = 0, s = txn_global->states; i < session_cnt; i++, s++) {
			if ((id = s->snap_min) != WT_TXN_NONE)
				if (TXNID_LT(id, oldest_snap_min))
					oldest_snap_min = id;
			if ((id = s->id) == WT_TXN_NONE)
				continue;
			else if (max_id == WT_TXN_NONE || TXNID_LT(id, max_id))
				txn->snapshot[n++] = id;
		}
	} while (current_id != txn_global->current);

	__txn_sort_snapshot(session, n,
	    (max_id != WT_TXN_NONE) ? max_id : current_id,
	    oldest_snap_min);
}

/*
 * __wt_txn_get_evict_snapshot --
 *	Set up a snapshot in the current transaction for eviction.
 *	No changes that are invisible to any active transaction can be evicted.
 */
void
__wt_txn_get_evict_snapshot(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *s;
	wt_txnid_t current_id, id, oldest_snap_min;
	uint32_t i, session_cnt;

	conn = S2C(session);
	txn_global = &conn->txn_global;
	oldest_snap_min = WT_TXN_ABORTED;

	do {
		/* Take a copy of the current session ID. */
		current_id = txn_global->current;

		/* Walk the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = 0, s = txn_global->states; i < session_cnt; i++, s++)
			if ((id = s->snap_min) != WT_TXN_NONE &&
			    TXNID_LT(id, oldest_snap_min))
				oldest_snap_min = id;
	} while (current_id != txn_global->current);

	__txn_sort_snapshot(session, 0, oldest_snap_min, oldest_snap_min);
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
	n = 0;
	txn = &session->txn;
	txn_global = &conn->txn_global;
	txn_state = &txn_global->states[session->id];
	oldest_snap_min = WT_TXN_ABORTED;

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

	WT_ASSERT(session, session->ncursors == 0);

	F_SET(txn, TXN_RUNNING);

	do {
		/* Take a copy of the current session ID. */
		txn->id = txn_global->current;
		WT_PUBLISH(txn_state->id, txn->id);

		if (txn->isolation == TXN_ISO_SNAPSHOT) {
			/* Copy the array of concurrent transactions. */
			WT_ORDERED_READ(session_cnt, conn->session_cnt);
			for (i = 0, s = txn_global->states;
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
	} while (!WT_ATOMIC_CAS(txn_global->current, txn->id, txn->id + 1) ||
	    txn->id == WT_TXN_NONE || txn->id == WT_TXN_ABORTED);

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
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn->mod_count = 0;
	txn_state = &S2C(session)->txn_global.states[session->id];

	/* Clear the transaction's ID from the global table. */
	WT_ASSERT(session, txn_state->id != WT_TXN_NONE &&
	    txn->id != WT_TXN_NONE);
	txn_state->id = txn_state->snap_min = WT_TXN_NONE;

	/* Reset the transaction state to not running. */
	txn->id = WT_TXN_NONE;
	__wt_txn_release_snapshot(session);
	txn->isolation = session->isolation;
	F_CLR(txn, TXN_ERROR | TXN_RUNNING);
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
	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

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
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_ABORTED;

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

	__wt_free(session, txn_global->states);
}
