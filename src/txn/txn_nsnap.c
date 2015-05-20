/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __nsnap_destroy --
 *	Destroy a named snapshot structure.
 */
static void
__nsnap_destroy(WT_SESSION_IMPL *session, WT_NAMED_SNAPSHOT *nsnap)
{
	__wt_free(session, nsnap->name);
	__wt_free(session, nsnap->snapshot);
	__wt_free(session, nsnap);
}

/*
 * __wt_txn_named_snapshot_begin --
 *	Begin an named in-memory snapshot.
 */
int
__wt_txn_named_snapshot_begin(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_NAMED_SNAPSHOT *nsnap, *nsnap_new;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	const char *txn_cfg[] =
	    { WT_CONFIG_BASE(session, WT_SESSION_begin_transaction),
	      "isolation=snapshot", NULL };

	nsnap_new = NULL;
	txn_global = &S2C(session)->txn_global;
	txn = &session->txn;

	WT_UNUSED(cfg);

	WT_RET(__wt_config_gets_def(session, cfg, "name", 0, &cval));
	if (cval.len == 0)
		return (0);

	if (WT_STRING_MATCH("all", cval.str, cval.len))
		WT_RET_MSG(session, EINVAL,
		    "Can't create named snapshot with reserved \"all\" name");

	WT_RET(__wt_name_check(session, cval.str, cval.len));

	/*
	 * TODO more config checking -- make sure no checkpoint config
	 * is supplied.
	 *
	 * TODO what if a snapshot with that name exists?
	 */
	WT_RET(__wt_txn_begin(session, txn_cfg));

	/* Save a copy of the transaction's snapshot. */
	WT_ERR(__wt_calloc_one(session, &nsnap_new));
	nsnap = nsnap_new;
	WT_ERR(__wt_strndup(
	    session, cval.str, cval.len, &nsnap->name));
	nsnap->snap_min = txn->snap_min;
	nsnap->snap_max = txn->snap_max;
	if (txn->snapshot_count > 0) {
		WT_ERR(__wt_calloc_def(
		    session, txn->snapshot_count, &nsnap->snapshot));
		memcpy(nsnap->snapshot, txn->snapshot,
		    txn->snapshot_count * sizeof(*nsnap->snapshot));
	}
	nsnap->snapshot_count = txn->snapshot_count;

	/* Update the list. */
	WT_ERR(__wt_writelock(session, txn_global->nsnap_rwlock));

	if (STAILQ_EMPTY(&txn_global->nsnaph))
		txn_global->nsnap_oldest_id = nsnap_new->snap_min;
	STAILQ_INSERT_TAIL(&txn_global->nsnaph, nsnap_new, q);
	nsnap_new = NULL;

	WT_TRET(__wt_writeunlock(session, txn_global->nsnap_rwlock));

err:	if (F_ISSET(txn, WT_TXN_RUNNING))
		WT_TRET(__wt_txn_rollback(session, NULL));
	if (nsnap_new != NULL)
		__nsnap_destroy(session, nsnap_new);

	return (ret);
}

/*
 * __nsnap_drop_one --
 *	Drop a single named snapshot
 */
static int
__nsnap_drop_one(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name)
{
	WT_DECL_RET;
	WT_NAMED_SNAPSHOT *found;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;

	WT_RET(__wt_writelock(session, txn_global->nsnap_rwlock));

	STAILQ_FOREACH(found, &txn_global->nsnaph, q)
		if (WT_STRING_MATCH(found->name, name->str, name->len))
			break;

	if (found == NULL)
		WT_ERR_MSG(session, EINVAL,
		    "Named snapshot '%.*s' for drop not found",
		    (int)name->len, name->str);

	/* Bump the global ID if we are removing the first entry */
	if (found == STAILQ_FIRST(&txn_global->nsnaph))
		txn_global->nsnap_oldest_id = (STAILQ_NEXT(found, q) != NULL) ?
		    STAILQ_NEXT(found, q)->snap_min : WT_TXN_NONE;
	STAILQ_REMOVE(&txn_global->nsnaph, found, __wt_named_snapshot, q);
	__nsnap_destroy(session, found);

err:	WT_TRET(__wt_writeunlock(session, txn_global->nsnap_rwlock));

	return (ret);
}

/*
 * __nsnap_drop_to --
 *	Drop named snapshots
 */
static int
__nsnap_drop_to(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name)
{
	WT_DECL_RET;
	WT_NAMED_SNAPSHOT *last, *nsnap;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;

	WT_RET(__wt_writelock(session, txn_global->nsnap_rwlock));

	if (STAILQ_EMPTY(&txn_global->nsnaph))
		goto err;

	/*
	 * TODO: We are in trouble if this drop exits on error. Maybe we
	 * should only update the special oldest ID on success, but that's
	 * likely to leave the state wrong as well (conservatively, rather than
	 * aggressively though).
	 */
	if (WT_STRING_MATCH("all", name->str, name->len))
		txn_global->nsnap_oldest_id = WT_TXN_NONE;
	else {
		STAILQ_FOREACH(last, &txn_global->nsnaph, q)
			if (WT_STRING_MATCH(last->name, name->str, name->len))
				break;
		if (last == NULL)
			WT_ERR_MSG(session, EINVAL,
			    "Named snapshot '%.*s' for drop not found",
			    (int)name->len, name->str);
		txn_global->nsnap_oldest_id = (STAILQ_NEXT(last, q) != NULL) ?
		    STAILQ_NEXT(last, q)->snap_min : WT_TXN_NONE;
	}

	do {
		nsnap = STAILQ_FIRST(&txn_global->nsnaph);
		WT_ASSERT(session, nsnap != NULL);
		STAILQ_REMOVE_HEAD(&txn_global->nsnaph, q);
		__nsnap_destroy(session, nsnap);
	/* Last will be NULL in the all case so it will never match */
	} while (nsnap != last && !STAILQ_EMPTY(&txn_global->nsnaph));

err:	WT_TRET(__wt_writeunlock(session, txn_global->nsnap_rwlock));

	return (ret);
}

/*
 * __wt_txn_named_snapshot_drop --
 *	Drop named snapshots
 */
int
__wt_txn_named_snapshot_drop(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONFIG objectconf;
	WT_CONFIG_ITEM cval, k, v;
	WT_DECL_RET;

	WT_RET(__wt_config_gets_def(session, cfg, "drop.to", 0, &cval));
	if (cval.len != 0)
		WT_RET(__nsnap_drop_to(session, &cval));

	WT_RET(__wt_config_gets_def(
	    session, cfg, "drop.names", 0, &cval));

	/* We are done if there are no named drops */
	if (cval.val == 0)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "drop.names", &cval));
	WT_RET(__wt_config_subinit(session, &objectconf, &cval));
	while ((ret = __wt_config_next(&objectconf, &k, &v)) == 0)
		WT_RET(__nsnap_drop_one(session, &k));
	if (ret == WT_NOTFOUND)
		ret = 0;

	return (ret);
}

/*
 * __wt_txn_nsnap_get --
 *	Lookup a named snapshot for a transaction.
 */
int
__wt_txn_nsnap_get(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *nameval)
{
	WT_NAMED_SNAPSHOT *nsnap;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = &S2C(session)->txn_global.states[session->id];

	txn->isolation = WT_ISO_SNAPSHOT;
	if (session->ncursors > 0)
		WT_RET(__wt_session_copy_values(session));

	WT_RET(__wt_readlock(session, txn_global->nsnap_rwlock));
	STAILQ_FOREACH(nsnap, &txn_global->nsnaph, q)
		if (WT_STRING_MATCH(
		    nsnap->name, nameval->str, nameval->len)) {
			txn->snap_min = txn_state->snap_min = nsnap->snap_min;
			txn->snap_max = nsnap->snap_max;
			if ((txn->snapshot_count = nsnap->snapshot_count) != 0)
				memcpy(txn->snapshot, nsnap->snapshot,
				    nsnap->snapshot_count *
				    sizeof(*nsnap->snapshot));
			F_SET(txn, WT_TXN_HAS_SNAPSHOT);
			break;
		}
	WT_RET(__wt_readunlock(session, txn_global->nsnap_rwlock));

	if (nsnap == NULL)
		WT_RET_MSG(session, EINVAL,
		    "Named snapshot '%.*s' not found",
		    (int)nameval->len, nameval->str);

	/* Flag that this transaction is opened on a named snapshot */
	F_SET(txn, WT_TXN_NAMED_SNAPSHOT);

	return (0);
}

/*
 * __wt_txn_nsnap_destroy --
 *	Lookup a named snapshot for a transaction.
 */
int
__wt_txn_nsnap_destroy(WT_SESSION_IMPL *session)
{
	WT_NAMED_SNAPSHOT *nsnap;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;
	txn_global->nsnap_oldest_id = WT_TXN_NONE;

	while (!STAILQ_EMPTY(&txn_global->nsnaph)) {
		nsnap = STAILQ_FIRST(&txn_global->nsnaph);
		WT_ASSERT(session, nsnap != NULL);
		STAILQ_REMOVE_HEAD(&txn_global->nsnaph, q);
		__nsnap_destroy(session, nsnap);
	}

	return (0);
}
