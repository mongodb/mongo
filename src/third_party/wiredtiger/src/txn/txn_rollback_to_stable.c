/*-
 * Copyright (c) 2014-2018 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_TIMESTAMPS
/*
 * __txn_rollback_to_stable_lookaside_fixup --
 *	Remove any updates that need to be rolled back from the lookaside file.
 */
static int
__txn_rollback_to_stable_lookaside_fixup(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_DECL_TIMESTAMP(rollback_timestamp)
	WT_ITEM las_key, las_timestamp, las_value;
	WT_TXN_GLOBAL *txn_global;
	uint64_t las_counter, las_pageid, las_total, las_txnid, remove_cnt;
	uint32_t las_id, session_flags;
	uint8_t upd_type;

	conn = S2C(session);
	cursor = NULL;
	las_total = remove_cnt = 0;
	session_flags = 0;		/* [-Werror=maybe-uninitialized] */
	WT_CLEAR(las_timestamp);

	/*
	 * Copy the stable timestamp, otherwise we'd need to lock it each time
	 * it's accessed. Even though the stable timestamp isn't supposed to be
	 * updated while rolling back, accessing it without a lock would
	 * violate protocol.
	 */
	txn_global = &conn->txn_global;
	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    __wt_timestamp_set(
	    &rollback_timestamp, &txn_global->stable_timestamp));

	__wt_las_cursor(session, &cursor, &session_flags);

	/* Discard pages we read as soon as we're done with them. */
	F_SET(session, WT_SESSION_READ_WONT_NEED);

	/* Walk the file. */
	__wt_writelock(session, &conn->cache->las_sweepwalk_lock);
	while ((ret = cursor->next(cursor)) == 0) {
		WT_ERR(cursor->get_key(cursor,
		    &las_pageid, &las_id, &las_counter, &las_key));

		/* Check the file ID so we can skip durable tables */
		if (las_id >= conn->stable_rollback_maxfile)
			WT_PANIC_RET(session, EINVAL, "file ID %" PRIu32
			    " in lookaside table larger than max %" PRIu32,
			    las_id, conn->stable_rollback_maxfile);
		if (__bit_test(conn->stable_rollback_bitstring, las_id))
			continue;

		WT_ERR(cursor->get_value(cursor,
		    &las_txnid, &las_timestamp, &upd_type, &las_value));

		/*
		 * Entries with no timestamp will have a timestamp of zero,
		 * which will fail the following check and cause them to never
		 * be removed.
		 */
		if (__wt_timestamp_cmp(
		    &rollback_timestamp, las_timestamp.data) < 0) {
			WT_ERR(cursor->remove(cursor));
			++remove_cnt;
		} else
			++las_total;
	}
	WT_ERR_NOTFOUND_OK(ret);
err:	__wt_writeunlock(session, &conn->cache->las_sweepwalk_lock);
	WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));
	__wt_cache_decr_check_uint64(session,
	    &conn->cache->las_entry_count, remove_cnt, "lookaside entry count");
	WT_STAT_CONN_SET(session, cache_lookaside_entries, las_total);

	F_CLR(session, WT_SESSION_READ_WONT_NEED);

	return (ret);
}

/*
 * __txn_abort_newer_update --
 *	Abort updates in an update change with timestamps newer than the
 *	rollback timestamp.
 */
static void
__txn_abort_newer_update(WT_SESSION_IMPL *session,
    WT_UPDATE *upd, wt_timestamp_t *rollback_timestamp)
{
	WT_UPDATE *next_upd;
	bool aborted_one;

	aborted_one = false;
	for (next_upd = upd; next_upd != NULL; next_upd = next_upd->next) {
		/*
		 * Updates with no timestamp will have a timestamp of zero
		 * which will fail the following check and cause them to never
		 * be aborted.
		 */
		if (__wt_timestamp_cmp(
		    rollback_timestamp, &next_upd->timestamp) < 0) {
			next_upd->txnid = WT_TXN_ABORTED;
			__wt_timestamp_set_zero(&next_upd->timestamp);

			/*
			* If any updates are aborted, all newer updates
			* better be aborted as well.
			*/
			if (!aborted_one)
				WT_ASSERT(session,
				    !aborted_one || upd == next_upd);
			aborted_one = true;
		}
	}
}

/*
 * __txn_abort_newer_insert --
 *	Apply the update abort check to each entry in an insert skip list
 */
static void
__txn_abort_newer_insert(WT_SESSION_IMPL *session,
    WT_INSERT_HEAD *head, wt_timestamp_t *rollback_timestamp)
{
	WT_INSERT *ins;

	WT_SKIP_FOREACH(ins, head)
		__txn_abort_newer_update(session, ins->upd, rollback_timestamp);
}

/*
 * __txn_abort_newer_col_var --
 *	Abort updates on a variable length col leaf page with timestamps newer
 *	than the rollback timestamp.
 */
static void
__txn_abort_newer_col_var(
    WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t *rollback_timestamp)
{
	WT_COL *cip;
	WT_INSERT_HEAD *ins;
	uint32_t i;

	/* Review the changes to the original on-page data items */
	WT_COL_FOREACH(page, cip, i)
		if ((ins = WT_COL_UPDATE(page, cip)) != NULL)
			__txn_abort_newer_insert(session,
			    ins, rollback_timestamp);

	/* Review the append list */
	if ((ins = WT_COL_APPEND(page)) != NULL)
		__txn_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __txn_abort_newer_col_fix --
 *	Abort updates on a fixed length col leaf page with timestamps newer than
 *	the rollback timestamp.
 */
static void
__txn_abort_newer_col_fix(
    WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t *rollback_timestamp)
{
	WT_INSERT_HEAD *ins;

	/* Review the changes to the original on-page data items */
	if ((ins = WT_COL_UPDATE_SINGLE(page)) != NULL)
		__txn_abort_newer_insert(session, ins, rollback_timestamp);

	/* Review the append list */
	if ((ins = WT_COL_APPEND(page)) != NULL)
		__txn_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __txn_abort_newer_row_leaf --
 *	Abort updates on a row leaf page with timestamps newer than the
 *	rollback timestamp.
 */
static void
__txn_abort_newer_row_leaf(
    WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t *rollback_timestamp)
{
	WT_INSERT_HEAD *insert;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	/*
	 * Review the insert list for keys before the first entry on the disk
	 * page.
	 */
	if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
		__txn_abort_newer_insert(session, insert, rollback_timestamp);

	/*
	 * Review updates that belong to keys that are on the disk image,
	 * as well as for keys inserted since the page was read from disk.
	 */
	WT_ROW_FOREACH(page, rip, i) {
		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
			__txn_abort_newer_update(
			    session, upd, rollback_timestamp);

		if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
			__txn_abort_newer_insert(
			    session, insert, rollback_timestamp);
	}
}

/*
 * __txn_abort_newer_updates --
 *	Abort updates on this page newer than the timestamp.
 */
static int
__txn_abort_newer_updates(
    WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t *rollback_timestamp)
{
	WT_PAGE *page;

	page = ref->page;
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		__txn_abort_newer_col_fix(session, page, rollback_timestamp);
		break;
	case WT_PAGE_COL_VAR:
		__txn_abort_newer_col_var(session, page, rollback_timestamp);
		break;
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		/*
		 * There is nothing to do for internal pages, since we aren't
		 * rolling back far enough to potentially include reconciled
		 * changes - and thus won't need to roll back structure
		 * changes on internal pages.
		 */
		break;
	case WT_PAGE_ROW_LEAF:
		__txn_abort_newer_row_leaf(session, page, rollback_timestamp);
		break;
	WT_ILLEGAL_VALUE(session);
	}

	return (0);
}

/*
 * __txn_rollback_to_stable_btree_walk --
 *	Called for each open handle - choose to either skip or wipe the commits
 */
static int
__txn_rollback_to_stable_btree_walk(
    WT_SESSION_IMPL *session, wt_timestamp_t *rollback_timestamp)
{
	WT_DECL_RET;
	WT_REF *ref;

	/* Walk the tree, marking commits aborted where appropriate. */
	ref = NULL;
	while ((ret = __wt_tree_walk(session, &ref,
	    WT_READ_CACHE | WT_READ_LOOKASIDE | WT_READ_NO_EVICT)) == 0 &&
	    ref != NULL) {
		if (ref->page_las != NULL &&
		    __wt_timestamp_cmp(rollback_timestamp,
		    &ref->page_las->onpage_timestamp) < 0)
			ref->page_las->invalid = true;

		/* Review deleted page saved to the ref */
		if (ref->page_del != NULL && __wt_timestamp_cmp(
		    rollback_timestamp, &ref->page_del->timestamp) < 0)
			__wt_delete_page_rollback(session, ref);

		if (!__wt_page_is_modified(ref->page))
			continue;

		WT_RET(__txn_abort_newer_updates(
		    session, ref, rollback_timestamp));
	}
	return (ret);
}

/*
 * __txn_rollback_eviction_drain --
 *	Wait for eviction to drain from a tree.
 */
static int
__txn_rollback_eviction_drain(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_UNUSED(cfg);

	WT_RET(__wt_evict_file_exclusive_on(session));
	__wt_evict_file_exclusive_off(session);
	return (0);
}

/*
 * __txn_rollback_to_stable_btree --
 *	Called for each open handle - choose to either skip or wipe the commits
 */
static int
__txn_rollback_to_stable_btree(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_DECL_TIMESTAMP(rollback_timestamp)
	WT_TXN_GLOBAL *txn_global;

	WT_UNUSED(cfg);

	btree = S2BT(session);
	conn = S2C(session);
	txn_global = &conn->txn_global;

	/*
	 * Immediately durable files don't get their commits wiped. This case
	 * mostly exists to support the semantic required for the oplog in
	 * MongoDB - updates that have been made to the oplog should not be
	 * aborted. It also wouldn't be safe to roll back updates for any
	 * table that had it's records logged, since those updates would be
	 * recovered after a crash making them inconsistent.
	 */
	if (__wt_btree_immediately_durable(session)) {
		/*
		 * Add the btree ID to the bitstring, so we can exclude any
		 * lookaside entries for this btree.
		 */
		if (btree->id >= conn->stable_rollback_maxfile)
			WT_PANIC_RET(session, EINVAL, "btree file ID %" PRIu32
			    " larger than max %" PRIu32,
			    btree->id, conn->stable_rollback_maxfile);
		__bit_set(conn->stable_rollback_bitstring, btree->id);
		return (0);
	}

	/* There is never anything to do for checkpoint handles */
	if (session->dhandle->checkpoint != NULL)
		return (0);

	/* There is nothing to do on an empty tree. */
	if (btree->root.page == NULL)
		return (0);

	/*
	 * Copy the stable timestamp, otherwise we'd need to lock it each time
	 * it's accessed. Even though the stable timestamp isn't supposed to be
	 * updated while rolling back, accessing it without a lock would
	 * violate protocol.
	 */
	WT_WITH_TIMESTAMP_READLOCK(session, &txn_global->rwlock,
	    __wt_timestamp_set(
	    &rollback_timestamp, &txn_global->stable_timestamp));

	/*
	 * Ensure the eviction server is out of the file - we don't
	 * want it messing with us. This step shouldn't be required, but
	 * it simplifies some of the reasoning about what state trees can
	 * be in.
	 */
	WT_RET(__wt_evict_file_exclusive_on(session));
	ret = __txn_rollback_to_stable_btree_walk(
	    session, &rollback_timestamp);
	__wt_evict_file_exclusive_off(session);

	return (ret);
}

/*
 * __txn_rollback_to_stable_check --
 *	Ensure the rollback request is reasonable.
 */
static int
__txn_rollback_to_stable_check(WT_SESSION_IMPL *session)
{
	WT_TXN_GLOBAL *txn_global;
	bool txn_active;

	txn_global = &S2C(session)->txn_global;
	if (!txn_global->has_stable_timestamp)
		WT_RET_MSG(session, EINVAL,
		    "rollback_to_stable requires a stable timestamp");

	/*
	 * Help the user - see if they have any active transactions. I'd
	 * like to check the transaction running flag, but that would
	 * require peeking into all open sessions, which isn't really
	 * kosher.
	 */
	WT_RET(__wt_txn_activity_check(session, &txn_active));
	if (txn_active)
		WT_RET_MSG(session, EINVAL,
		    "rollback_to_stable illegal with active transactions");

	return (0);
}
#endif

/*
 * __wt_txn_rollback_to_stable --
 *	Rollback all in-memory state related to timestamps more recent than
 *	the passed in timestamp.
 */
int
__wt_txn_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[])
{
#ifndef HAVE_TIMESTAMPS
	WT_UNUSED(cfg);

	WT_RET_MSG(session, ENOTSUP, "rollback_to_stable "
	    "requires a version of WiredTiger built with timestamp support");
#else
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * Mark that a rollback operation is in progress and wait for eviction
	 * to drain.  This is necessary because lookaside eviction uses
	 * transactions and causes the check for a quiescent system to fail.
	 *
	 * Configuring lookaside eviction off isn't atomic, safe because the
	 * flag is only otherwise set when closing down the database. Assert
	 * to avoid confusion in the future.
	 */
	WT_ASSERT(session, !F_ISSET(conn, WT_CONN_EVICTION_NO_LOOKASIDE));
	F_SET(conn, WT_CONN_EVICTION_NO_LOOKASIDE);

	WT_ERR(__wt_conn_btree_apply(session,
	    NULL, __txn_rollback_eviction_drain, NULL, cfg));

	WT_ERR(__txn_rollback_to_stable_check(session));

	F_CLR(conn, WT_CONN_EVICTION_NO_LOOKASIDE);

	/*
	 * Allocate a non-durable btree bitstring.  We increment the global
	 * value before using it, so the current value is already in use, and
	 * hence we need to add one here.
	 */
	conn->stable_rollback_maxfile = conn->next_file_id + 1;
	WT_ERR(__bit_alloc(session,
	    conn->stable_rollback_maxfile, &conn->stable_rollback_bitstring));
	WT_ERR(__wt_conn_btree_apply(session,
	    NULL, __txn_rollback_to_stable_btree, NULL, cfg));

	/*
	 * Clear any offending content from the lookaside file. This must be
	 * done after the in-memory application, since the process of walking
	 * trees in cache populates a list that is used to check which
	 * lookaside records should be removed.
	 */
	WT_ERR(__txn_rollback_to_stable_lookaside_fixup(session));

err:	F_CLR(conn, WT_CONN_EVICTION_NO_LOOKASIDE);
	__wt_free(session, conn->stable_rollback_bitstring);
	return (ret);
#endif
}
