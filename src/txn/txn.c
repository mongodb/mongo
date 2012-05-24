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
 *	Sort a snapshot and size for faster searching.
 */
static void
__txn_sort_snapshot(WT_SESSION_IMPL *session, wt_txnid_t id, uint32_t n)
{
	WT_TXN *txn;
	wt_txnid_t *lastid;

	txn = &session->txn;

	/* Sort the snapshot and size for faster searching. */
	qsort(txn->snapshot, n, sizeof(wt_txnid_t), __wt_txnid_cmp);
	lastid = bsearch(&id, txn->snapshot, n, sizeof(wt_txnid_t),
	    __wt_txnid_cmp);
	WT_ASSERT(session, lastid != NULL);
	while (lastid > txn->snapshot && lastid[-1] == lastid[0])
		--lastid;
	txn->snap_min = txn->snapshot[0];
	WT_ASSERT(session, txn->snap_min != WT_TXN_NONE);
	txn->snapshot_count = (u_int)(lastid - txn->snapshot);
}

/*
 * __wt_txn_get_snapshot --
 *	Set up a snapshot in the current transaction, without allocating an ID.
 */
int
__wt_txn_get_snapshot(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_txnid_t id;
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	do {
		/* Take a copy of the current session ID. */
		id = txn_global->current;

		/* Copy the array of concurrent transactions. */
		WT_ORDERED_READ(session_cnt, conn->session_cnt);
		for (i = n = 0; i < session_cnt; i++)
			if ((txn->snapshot[n] =
			    txn_global->ids[i]) != WT_TXN_NONE)
				++n;
	} while (txn_global->current != id);

	__txn_sort_snapshot(session, id, n);
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
	uint32_t i, n, session_cnt;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	if (F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "Transaction already running");

	WT_RET(__wt_config_gets(session, cfg, "isolation", &cval));
	txn->isolation = (strcmp(cval.str, "snapshot") == 0) ?
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
			for (i = n = 0; i < conn->session_cnt; i++)
				if ((txn->snapshot[n] =
				    txn_global->ids[i]) != WT_TXN_NONE)
					++n;
		}
	} while (!WT_ATOMIC_CAS(txn_global->current, txn->id, txn->id + 1) ||
	    txn->id == WT_TXN_NONE || txn->id == WT_TXN_ABORTED);

	if (txn->isolation == TXN_ISO_SNAPSHOT)
		__txn_sort_snapshot(session, txn->id, n);

	F_SET(txn, TXN_RUNNING);

	return (0);
}

/*
 * __txn_release --
 *	Release the resources associated with the current transaction.
 */
static int
__txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn->mod_count = 0;

	if (!F_ISSET(txn, TXN_RUNNING))
		WT_RET_MSG(session, EINVAL, "No transaction is active");

	txn_global = &S2C(session)->txn_global;
	WT_ASSERT(session, txn_global->ids[session->id] != WT_TXN_NONE &&
	    txn->id != WT_TXN_NONE);
	WT_PUBLISH(txn_global->ids[session->id], txn->id = WT_TXN_NONE);
	F_CLR(txn, TXN_RUNNING);

	return (0);
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

#if 0
	/*
	 * We want to avoid this cost in application transactions -- the only
	 * reason to do it is if 2 billion transactions are executed between
	 * writes of a page...
	 */
	wt_txnid_t **m;
	u_int i;
	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_NONE;
#endif
	return (__txn_release(session));
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

	return (__txn_release(session));
}

/*
 * __wt_txn_checkpoint --
 *	Write a checkpoint.
 */
int
__wt_txn_checkpoint(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_TXN_GLOBAL *txn_global;
	const char *snapshot;
	const char *txn_cfg[] = { "isolation=snapshot", NULL };

	cursor = NULL;
	txn_global = &S2C(session)->txn_global;

	if ((ret = __wt_config_gets(
	    session, cfg, "snapshot", &cval)) != 0 && ret != WT_NOTFOUND)
		WT_RET(ret);
	if (cval.len != 0)
		WT_RET(__wt_strndup(session, cval.str, cval.len, &snapshot));
	else
		snapshot = NULL;

	/* Only one checkpoint can be active at a time. */
	__wt_writelock(session, S2C(session)->ckpt_rwlock);

	WT_ERR(__wt_txn_begin(session, txn_cfg));

	/* TODO: prevent eviction from evicting anything newer than this... */
	txn_global->checkpoint_txn = &session->txn;

	/*
	 * If we're doing an ordinary unnamed checkpoint, we only need to flush
	 * open files.	If we're creating a named snapshot, we need to walk the
	 * entire list of files in the metadata.
	 */
	WT_TRET((snapshot == NULL) ?
	    __wt_conn_btree_apply(session, __wt_snapshot, cfg) :
	    __wt_meta_btree_apply(session, __wt_snapshot, cfg, 0));

	if (cursor != NULL)
		WT_TRET(cursor->close(cursor));

	txn_global->checkpoint_txn = NULL;

	WT_TRET(__txn_release(session));

err:	__wt_rwunlock(session, S2C(session)->ckpt_rwlock);
	__wt_free(session, snapshot);
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
