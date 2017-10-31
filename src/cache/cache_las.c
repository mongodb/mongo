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
	if (!F_ISSET(conn, WT_CONN_LOOKASIDE_OPEN))
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
	 * Open a shared internal session and cursor used for the lookaside
	 * table. This session should never be tapped for eviction.
	 */
	session_flags = WT_SESSION_NO_EVICTION;
	WT_RET(__wt_open_internal_session(
	    conn, "lookaside table", true, session_flags, &conn->las_session));
	WT_RET(__wt_las_cursor_open(conn->las_session));

	/* The statistics server is already running, make sure we don't race. */
	WT_WRITE_BARRIER();
	F_SET(conn, WT_CONN_LOOKASIDE_OPEN);

	return (0);
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

	F_CLR(conn, WT_CONN_LOOKASIDE_OPEN);
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
__wt_las_cursor_open(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL };

	WT_WITHOUT_DHANDLE(session, ret = __wt_open_cursor(
	    session, WT_LAS_URI, NULL, open_cursor_cfg, &cursor));
	WT_RET(ret);

	/*
	 * Retrieve the btree from the cursor, rather than the session because
	 * we don't always switch the LAS handle in to the session before
	 * entering this function.
	 */
	btree = ((WT_CURSOR_BTREE *)cursor)->btree;

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

	session->las_cursor = cursor;
	F_SET(session, WT_SESSION_LOOKASIDE_CURSOR);

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
 * __las_insert_block_verbose --
 *	Display a verbose message once per checkpoint with details about the
 *	cache state when performing a lookaside table write.
 */
static void
__las_insert_block_verbose(
    WT_SESSION_IMPL *session, uint32_t btree_id, uint64_t las_pageid)
{
#ifdef HAVE_VERBOSE
	WT_CONNECTION_IMPL *conn;
	uint64_t ckpt_gen_current, ckpt_gen_last;
	uint32_t pct_dirty, pct_full;

	if (!WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE))
		return;

	conn = S2C(session);
	ckpt_gen_current = __wt_gen(session, WT_GEN_CHECKPOINT);
	ckpt_gen_last = conn->las_verb_gen_write;

	/*
	 * This message is throttled to one per checkpoint. To do this we
	 * track the generation of the last checkpoint for which the message
	 * was printed and check against the current checkpoint generation.
	 */
	if (ckpt_gen_current > ckpt_gen_last) {
		/*
		 * Attempt to atomically replace the last checkpoint generation
		 * for which this message was printed. If the atomic swap fails
		 * we have raced and the winning thread will print the message.
		 */
		if (__wt_atomic_casv64(&conn->las_verb_gen_write,
		    ckpt_gen_last, ckpt_gen_current)) {
			(void)__wt_eviction_clean_needed(session, &pct_full);
			(void)__wt_eviction_dirty_needed(session, &pct_dirty);

			__wt_verbose(session, WT_VERB_LOOKASIDE,
			    "Page reconciliation triggered lookaside write"
			    "file ID %" PRIu32 ", page ID %" PRIu64 ". "
			    "Entries now in lookaside file: %" PRId64 ", "
			    "cache dirty: %" PRIu32 "%% , "
			    "cache use: %" PRIu32 "%%",
			    btree_id, las_pageid,
			    WT_STAT_READ(conn->stats, cache_lookaside_entries),
			    pct_dirty, pct_full);
		}
	}
#else
	WT_UNUSED(session);
	WT_UNUSED(btree_id);
	WT_UNUSED(las_pageid);
#endif
}

/*
 * __wt_las_insert_block --
 *	Copy one set of saved updates into the database's lookaside buffer.
 */
int
__wt_las_insert_block(WT_SESSION_IMPL *session,
    WT_PAGE *page, WT_CURSOR *cursor, WT_MULTI *multi, WT_ITEM *key)
{
	WT_ITEM las_timestamp, las_value;
	WT_SAVE_UPD *list;
	WT_UPDATE *upd;
	uint64_t insert_cnt, las_counter, las_pageid;
	uint32_t btree_id, i, slot;
	uint8_t *p;

	WT_CLEAR(las_timestamp);
	WT_CLEAR(las_value);
	insert_cnt = 0;

	btree_id = S2BT(session)->id;
	las_pageid = multi->page_las.las_pageid =
	    __wt_atomic_add64(&S2BT(session)->las_pageid, 1);

	/*
	 * Make sure there are no leftover entries (e.g., from a handle
	 * reopen).
	 */
	WT_RET(__wt_las_remove_block(session, cursor, btree_id, las_pageid));

	/* Enter each update in the boundary's list into the lookaside store. */
	for (las_counter = 0, i = 0,
	    list = multi->supd; i < multi->supd_entries; ++i, ++list) {
		/* Lookaside table key component: source key. */
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			p = key->mem;
			WT_RET(
			    __wt_vpack_uint(&p, 0, WT_INSERT_RECNO(list->ins)));
			key->size = WT_PTRDIFF(p, key->data);
			break;
		case WT_PAGE_ROW_LEAF:
			if (list->ins == NULL)
				WT_RET(__wt_row_leaf_key(
				    session, page, list->ripcip, key, false));
			else {
				key->data = WT_INSERT_KEY(list->ins);
				key->size = WT_INSERT_KEY_SIZE(list->ins);
			}
			break;
		WT_ILLEGAL_VALUE(session);
		}

		/*
		 * Lookaside table value component: update reference. Updates
		 * come from the row-store insert list (an inserted item), or
		 * update array (an update to an original on-page item), or from
		 * a column-store insert list (column-store format has no update
		 * array, the insert list contains both inserted items and
		 * updates to original on-page items). When rolling forward a
		 * modify update from an original on-page item, we need an
		 * on-page slot so we can find the original on-page item. When
		 * rolling forward from an inserted item, no on-page slot is
		 * possible.
		 */
		slot = UINT32_MAX;			/* Impossible slot */
		if (list->ripcip != NULL)
			slot = page->type == WT_PAGE_ROW_LEAF ?
			    WT_ROW_SLOT(page, list->ripcip) :
			    WT_COL_SLOT(page, list->ripcip);
		upd = list->ins == NULL ?
		    page->modify->mod_row_update[slot] : list->ins->upd;

		/*
		 * Walk the list of updates, storing each key/value pair into
		 * the lookaside table. Skip aborted items (there's no point
		 * to restoring them), and assert we never see a reserved item.
		 */
		do {
			if (upd->txnid == WT_TXN_ABORTED)
				continue;

			switch (upd->type) {
			case WT_UPDATE_DELETED:
				las_value.size = 0;
				break;
			case WT_UPDATE_MODIFIED:
			case WT_UPDATE_STANDARD:
				las_value.data = upd->data;
				las_value.size = upd->size;
				break;
			case WT_UPDATE_RESERVED:
				WT_ASSERT(session,
				    upd->type != WT_UPDATE_RESERVED);
				continue;
			}

			cursor->set_key(cursor,
			    btree_id, las_pageid, ++las_counter, key);

#ifdef HAVE_TIMESTAMPS
			las_timestamp.data = &upd->timestamp;
			las_timestamp.size = WT_TIMESTAMP_SIZE;
#endif
			cursor->set_value(cursor,
			    upd->txnid, &las_timestamp, upd->type, &las_value);

			WT_RET(cursor->insert(cursor));
			++insert_cnt;
		} while ((upd = upd->next) != NULL);
	}

	__wt_free(session, multi->supd);
	multi->supd_entries = 0;

	if (insert_cnt > 0) {
		WT_STAT_CONN_INCRV(
		    session, cache_lookaside_entries, insert_cnt);
		__las_insert_block_verbose(session, btree_id, las_pageid);
	}
	return (0);
}

/*
 * __wt_las_cursor_position --
 *	Position a lookaside cursor at the beginning of a block.
 *
 *	There may be no block of lookaside entries if they have been removed by
 *	WT_CONNECTION::rollback_to_stable.
 */
int
__wt_las_cursor_position(WT_CURSOR *cursor, uint32_t btree_id, uint64_t pageid)
{
	WT_ITEM las_key;
	uint64_t las_counter, las_pageid;
	uint32_t las_id;
	int exact;

	/*
	 * Because of the special visibility rules for lookaside, a new block
	 * can appear in between our search and the block of interest.  Keep
	 * trying until we find it.
	 */
	for (;;) {
		WT_CLEAR(las_key);
		cursor->set_key(cursor,
		    btree_id, pageid, (uint64_t)0, &las_key);
		WT_RET(cursor->search_near(cursor, &exact));
		if (exact < 0) {
			WT_RET(cursor->next(cursor));

			/*
			 * Because of the special visibility rules for
			 * lookaside, a new block can appear in between our
			 * search and the block of interest.  Keep trying while
			 * we have a key lower that we expect.
			 *
			 * There may be no block of lookaside entries if they
			 * have been removed by
			 * WT_CONNECTION::rollback_to_stable.
			 */
			WT_RET(cursor->get_key(cursor,
			    &las_id, &las_pageid, &las_counter, &las_key));
			if (las_id < btree_id || (las_id == btree_id &&
			    pageid != 0 && las_pageid < pageid))
				continue;
		}

		return (0);
	}

	/* NOTREACHED */
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
	uint32_t las_id, session_flags;
	bool local_cursor;

	remove_cnt = 0;
	session_flags = 0;		/* [-Wconditional-uninitialized] */

	local_cursor = false;
	if (cursor == NULL) {
		__wt_las_cursor(session, &cursor, &session_flags);
		local_cursor = true;
	}

	/*
	 * Search for the block's unique prefix and step through all matching
	 * records, removing them.
	 */
	ret = __wt_las_cursor_position(cursor, btree_id, pageid);
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

err:	if (local_cursor)
		WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));

	WT_STAT_CONN_DECRV(session, cache_lookaside_entries, remove_cnt);
	return (ret);
}
