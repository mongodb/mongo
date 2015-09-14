/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_las_stats_update --
 *	Update the lookaside table statistics for return to the application.
 */
void
__wt_las_stats_update(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS **cstats;
	WT_DSRC_STATS **dstats;

	conn = S2C(session);

	/*
	 * Lookaside table statistics are copied from the underlying lookaside
	 * table data-source statistics. If there's no lookaside table, values
	 * remain 0. In the current system, there's always a lookaside table,
	 * but there's no reason not to be cautious.
	 */
	if (conn->las_cursor == NULL)
		return;

	/*
	 * We have a cursor, and we need the underlying data handle; we can get
	 * to it by way of the underlying btree handle, but it's a little ugly.
	 */
	cstats = conn->stats;
	dstats = ((WT_CURSOR_BTREE *)conn->las_cursor)->btree->dhandle->stats;

	WT_STAT_SET(session, cstats,
	    cache_lookaside_insert, WT_STAT_READ(dstats, cursor_insert));
	WT_STAT_SET(session, cstats,
	    cache_lookaside_remove, WT_STAT_READ(dstats, cursor_remove));
}

/*
 * __las_cursor_create --
 *	Open a new lookaside table cursor.
 */
static int
__las_cursor_create(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
	WT_BTREE *btree;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL };

	WT_RET(__wt_open_cursor(
	    session, WT_LAS_URI, NULL, open_cursor_cfg, cursorp));

	/*
	 * Set special flags for the lookaside table: the lookaside flag (used,
	 * for example, to avoid writing records during reconciliation), also
	 * turn off checkpoints and logging.
	 *
	 * Test flags before setting them so updates can't race in subsequent
	 * opens (the first update is safe because it's single-threaded from
	 * wiredtiger_open).
	 */
	btree = S2BT(session);
	if (!F_ISSET(btree, WT_BTREE_LOOKASIDE))
		F_SET(btree, WT_BTREE_LOOKASIDE);
	if (!F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
		F_SET(btree, WT_BTREE_NO_CHECKPOINT);
	if (!F_ISSET(btree, WT_BTREE_NO_LOGGING))
		F_SET(btree, WT_BTREE_NO_LOGGING);

	return (0);
}

/*
 * __wt_las_create --
 *	Initialize the database's lookaside store.
 */
int
__wt_las_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL };

	conn = S2C(session);

	/*
	 * Done at startup: we cannot do it on demand because we require the
	 * schema lock to create and drop the file, and it may not always be
	 * available.
	 *
	 * Open an internal session, used for the shared lookaside cursor.
	 *
	 * Sessions associated with a lookaside cursor should never be tapped
	 * for eviction.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "lookaside table", 1, 1, &conn->las_session));
	session = conn->las_session;
	F_SET(session, WT_SESSION_LOOKASIDE_CURSOR | WT_SESSION_NO_EVICTION);

	/* Discard any previous incarnation of the file. */
	WT_RET(__wt_session_drop(session, WT_LAS_URI, drop_cfg));

	/* Re-create the file. */
	WT_RET(__wt_session_create(session, WT_LAS_URI, WT_LAS_FORMAT));

	/* Open the shared cursor. */
	WT_WITHOUT_DHANDLE(session,
	    ret = __las_cursor_create(session, &conn->las_cursor));

	return (ret);
}

/*
 * __wt_las_destroy --
 *	Destroy the database's lookaside store.
 */
int
__wt_las_destroy(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	if (conn->las_session == NULL)
		return (0);

	wt_session = &conn->las_session->iface;
	ret = wt_session->close(wt_session, NULL);

	conn->las_cursor = NULL;
	conn->las_session = NULL;

	return (ret);
}

/*
 * __wt_las_set_written --
 *	Flag that the lookaside table has been written.
 */
void
__wt_las_set_written(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	if (!conn->las_written) {
		conn->las_written = true;

		/*
		 * Push the flag: unnecessary, but from now page reads must deal
		 * with lookaside table records, and we only do the write once.
		 */
		WT_FULL_BARRIER();
	}
}

/*
 * __wt_las_is_written --
 *	Return if the lookaside table has been written.
 */
bool
__wt_las_is_written(WT_SESSION_IMPL *session)
{
	return (S2C(session)->las_written);
}

/*
 * __wt_las_cursor --
 *	Return a lookaside cursor.
 */
int
__wt_las_cursor(
    WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t *session_flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	*cursorp = NULL;

	/*
	 * We don't want to get tapped for eviction after we start using the
	 * lookaside cursor; save a copy of the current eviction state, we'll
	 * turn eviction off before we return.
	 *
	 * Don't cache lookaside table pages, we're here because of eviction
	 * problems and there's no reason to believe lookaside pages will be
	 * useful more than once.
	 */
	*session_flags =
	    F_ISSET(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_EVICTION);

	conn = S2C(session);

	/* Eviction and sweep threads have their own lookaside table cursors. */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR)) {
		if (session->las_cursor == NULL) {
			WT_WITHOUT_DHANDLE(session, ret =
			    __las_cursor_create(session, &session->las_cursor));
			WT_RET(ret);
		}

		*cursorp = session->las_cursor;
	} else {
		/* Lock the shared lookaside cursor. */
		__wt_spin_lock(session, &conn->las_lock);

		*cursorp = conn->las_cursor;
	}

	/* Turn caching and eviction off. */
	F_SET(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_EVICTION);

	return (0);
}

/*
 * __wt_las_cursor_close --
 *	Discard a lookaside cursor.
 */
int
__wt_las_cursor_close(
	WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t session_flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	conn = S2C(session);

	if ((cursor = *cursorp) == NULL)
		return (0);
	*cursorp = NULL;

	/* Reset the cursor. */
	ret = cursor->reset(cursor);

	/*
	 * We turned off caching and eviction while the lookaside cursor was in
	 * use, restore the session's flags.
	 */
	F_CLR(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_EVICTION);
	F_SET(session, session_flags);

	/*
	 * Eviction and sweep threads have their own lookaside table cursors;
	 * else, unlock the shared lookaside cursor.
	 */
	if (!F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
		__wt_spin_unlock(session, &conn->las_lock);

	return (ret);
}

/*
 * __wt_las_sweep --
 *	Sweep the lookaside table.
 */
int
__wt_las_sweep(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_ITEM(las_addr);
	WT_DECL_ITEM(las_key);
	WT_DECL_RET;
	WT_ITEM *key;
	uint64_t cnt, las_counter, las_txnid;
	uint32_t las_id, session_flags;
	int notused;

	conn = S2C(session);
	cursor = NULL;
	key = &conn->las_sweep_key;
	session_flags = 0;		/* [-Werror=maybe-uninitialized] */

	WT_ERR(__wt_scr_alloc(session, 0, &las_addr));
	WT_ERR(__wt_scr_alloc(session, 0, &las_key));

	WT_ERR(__wt_las_cursor(session, &cursor, &session_flags));

	/*
	 * If we're not starting a new sweep, position the cursor using the key
	 * from the last call (we don't care if we're before or after the key,
	 * just roughly in the same spot is fine).
	 */
	if (conn->las_sweep_call != 0 && key->data != NULL) {
		__wt_cursor_set_raw_key(cursor, key);
		if ((ret = cursor->search_near(cursor, &notused)) != 0)
			goto srch_notfound;
	}

	/*
	 * The sweep server wakes up every 10 seconds (by default), it's a slow
	 * moving thread. Try to review the entire lookaside table once every 5
	 * minutes, or every 30 calls.
	 *
	 * The reason is because the lookaside table exists because we're seeing
	 * cache/eviction pressure (it allows us to trade performance and disk
	 * space for cache space), and it's likely lookaside blocks are being
	 * evicted, and reading them back in doesn't help things. A trickier,
	 * but possibly better, alternative might be to review all lookaside
	 * blocks in the cache in order to get rid of them, and slowly review
	 * lookaside blocks that have already been evicted.
	 *
	 * We can't know for sure how many records are in the lookaside table,
	 * the cursor insert and remove statistics aren't updated atomically.
	 * Start with reviewing 100 rows, and if it takes more than the target
	 * number of calls to finish, increase the number of rows checked on
	 * each call; if it takes less than the target calls to finish, then
	 * decrease the number of rows reviewed on each call (but never less
	 * than 100).
	 */
#define	WT_SWEEP_LOOKASIDE_MIN_CNT	100
#define	WT_SWEEP_LOOKASIDE_PASS_TARGET	 30
	++conn->las_sweep_call;
	if ((cnt = conn->las_sweep_cnt) < WT_SWEEP_LOOKASIDE_MIN_CNT)
		cnt = conn->las_sweep_cnt = WT_SWEEP_LOOKASIDE_MIN_CNT;

	/* Walk the file. */
	for (; cnt > 0 && (ret = cursor->next(cursor)) == 0; --cnt) {
		/*
		 * If the loop terminates after completing a work unit, we will
		 * continue the table sweep next time. Get a local copy of the
		 * sweep key, we're going to reset the cursor; do so before
		 * calling cursor.remove, cursor.remove can discard our hazard
		 * pointer and the page could be evicted from underneath us.
		 */
		if (cnt == 1) {
			WT_ERR(__wt_cursor_get_raw_key(cursor, key));
			if (!WT_DATA_IN_ITEM(key))
				WT_ERR(__wt_buf_set(
				    session, key, key->data, key->size));
		}

		WT_ERR(cursor->get_key(cursor,
		    &las_id, las_addr, &las_counter, &las_txnid, las_key));

		/*
		 * If the on-page record transaction ID associated with the
		 * record is globally visible, the record can be discarded.
		 *
		 * Cursor opened overwrite=true: won't return WT_NOTFOUND should
		 * another thread remove the record before we do, and the cursor
		 * remains positioned in that case.
		 */
		if (__wt_txn_visible_all(session, las_txnid))
			WT_ERR(cursor->remove(cursor));
	}

	/*
	 * When reaching the lookaside table end or the target number of calls,
	 * adjust the row count. Decrease/increase the row count depending on
	 * if the number of calls is less/more than the target.
	 */
	if (ret == WT_NOTFOUND ||
	    conn->las_sweep_call > WT_SWEEP_LOOKASIDE_PASS_TARGET) {
		if (conn->las_sweep_call < WT_SWEEP_LOOKASIDE_PASS_TARGET &&
		    conn->las_sweep_cnt > WT_SWEEP_LOOKASIDE_MIN_CNT)
			conn->las_sweep_cnt -= WT_SWEEP_LOOKASIDE_MIN_CNT;
		if (conn->las_sweep_call > WT_SWEEP_LOOKASIDE_PASS_TARGET)
			conn->las_sweep_cnt += WT_SWEEP_LOOKASIDE_MIN_CNT;
	}

srch_notfound:
	if (ret == WT_NOTFOUND)
		conn->las_sweep_call = 0;

	WT_ERR_NOTFOUND_OK(ret);

	if (0) {
err:		__wt_buf_free(session, key);
	}

	WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));

	__wt_scr_free(session, &las_addr);
	__wt_scr_free(session, &las_key);

	return (ret);
}
