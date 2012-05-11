/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __txnid_cmp --
 *	Compare transaction IDs for sorting / searching.
 */
static int
__txnid_cmp(const void *v1, const void *v2)
{
	wt_txnid_t id1, id2;

	id1 = *(wt_txnid_t *)v1;
	id2 = *(wt_txnid_t *)v2;

	return ((id1 == id2) ? 0 : TXNID_LT(id1, id2) ? -1 : 1);
}

/*
Non-blocking approach to getting a snapshot:

for (;;) {
  read conn->txnid into our slot, barrier
  copy the array of active transactions
  attempt to atomically swap conn->txnid to our txnid + 1
  if successful:
    break
}

sort our snapshot so we can efficiently bsearch for concurrent txns
then size to ignore any IDs in the snapshot >= our ID
*/
int
__wt_txn_begin(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	wt_txnid_t *myid;

	conn = S2C(session);
	txn = &session->txn;
	txn_global = &conn->txn_global;

	if (F_ISSET(txn, TXN_RUNNING)) {
		__wt_errx(session, "Transaction already running");
		return (EINVAL);
	}

	WT_RET(__wt_config_gets(session, cfg, "isolation", &cval));
	txn->isolation = (strcmp(cval.str, "snapshot") == 0) ?
	    TXN_ISO_SNAPSHOT : TXN_ISO_READ_UNCOMMITTED;

	WT_ASSERT(session, txn->id == WT_TXN_INVALID);
	WT_ASSERT(session, txn_global->ids[session->id] == WT_TXN_INVALID);

	do {
		/* Take a copy of the current session ID. */
		txn->id = txn_global->current;
		WT_PUBLISH(txn_global->ids[session->id], txn->id);

		if (txn->isolation == TXN_ISO_SNAPSHOT)
			/* Copy the array of concurrent transactions. */
			memcpy(txn->snapshot, txn_global->ids,
			    sizeof(wt_txnid_t) * conn->session_size);
	} while (!WT_ATOMIC_CAS(txn_global->current, txn->id, txn->id + 1) ||
	    txn->id == WT_TXN_NONE || txn->id == WT_TXN_INVALID);

	if (txn->isolation == TXN_ISO_SNAPSHOT) {
		/* Sort the snapshot and size for faster searching. */
		qsort(txn->snapshot, conn->session_size, sizeof(wt_txnid_t),
		    __txnid_cmp);
		myid = bsearch(&txn->id, txn->snapshot, conn->session_size,
		    sizeof(wt_txnid_t), __txnid_cmp);
		WT_ASSERT(session, myid != NULL);
		while (myid > txn->snapshot && myid[-1] == myid[0])
			--myid;
		txn->snap_min = txn->snapshot[0];
		txn->snapshot_count = (u_int)(myid - txn->snapshot);
	}

	F_SET(txn, TXN_RUNNING);

	return (0);
}

int
__wt_txn_release(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;

	txn = &session->txn;
	txn->mod_count = 0;

	if (!F_ISSET(txn, TXN_RUNNING)) {
		__wt_errx(session, "No transaction is active");
		return (EINVAL);
	}

	txn_global = &S2C(session)->txn_global;
	WT_ASSERT(session, txn_global->ids[session->id] != WT_TXN_INVALID);
	WT_PUBLISH(txn_global->ids[session->id], WT_TXN_INVALID);
	F_CLR(txn, TXN_RUNNING);

	return (0);
}

int
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
void
__wt_txn_unmodify(WT_SESSION_IMPL *session)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (F_ISSET(txn, TXN_RUNNING)) {
		WT_ASSERT(session, txn->mod_count > 0);
		txn->mod_count--;
	}
}

int
__wt_txn_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN *txn;

	WT_UNUSED(cfg);

	txn = &session->txn;
	ret = 0;

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
	WT_TRET(__wt_txn_release(session));
	return (ret);
}

int
__wt_txn_rollback(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_TXN *txn;
	wt_txnid_t **m;
	u_int i;

	WT_UNUSED(cfg);

	txn = &session->txn;
	ret = 0;

	for (i = 0, m = txn->mod; i < txn->mod_count; i++, m++)
		**m = WT_TXN_INVALID;
	WT_TRET(__wt_txn_release(session));
	return (ret);
}

/*
 * __wt_txn_visible --
 *	Can the current transaction see the given ID?
 */
int
__wt_txn_visible(WT_SESSION_IMPL *session, wt_txnid_t id)
{
	WT_TXN *txn;

	/* Nobody sees the results of aborted transactions. */
	if (id == WT_TXN_INVALID)
		return 0;

	/*
	 * Changes with no associated transaction are always visible, and
	 * non-snapshot transactions see all other changes.
	 */
	txn = &session->txn;
	if (id == WT_TXN_NONE || txn->isolation != TXN_ISO_SNAPSHOT)
		return 1;

	/*
	 * The snapshot test.
	 */
	if (TXNID_LT(id, txn->snap_min))
		return 1;
	if (TXNID_LT(txn->id, id))
		return 0;

	/*
	 * Otherwise, the ID is visible if it is not the result of a concurrent
	 * transaction.  That is, if it is not in the snapshot list.
	 */
	return (bsearch(&id, txn->snapshot, txn->snapshot_count,
	    sizeof(wt_txnid_t), __txnid_cmp) == NULL);
}

/*
 * __wt_txn_read --
 *	Get the first visible update in a list (or NULL if none are visible).
 */
WT_UPDATE *
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
int
__wt_txn_update_check(WT_SESSION_IMPL *session, WT_UPDATE *upd)
{
	WT_TXN *txn;

	txn = &session->txn;
	if (txn->isolation == TXN_ISO_SNAPSHOT)
		while (upd != NULL && !__wt_txn_visible(session, upd->txnid)) {
			if (upd->txnid != WT_TXN_INVALID)
				return (WT_DEADLOCK);
			upd = upd->next;
		}

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
	txn->id = WT_TXN_INVALID;

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
		txn_global->ids[i] = WT_TXN_INVALID;

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
