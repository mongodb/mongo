/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
 * __nsnap_drop_one --
 *	Drop a single named snapshot. The named snapshot lock must be held
 *	write locked.
 */
static int
__nsnap_drop_one(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name)
{
	WT_DECL_RET;
	WT_NAMED_SNAPSHOT *found;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;

	TAILQ_FOREACH(found, &txn_global->nsnaph, q)
		if (WT_STRING_MATCH(found->name, name->str, name->len))
			break;

	if (found == NULL)
		return (WT_NOTFOUND);

	/* Bump the global ID if we are removing the first entry */
	if (found == TAILQ_FIRST(&txn_global->nsnaph))
		txn_global->nsnap_oldest_id = (TAILQ_NEXT(found, q) != NULL) ?
		    TAILQ_NEXT(found, q)->snap_min : WT_TXN_NONE;
	TAILQ_REMOVE(&txn_global->nsnaph, found, q);
	__nsnap_destroy(session, found);
	WT_STAT_FAST_CONN_INCR(session, txn_snapshots_dropped);

	return (ret);
}

/*
 * __nsnap_drop_to --
 *	Drop named snapshots, if the name is NULL all snapshots will be
 *	dropped. The named snapshot lock must be held write locked.
 */
static int
__nsnap_drop_to(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *name, bool inclusive)
{
	WT_DECL_RET;
	WT_NAMED_SNAPSHOT *last, *nsnap, *prev;
	WT_TXN_GLOBAL *txn_global;
	uint64_t new_nsnap_oldest;

	last = nsnap = prev = NULL;
	txn_global = &S2C(session)->txn_global;

	if (TAILQ_EMPTY(&txn_global->nsnaph)) {
		if (name == NULL)
			return (0);
		/*
		 * Dropping specific snapshots when there aren't any it's an
		 * error.
		 */
		WT_RET_MSG(session, EINVAL,
		    "Named snapshot '%.*s' for drop not found",
		    (int)name->len, name->str);
	}

	/*
	 * The new ID will be none if we are removing all named snapshots
	 * which is the default behavior of this loop.
	 */
	new_nsnap_oldest = WT_TXN_NONE;
	if (name != NULL) {
		TAILQ_FOREACH(last, &txn_global->nsnaph, q) {
			if (WT_STRING_MATCH(last->name, name->str, name->len))
				break;
			prev = last;
		}
		if (last == NULL)
			WT_RET_MSG(session, EINVAL,
			    "Named snapshot '%.*s' for drop not found",
			    (int)name->len, name->str);

		if (!inclusive) {
			/* We are done if a drop before points to the head */
			if (prev == 0)
				return (0);
			last = prev;
		}

		if (TAILQ_NEXT(last, q) != NULL)
			new_nsnap_oldest = TAILQ_NEXT(last, q)->snap_min;
	}

	do {
		nsnap = TAILQ_FIRST(&txn_global->nsnaph);
		WT_ASSERT(session, nsnap != NULL);
		TAILQ_REMOVE(&txn_global->nsnaph, nsnap, q);
		__nsnap_destroy(session, nsnap);
		WT_STAT_FAST_CONN_INCR(session, txn_snapshots_dropped);
	/* Last will be NULL in the all case so it will never match */
	} while (nsnap != last && !TAILQ_EMPTY(&txn_global->nsnaph));

	/* Now that the queue of named snapshots is updated, update the ID */
	txn_global->nsnap_oldest_id = new_nsnap_oldest;

	return (ret);
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
	bool started_txn;

	started_txn = false;
	nsnap_new = NULL;
	txn_global = &S2C(session)->txn_global;
	txn = &session->txn;

	WT_RET(__wt_config_gets_def(session, cfg, "name", 0, &cval));
	WT_ASSERT(session, cval.len != 0);

	if (!F_ISSET(txn, WT_TXN_RUNNING)) {
		WT_RET(__wt_txn_begin(session, txn_cfg));
		started_txn = true;
	}
	F_SET(txn, WT_TXN_READONLY);

	/* Save a copy of the transaction's snapshot. */
	WT_ERR(__wt_calloc_one(session, &nsnap_new));
	nsnap = nsnap_new;
	WT_ERR(__wt_strndup(session, cval.str, cval.len, &nsnap->name));
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

	/*
	 * The semantic is that a new snapshot with the same name as an
	 * existing snapshot will replace the old one.
	 */
	WT_ERR_NOTFOUND_OK(__nsnap_drop_one(session, &cval));

	if (TAILQ_EMPTY(&txn_global->nsnaph))
		txn_global->nsnap_oldest_id = nsnap_new->snap_min;
	TAILQ_INSERT_TAIL(&txn_global->nsnaph, nsnap_new, q);
	WT_STAT_FAST_CONN_INCR(session, txn_snapshots_created);
	nsnap_new = NULL;

err:	if (started_txn)
		WT_TRET(__wt_txn_rollback(session, NULL));
	else if (ret == 0)
		F_SET(txn, WT_TXN_NAMED_SNAPSHOT);

	if (nsnap_new != NULL)
		__nsnap_destroy(session, nsnap_new);

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
	WT_CONFIG_ITEM all_config, k, names_config, to_config, before_config, v;
	WT_DECL_RET;

	WT_RET(__wt_config_gets_def(session, cfg, "drop.all", 0, &all_config));
	WT_RET(__wt_config_gets_def(
	    session, cfg, "drop.names", 0, &names_config));
	WT_RET(__wt_config_gets_def(session, cfg, "drop.to", 0, &to_config));
	WT_RET(__wt_config_gets_def(
	    session, cfg, "drop.before", 0, &before_config));

	if (all_config.val != 0)
		WT_RET(__nsnap_drop_to(session, NULL, true));
	else if (before_config.len != 0)
		WT_RET(__nsnap_drop_to(session, &before_config, false));
	else if (to_config.len != 0)
		WT_RET(__nsnap_drop_to(session, &to_config, true));

	/* We are done if there are no named drops */

	if (names_config.len != 0) {
		WT_RET(__wt_config_subinit(
		    session, &objectconf, &names_config));
		while ((ret = __wt_config_next(&objectconf, &k, &v)) == 0) {
			ret = __nsnap_drop_one(session, &k);
			if (ret != 0)
				WT_RET_MSG(session, EINVAL,
				    "Named snapshot '%.*s' for drop not found",
				    (int)k.len, k.str);
		}
		if (ret == WT_NOTFOUND)
			ret = 0;
	}

	return (ret);
}

/*
 * __wt_txn_named_snapshot_get --
 *	Lookup a named snapshot for a transaction.
 */
int
__wt_txn_named_snapshot_get(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *nameval)
{
	WT_NAMED_SNAPSHOT *nsnap;
	WT_TXN *txn;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;

	txn = &session->txn;
	txn_global = &S2C(session)->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);

	txn->isolation = WT_ISO_SNAPSHOT;
	if (session->ncursors > 0)
		WT_RET(__wt_session_copy_values(session));

	WT_RET(__wt_readlock(session, txn_global->nsnap_rwlock));
	TAILQ_FOREACH(nsnap, &txn_global->nsnaph, q)
		if (WT_STRING_MATCH(nsnap->name, nameval->str, nameval->len)) {
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
 * __wt_txn_named_snapshot_config --
 *	Check the configuration for a named snapshot
 */
int
__wt_txn_named_snapshot_config(WT_SESSION_IMPL *session,
    const char *cfg[], bool *has_create, bool *has_drops)
{
	WT_CONFIG_ITEM cval;
	WT_CONFIG_ITEM all_config, names_config, to_config, before_config;
	WT_TXN *txn;

	txn = &session->txn;
	*has_create = *has_drops = false;

	/* Verify that the name is legal. */
	WT_RET(__wt_config_gets_def(session, cfg, "name", 0, &cval));
	if (cval.len != 0) {
		if (WT_STRING_MATCH("all", cval.str, cval.len))
			WT_RET_MSG(session, EINVAL,
			    "Can't create snapshot with reserved \"all\" name");

		WT_RET(__wt_name_check(session, cval.str, cval.len));

		if (F_ISSET(txn, WT_TXN_RUNNING) &&
		    txn->isolation != WT_ISO_SNAPSHOT)
			WT_RET_MSG(session, EINVAL,
			    "Can't create a named snapshot from a running "
			    "transaction that isn't snapshot isolation");
		else if (F_ISSET(txn, WT_TXN_RUNNING) && txn->mod_count != 0)
			WT_RET_MSG(session, EINVAL,
			    "Can't create a named snapshot from a running "
			    "transaction that has made updates");
		*has_create = true;
	}

	/* Verify that the drop configuration is sane. */
	WT_RET(__wt_config_gets_def(session, cfg, "drop.all", 0, &all_config));
	WT_RET(__wt_config_gets_def(
	    session, cfg, "drop.names", 0, &names_config));
	WT_RET(__wt_config_gets_def(session, cfg, "drop.to", 0, &to_config));
	WT_RET(__wt_config_gets_def(
	    session, cfg, "drop.before", 0, &before_config));

	/* Avoid more work if no drops are configured. */
	if (all_config.val != 0 || names_config.len != 0 ||
	    before_config.len != 0 || to_config.len != 0) {
		if (before_config.len != 0 && to_config.len != 0)
			WT_RET_MSG(session, EINVAL,
			    "Illegal configuration; named snapshot drop can't "
			    "specify both before and to options");
		if (all_config.val != 0 && (names_config.len != 0 ||
		    to_config.len != 0 || before_config.len != 0))
			WT_RET_MSG(session, EINVAL,
			    "Illegal configuration; named snapshot drop can't "
			    "specify all and any other options");
		*has_drops = true;
	}

	if (!*has_create && !*has_drops)
		WT_RET_MSG(session, EINVAL,
		    "WT_SESSION::snapshot API called without any drop or "
		    "name option");

	return (0);
}

/*
 * __wt_txn_named_snapshot_destroy --
 *	Destroy all named snapshots on connection close
 */
int
__wt_txn_named_snapshot_destroy(WT_SESSION_IMPL *session)
{
	WT_NAMED_SNAPSHOT *nsnap;
	WT_TXN_GLOBAL *txn_global;

	txn_global = &S2C(session)->txn_global;
	txn_global->nsnap_oldest_id = WT_TXN_NONE;

	while ((nsnap = TAILQ_FIRST(&txn_global->nsnaph)) != NULL) {
		TAILQ_REMOVE(&txn_global->nsnaph, nsnap, q);
		__nsnap_destroy(session, nsnap);
	}

	return (0);
}
