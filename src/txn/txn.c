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
__txn_sort_snapshot(WT_SESSION_IMPL *session, uint32_t n, wt_txnid_t id)
{
	WT_TXN *txn;

	txn = &session->txn;

	qsort(txn->snapshot, n, sizeof(wt_txnid_t), __wt_txnid_cmp);
	txn->snapshot_count = n;
	txn->snap_min = (n == 0) ? id : txn->snapshot[0];
	txn->snap_max = (n == 0) ? id : txn->snapshot[n - 1];
	WT_ASSERT(session, txn->snap_min != WT_TXN_NONE);
}

/*
 * __wt_txn_get_snapshot --
 *	Set up a snapshot in the current transaction, without allocating an ID.
 */
int
__wt_txn_get_snapshot(WT_SESSION_IMPL *session, wt_txnid_t max_id)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_txnid_t current_id, id;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	n = 0;
	txn = &session->txn;
	txn_global = &conn->txn_global;

	do {
		/* Take a copy of the current session ID. */
		current_id = txn_global->current;

		/* Copy the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = 0; i < session_cnt; i++)
			if ((id = txn_global->ids[i]) != WT_TXN_NONE &&
			    (max_id == WT_TXN_NONE || TXNID_LT(id, max_id)))
				txn->snapshot[n++] = id;
	} while (current_id != txn_global->current);

	__txn_sort_snapshot(
	    session, n, (max_id != WT_TXN_NONE) ? max_id : current_id);
	return (0);
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
	wt_txnid_t id;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	n = 0;
	txn = &session->txn;
	txn_global = &conn->txn_global;

	if (F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "Transaction already running");

	WT_RET(__wt_config_gets(session, cfg, "isolation", &cval));
	txn->isolation = (__wt_config_strcmp(&cval, "snapshot") == 0) ?
	    TXN_ISO_SNAPSHOT : TXN_ISO_READ_UNCOMMITTED;

	WT_ASSERT(session, txn->id == WT_TXN_NONE);
	WT_ASSERT(session, txn_global->ids[session->id] == WT_TXN_NONE);

	do {
		/* Take a copy of the current session ID. */
		txn->id = txn_global->current;
		WT_PUBLISH(txn_global->ids[session->id], txn->id);

		if (txn->isolation == TXN_ISO_SNAPSHOT) {
			/* Copy the array of concurrent transactions. */
			WT_ORDERED_READ(session_cnt, conn->session_cnt);
			for (i = 0; i < session_cnt; i++)
				if ((id = txn_global->ids[i]) != WT_TXN_NONE &&
				    TXNID_LT(id, txn->id))
					txn->snapshot[n++] = id;
		}
	} while (!WT_ATOMIC_CAS(txn_global->current, txn->id, txn->id + 1) ||
	    txn->id == WT_TXN_NONE || txn->id == WT_TXN_ABORTED);

	if (txn->isolation == TXN_ISO_SNAPSHOT)
		__txn_sort_snapshot(session, n, txn->id);

	F_SET(txn, TXN_RUNNING);

	return (0);
}

/*
 * __wt_txn_release --
 *	Release the resources associated with the current transaction.
 */
int
__wt_txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn->mod_count = 0;

	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	/* Clear the transaction's ID from the global table. */
	txn_global = &S2C(session)->txn_global;
	WT_ASSERT(session, txn_global->ids[session->id] != WT_TXN_NONE &&
	    txn->id != WT_TXN_NONE);
	WT_PUBLISH(txn_global->ids[session->id], txn->id = WT_TXN_NONE);

	/* Reset the transaction state to not running. */
	txn->isolation = TXN_ISO_READ_UNCOMMITTED;
	F_CLR(txn, TXN_ERROR | TXN_RUNNING);

	return (0);
}

/*
 * __wt_txn_commit --
 *	Commit the current transaction.
 */
int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	return (__wt_txn_release(session));
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
	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_ABORTED;

	return (__wt_txn_release(session));
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
	u_int i;

	WT_UNUSED(cfg);
	session = conn->default_session;
	txn_global = &conn->txn_global;
	txn_global->current = 1;
	txn_global->ckpt_txnid = WT_TXN_NONE;

	WT_RET(__wt_calloc_def(session, conn->session_size, &txn_global->ids));
	for (i = 0; i < conn->session_size; i++)
		txn_global->ids[i] = WT_TXN_NONE;

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

	__wt_free(session, txn_global->ids);
}
