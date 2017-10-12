/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
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
	int64_t v;

	conn = S2C(session);

	/*
	 * Lookaside table statistics are copied from the underlying lookaside
	 * table data-source statistics. If there's no lookaside table, values
	 * remain 0.
	 */
	if (!F_ISSET(conn, WT_CONN_LAS_OPEN))
		return;

	/*
	 * We have a cursor, and we need the underlying data handle; we can get
	 * to it by way of the underlying btree handle, but it's a little ugly.
	 */
	cstats = conn->stats;
	dstats = ((WT_CURSOR_BTREE *)
	    conn->las_session->las_cursor)->btree->dhandle->stats;

	v = WT_STAT_READ(dstats, cursor_insert);
	WT_STAT_SET(session, cstats, cache_lookaside_insert, v);
	v = WT_STAT_READ(dstats, cursor_remove);
	WT_STAT_SET(session, cstats, cache_lookaside_remove, v);
	/*
	 * If we're clearing stats we need to clear the cursor values we just
	 * read.  This does not clear the rest of the statistics in the
	 * lookaside data source stat cursor, but we own that namespace so we
	 * don't have to worry about users seeing inconsistent data source
	 * information.
	 */
	if (FLD_ISSET(conn->stat_flags, WT_STAT_CLEAR)) {
		WT_STAT_SET(session, dstats, cursor_insert, 0);
		WT_STAT_SET(session, dstats, cursor_remove, 0);
	}
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
	uint32_t session_flags;
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL };

	conn = S2C(session);

	/* Read-only and in-memory configurations don't need the LAS table. */
	if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
		return (0);

	/*
	 * Done at startup: we cannot do it on demand because we require the
	 * schema lock to create and drop the table, and it may not always be
	 * available.
	 *
	 * Discard any previous incarnation of the table.
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_drop(session, WT_LAS_URI, drop_cfg));
	WT_RET(ret);

	/* Re-create the table. */
	WT_RET(__wt_session_create(session, WT_LAS_URI, WT_LAS_FORMAT));

	/*
	 * Flag that the lookaside table has been created (before creating the
	 * connection's lookaside table session, it checks before creating a
	 * lookaside table cursor.
	 */
	F_SET(conn, WT_CONN_LAS_OPEN);

	/*
	 * Open a shared internal session used to access the lookaside table.
	 * This session should never be tapped for eviction.
	 */
	session_flags = WT_SESSION_LOOKASIDE_CURSOR | WT_SESSION_NO_EVICTION;
	WT_ERR(__wt_open_internal_session(
	    conn, "lookaside table", true, session_flags, &conn->las_session));

	return (0);

err:	F_CLR(conn, WT_CONN_LAS_OPEN);
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

	conn->las_session = NULL;

	return (ret);
}

/*
 * __wt_las_cursor_open --
 *	Open a new lookaside table cursor.
 */
int
__wt_las_cursor_open(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL };

	WT_WITHOUT_DHANDLE(session, ret = __wt_open_cursor(
	    session, WT_LAS_URI, NULL, open_cursor_cfg, cursorp));
	WT_RET(ret);

	/*
	 * Retrieve the btree from the cursor, rather than the session because
	 * we don't always switch the LAS handle in to the session before
	 * entering this function.
	 */
	btree = ((WT_CURSOR_BTREE *)(*cursorp))->btree;

	/* Track the lookaside file ID. */
	if (S2C(session)->las_fileid == 0)
		S2C(session)->las_fileid = btree->id;

	/*
	 * Set special flags for the lookaside table: the lookaside flag (used,
	 * for example, to avoid writing records during reconciliation), also
	 * turn off checkpoints and logging.
	 *
	 * Test flags before setting them so updates can't race in subsequent
	 * opens (the first update is safe because it's single-threaded from
	 * wiredtiger_open).
	 */
	if (!F_ISSET(btree, WT_BTREE_LOOKASIDE))
		F_SET(btree, WT_BTREE_LOOKASIDE);
	if (!F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
		F_SET(btree, WT_BTREE_NO_CHECKPOINT);
	if (!F_ISSET(btree, WT_BTREE_NO_LOGGING))
		F_SET(btree, WT_BTREE_NO_LOGGING);

	return (0);
}

/*
 * __wt_las_cursor --
 *	Return a lookaside cursor.
 */
void
__wt_las_cursor(
    WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t *session_flags)
{
	WT_CONNECTION_IMPL *conn;

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
	    F_MASK(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_EVICTION);

	conn = S2C(session);

	/*
	 * Some threads have their own lookaside table cursors, else lock the
	 * shared lookaside cursor.
	 */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
		*cursorp = session->las_cursor;
	else {
		__wt_spin_lock(session, &conn->las_lock);
		*cursorp = conn->las_session->las_cursor;
	}

	/* Turn caching and eviction off. */
	F_SET(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_EVICTION);
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
	 * Some threads have their own lookaside table cursors, else unlock the
	 * shared lookaside cursor.
	 */
	if (!F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
		__wt_spin_unlock(session, &conn->las_lock);

	return (ret);
}

/*
 * __wt_las_remove_block --
 *	Remove all records matching a key prefix from the lookaside store.
 */
int
__wt_las_remove_block(WT_SESSION_IMPL *session,
    WT_CURSOR *cursor, uint32_t btree_id, uint64_t pageid)
{
	WT_DECL_RET;
	WT_ITEM las_key;
	uint64_t las_counter, las_pageid, remove_cnt;
	uint32_t las_id;
	int exact;

	remove_cnt = 0;

	/*
	 * Search for the block's unique prefix and step through all matching
	 * records, removing them.
	 */
	las_key.size = 0;
	cursor->set_key(cursor, btree_id, pageid, (uint64_t)0, &las_key);
	if ((ret = cursor->search_near(cursor, &exact)) == 0 && exact < 0)
		ret = cursor->next(cursor);
	for (; ret == 0; ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor,
		    &las_id, &las_pageid, &las_counter, &las_key));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.  Note that
		 * page ID zero is special: it is a wild card indicating that
		 * all pages in the tree should be removed.
		 */
		 if (las_id != btree_id ||
		    (pageid != 0 && las_pageid != pageid))
			break;

		WT_ERR(cursor->remove(cursor));
		++remove_cnt;
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_STAT_CONN_DECRV(session, cache_lookaside_entries, remove_cnt);
	return (ret);
}
