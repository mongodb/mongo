/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * When an operation is accessing the lookaside table, it should ignore the
 * cache size (since the cache is already full), any pages it reads should be
 * evicted before application data, and the operation can't reenter
 * reconciliation.
 */
#define	WT_LAS_SESSION_FLAGS						\
	(WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_READ_WONT_NEED |	\
	WT_SESSION_NO_RECONCILE)

/*
 * __wt_las_nonempty --
 *	Return when there are entries in the lookaside table.
 */
bool
__wt_las_nonempty(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;

	return (cache->las_entry_count > 0);
}

/*
 * __wt_las_stats_update --
 *	Update the lookaside table statistics for return to the application.
 */
void
__wt_las_stats_update(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS **cstats;
	WT_DSRC_STATS **dstats;
	int64_t v;

	conn = S2C(session);
	cache = conn->cache;

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
	    cache->las_session[0]->las_cursor)->btree->dhandle->stats;

	v = WT_STAT_READ(dstats, cursor_update);
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
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int i;
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL };

	conn = S2C(session);
	cache = conn->cache;

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
	WT_RET(__wt_session_create(session, WT_LAS_URI, WT_LAS_CONFIG));

	/*
	 * Open a shared internal session and cursor used for the lookaside
	 * table. This session should never perform reconciliation.
	 */
	for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
		WT_RET(__wt_open_internal_session(conn, "lookaside table",
		    true, WT_LAS_SESSION_FLAGS, &cache->las_session[i]));
		WT_RET(__wt_las_cursor_open(cache->las_session[i]));
	}

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
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	int i;

	conn = S2C(session);
	cache = conn->cache;

	F_CLR(conn, WT_CONN_LOOKASIDE_OPEN);
	if (cache == NULL)
		return (0);

	for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
		if (cache->las_session[i] == NULL)
			continue;

		wt_session = &cache->las_session[i]->iface;
		WT_TRET(wt_session->close(wt_session, NULL));
		cache->las_session[i] = NULL;
	}

	__wt_buf_free(session, &cache->las_sweep_key);
	__wt_free(session, cache->las_dropped);
	__wt_free(session, cache->las_sweep_dropmap);

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
	if (S2C(session)->cache->las_fileid == 0)
		S2C(session)->cache->las_fileid = btree->id;

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
	WT_CACHE *cache;
	int i;

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
	*session_flags = F_MASK(session, WT_LAS_SESSION_FLAGS);

	cache = S2C(session)->cache;

	/*
	 * Some threads have their own lookaside table cursors, else lock the
	 * shared lookaside cursor.
	 */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
		*cursorp = session->las_cursor;
	else {
		for (;;) {
			__wt_spin_lock(session, &cache->las_lock);
			for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
				if (!cache->las_session_inuse[i]) {
					*cursorp =
					    cache->las_session[i]->las_cursor;
					cache->las_session_inuse[i] = true;
					break;
				}
			}
			__wt_spin_unlock(session, &cache->las_lock);
			if (*cursorp != NULL)
				break;
			/*
			 * If all the lookaside sessions are busy, stall.
			 *
			 * XXX better as a condition variable.
			 */
			__wt_sleep(0, 1000);
		}
	}

	/* Configure session to access the lookaside table. */
	F_SET(session, WT_LAS_SESSION_FLAGS);
}

/*
 * __wt_las_cursor_close --
 *	Discard a lookaside cursor.
 */
int
__wt_las_cursor_close(
    WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t session_flags)
{
	WT_CACHE *cache;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int i;

	cache = S2C(session)->cache;

	if ((cursor = *cursorp) == NULL)
		return (0);
	*cursorp = NULL;

	/* Reset the cursor. */
	ret = cursor->reset(cursor);

	/*
	 * We turned off caching and eviction while the lookaside cursor was in
	 * use, restore the session's flags.
	 */
	F_CLR(session, WT_LAS_SESSION_FLAGS);
	F_SET(session, session_flags);

	/*
	 * Some threads have their own lookaside table cursors, else unlock the
	 * shared lookaside cursor.
	 */
	if (!F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR)) {
		__wt_spin_lock(session, &cache->las_lock);
		for (i = 0; i < WT_LAS_NUM_SESSIONS; i++)
			if (cursor->session == &cache->las_session[i]->iface) {
				cache->las_session_inuse[i] = false;
				break;
			}
		__wt_spin_unlock(session, &cache->las_lock);
		WT_ASSERT(session, i != WT_LAS_NUM_SESSIONS);
	}

	return (ret);
}

/*
 * __las_insert_block_verbose --
 *	Display a verbose message once per checkpoint with details about the
 *	cache state when performing a lookaside table write.
 */
static int
__las_insert_block_verbose(WT_SESSION_IMPL *session, WT_MULTI *multi)
{
#ifdef HAVE_VERBOSE
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
#ifdef HAVE_TIMESTAMPS
	char hex_timestamp[2 * WT_TIMESTAMP_SIZE + 1];
#else
	char hex_timestamp[9]; /* Enough for disabled string */
#endif
	uint64_t ckpt_gen_current, ckpt_gen_last;
	uint32_t btree_id, pct_dirty, pct_full;

	btree_id = S2BT(session)->id;

	if (!WT_VERBOSE_ISSET(session,
	    WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY))
		return (0);

	conn = S2C(session);
	cache = conn->cache;
	ckpt_gen_current = __wt_gen(session, WT_GEN_CHECKPOINT);
	ckpt_gen_last = cache->las_verb_gen_write;

	/*
	 * Print a message if verbose lookaside, or once per checkpoint if
	 * only reporting activity. Avoid an expensive atomic operation as
	 * often as possible when the message rate is limited.
	 */
	if (WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE) ||
	    (ckpt_gen_current > ckpt_gen_last &&
	    __wt_atomic_casv64(&cache->las_verb_gen_write,
	    ckpt_gen_last, ckpt_gen_current))) {
		(void)__wt_eviction_clean_needed(session, &pct_full);
		(void)__wt_eviction_dirty_needed(session, &pct_dirty);

#ifdef HAVE_TIMESTAMPS
		WT_RET(__wt_timestamp_to_hex_string(
		    session, hex_timestamp, &multi->page_las.min_timestamp));
#else
		WT_RET(__wt_snprintf(
		    hex_timestamp, sizeof(hex_timestamp), "disabled"));
#endif
		__wt_verbose(session,
		    WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY,
		    "Page reconciliation triggered lookaside write "
		    "file ID %" PRIu32 ", page ID %" PRIu64 ". "
		    "Max txn ID %" PRIu64 ", min timestamp %s, skewed %s. "
		    "Entries now in lookaside file: %" PRId64 ", "
		    "cache dirty: %" PRIu32 "%% , "
		    "cache use: %" PRIu32 "%%",
		    btree_id, multi->page_las.las_pageid,
		    multi->page_las.las_max_txn,
		    hex_timestamp,
		    multi->page_las.las_skew_newest? "newest" : "oldest",
		    WT_STAT_READ(conn->stats, cache_lookaside_entries),
		    pct_dirty, pct_full);
	}

	/* Never skip updating the tracked generation */
	if (WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE))
		cache->las_verb_gen_write = ckpt_gen_current;
#else
	WT_UNUSED(session);
	WT_UNUSED(multi);
#endif
	return (0);
}

/*
 * __wt_las_insert_block --
 *	Copy one set of saved updates into the database's lookaside buffer.
 */
int
__wt_las_insert_block(WT_SESSION_IMPL *session, WT_CURSOR *cursor,
    WT_PAGE *page, WT_MULTI *multi, WT_ITEM *key)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM las_timestamp, las_value;
	WT_SAVE_UPD *list;
	WT_SESSION_IMPL *las_session;
	WT_UPDATE *upd;
	uint64_t insert_cnt, las_counter, las_pageid;
	uint32_t btree_id, i, slot;
	uint8_t *p;

	WT_CLEAR(las_timestamp);
	WT_CLEAR(las_value);
	insert_cnt = 0;

	btree = S2BT(session);
	btree_id = btree->id;
	las_pageid = multi->page_las.las_pageid =
	    __wt_atomic_add64(&S2C(session)->cache->las_pageid, 1);

	if (!btree->lookaside_entries)
		btree->lookaside_entries = true;

	/* Wrap all the updates in a transaction. */
	las_session = (WT_SESSION_IMPL *)cursor->session;
	WT_RET(__wt_txn_begin(las_session, NULL));
	las_session->txn.isolation = WT_TXN_ISO_READ_UNCOMMITTED;

	/*
	 * Make sure there are no leftover entries (e.g., from a handle
	 * reopen).
	 */
	WT_ERR(__wt_las_remove_block(session, cursor, btree_id, las_pageid));

	/* Enter each update in the boundary's list into the lookaside store. */
	for (las_counter = 0, i = 0,
	    list = multi->supd; i < multi->supd_entries; ++i, ++list) {
		/* Lookaside table key component: source key. */
		switch (page->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_VAR:
			p = key->mem;
			WT_ERR(
			    __wt_vpack_uint(&p, 0, WT_INSERT_RECNO(list->ins)));
			key->size = WT_PTRDIFF(p, key->data);
			break;
		case WT_PAGE_ROW_LEAF:
			if (list->ins == NULL)
				WT_ERR(__wt_row_leaf_key(
				    session, page, list->ripcip, key, false));
			else {
				key->data = WT_INSERT_KEY(list->ins);
				key->size = WT_INSERT_KEY_SIZE(list->ins);
			}
			break;
		WT_ILLEGAL_VALUE_ERR(session);
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
			    las_pageid, btree_id, ++las_counter, key);

#ifdef HAVE_TIMESTAMPS
			las_timestamp.data = &upd->timestamp;
			las_timestamp.size = WT_TIMESTAMP_SIZE;
#endif
			cursor->set_value(cursor,
			    upd->txnid, &las_timestamp, upd->type, &las_value);

			/*
			 * Using update looks a little strange because the keys
			 * are guaranteed to not exist, but since we're
			 * appending, we want the cursor to stay positioned in
			 * between inserts.
			 */
			WT_ERR(cursor->update(cursor));
			++insert_cnt;
		} while ((upd = upd->next) != NULL);
	}

	if (insert_cnt > 0) {
		WT_STAT_CONN_INCRV(
		    session, cache_lookaside_entries, insert_cnt);
		__wt_atomic_add64(
		    &S2C(session)->cache->las_entry_count, insert_cnt);
		WT_ERR(__las_insert_block_verbose(session, multi));
	}

err:	/* Resolve the transaction. */
	if (ret == 0)
		ret = __wt_txn_commit(las_session, NULL);
	else
		WT_TRET(__wt_txn_rollback(las_session, NULL));
	__wt_free(session, multi->supd);
	multi->supd_entries = 0;
	return (ret);
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
	 * When scanning for all pages, start at the beginning of the lookaside
	 * table.
	 */
	if (pageid == 0) {
		WT_RET(cursor->reset(cursor));
		return (cursor->next(cursor));
	}

	/*
	 * Because of the special visibility rules for lookaside, a new block
	 * can appear in between our search and the block of interest.  Keep
	 * trying until we find it.
	 */
	for (;;) {
		WT_CLEAR(las_key);
		cursor->set_key(cursor,
		    pageid, btree_id, (uint64_t)0, &las_key);
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
			    &las_pageid, &las_id, &las_counter, &las_key));
			if (las_pageid < pageid || (las_pageid == pageid &&
			    las_id < btree_id))
				continue;
		}

		return (0);
	}

	/* NOTREACHED */
}

/*
 * __wt_las_remove_block --
 *	Remove all records for a given page from the lookaside store.
 */
int
__wt_las_remove_block(WT_SESSION_IMPL *session,
    WT_CURSOR *cursor, uint32_t btree_id, uint64_t pageid)
{
	WT_DECL_RET;
	WT_ITEM las_key;
	WT_SESSION_IMPL *las_session;
	uint64_t las_counter, las_pageid, remove_cnt;
	uint32_t las_id, session_flags;
	bool local_cursor, local_txn;

	remove_cnt = 0;
	session_flags = 0;		/* [-Wconditional-uninitialized] */

	local_cursor = local_txn = false;
	if (cursor == NULL) {
		__wt_las_cursor(session, &cursor, &session_flags);
		local_cursor = true;
	}
	las_session = (WT_SESSION_IMPL *)cursor->session;

	/*
	 * Wrap all of the removes in a transaction, unless this remove is part
	 * of a larger operation.
	 */
	if (local_cursor) {
		WT_ERR(__wt_txn_begin(las_session, NULL));
		las_session->txn.isolation = WT_TXN_ISO_READ_UNCOMMITTED;
		local_txn = true;
	}

	/*
	 * Search for the block's unique prefix and step through all matching
	 * records, removing them.
	 */
	ret = __wt_las_cursor_position(cursor, btree_id, pageid);
	for (; ret == 0; ret = cursor->next(cursor)) {
		WT_ERR(cursor->get_key(cursor,
		    &las_pageid, &las_id, &las_counter, &las_key));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (las_pageid != pageid || las_id != btree_id)
			break;

		WT_ERR(cursor->remove(cursor));
		++remove_cnt;
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	if (local_txn) {
		if (ret == 0)
			ret = __wt_txn_commit(las_session, NULL);
		else
			WT_TRET(__wt_txn_rollback(las_session, NULL));
	}
	if (local_cursor)
		WT_TRET(__wt_las_cursor_close(
		    session, &cursor, session_flags));

	WT_STAT_CONN_DECRV(session, cache_lookaside_entries, remove_cnt);
	__wt_cache_decr_check_uint64(session,
	    &S2C(session)->cache->las_entry_count, remove_cnt,
	    "lookaside entry count");
	return (ret);
}

/*
 * __wt_las_save_dropped --
 *	Save a dropped btree ID to be swept from the lookaside table.
 */
int
__wt_las_save_dropped(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CACHE *cache;
	WT_DECL_RET;

	btree = S2BT(session);
	cache = S2C(session)->cache;

	__wt_spin_lock(session, &cache->las_sweep_lock);
	WT_ERR(__wt_realloc_def(session, &cache->las_dropped_alloc,
	    cache->las_dropped_next + 1, &cache->las_dropped));
	cache->las_dropped[cache->las_dropped_next++] = btree->id;
err:	__wt_spin_unlock(session, &cache->las_sweep_lock);
	return (ret);
}

/*
 * __las_sweep_init --
 *	Prepare to start a lookaside sweep.
 */
static int
__las_sweep_init(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	u_int i;

	cache = S2C(session)->cache;

	__wt_spin_lock(session, &cache->las_sweep_lock);
	/* If no files have been dropped, there's nothing to do. */
	if (cache->las_dropped_next == 0)
		WT_ERR(WT_NOTFOUND);

	/* Scan the btree IDs to find min/max. */
	cache->las_sweep_dropmin = UINT32_MAX;
	cache->las_sweep_dropmax = 0;
	for (i = 0; i < cache->las_dropped_next; i++) {
		cache->las_sweep_dropmin = WT_MIN(
		    cache->las_sweep_dropmin,
		    cache->las_dropped[i]);
		cache->las_sweep_dropmax = WT_MAX(
		    cache->las_sweep_dropmax,
		    cache->las_dropped[i]);
	}

	/* Initialize the bitmap. */
	__wt_free(session, cache->las_sweep_dropmap);
	WT_ERR(__bit_alloc(session,
	    1 + cache->las_sweep_dropmax - cache->las_sweep_dropmin,
	    &cache->las_sweep_dropmap));
	for (i = 0; i < cache->las_dropped_next; i++)
		__bit_set(cache->las_sweep_dropmap,
		    cache->las_dropped[i] - cache->las_sweep_dropmin);

	/* Clear the list of btree IDs. */
	cache->las_dropped_next = 0;

err:	__wt_spin_unlock(session, &cache->las_sweep_lock);
	return (ret);
}

/*
 * __wt_las_sweep --
 *	Sweep the lookaside table.
 */
int
__wt_las_sweep(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM *key, las_key;
	uint64_t cnt, las_counter, las_pageid, remove_cnt;
	uint32_t las_id, session_flags;
	int notused;

	cache = S2C(session)->cache;
	cursor = NULL;
	key = &cache->las_sweep_key;
	remove_cnt = 0;
	session_flags = 0;		/* [-Werror=maybe-uninitialized] */

	__wt_las_cursor(session, &cursor, &session_flags);

	/* We should have our own session. */
	WT_ASSERT(session, cursor->session == &session->iface);

	/*
	 * When continuing a sweep, position the cursor using the key from the
	 * last call (we don't care if we're before or after the key, either
	 * side is fine).
	 *
	 * Otherwise, we're starting a new sweep, gather the list of trees to
	 * sweep.
	 */
	if (key->size != 0) {
		__wt_cursor_set_raw_key(cursor, key);
		ret = cursor->search_near(cursor, &notused);

		/*
		 * Don't search for the same key twice; if we don't set a new
		 * key below, it's because we've reached the end of the table
		 * and we want the next pass to start at the beginning of the
		 * table. Searching for the same key could leave us stuck at
		 * the end of the table, repeatedly checking the same rows.
		 */
		key->size = 0;
	} else
		ret = __las_sweep_init(session);

	if (ret != 0)
		goto srch_notfound;

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
	 */
	cnt = (uint64_t)WT_MAX(100, cache->las_entry_count / 30);

	/* Walk the file. */
	for (; cnt > 0 && (ret = cursor->next(cursor)) == 0; --cnt) {
		/*
		 * Give up if the cache is stuck: we are ignoring the cache
		 * size while scanning the lookaside table, so we're making
		 * things worse.
		 */
		if (__wt_cache_stuck(session))
			cnt = 1;

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
		    &las_pageid, &las_id, &las_counter, &las_key));

		/*
		 * If the entry belongs to a dropped tree, discard it.
		 *
		 * Cursor opened overwrite=true: won't return WT_NOTFOUND
		 * should another thread remove the record before we do (not
		 * expected for dropped trees), and the cursor remains
		 * positioned in that case.
		 *
		 * TODO it would also be good to remove entries in lookaside
		 * from live files that have aged out.  If we track for each
		 * entry whether it was the on-page value chosen by
		 * reconciliation, we can safely remove entries from that point
		 * on (for the given key) that are visible to all readers.
		 */
		if (las_id >= cache->las_sweep_dropmin &&
		    las_id <= cache->las_sweep_dropmax &&
		    __bit_test(cache->las_sweep_dropmap,
		    las_id - cache->las_sweep_dropmin)) {
			WT_ERR(cursor->remove(cursor));
			++remove_cnt;
		}
	}

srch_notfound:
	WT_ERR_NOTFOUND_OK(ret);

	if (0) {
err:		__wt_buf_free(session, key);
	}

	WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));

	__wt_cache_decr_check_uint64(session,
	    &S2C(session)->cache->las_entry_count, remove_cnt,
	    "lookaside entry count");

	return (ret);
}
