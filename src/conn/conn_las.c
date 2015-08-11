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
	WT_CONNECTION_STATS *stats;

	conn = S2C(session);
	stats = &conn->stats;

	/*
	 * Lookaside table statistics are copied from the underlying lookaside
	 * table data-source statistics. If there's no lookaside table (yet),
	 * the values remain 0.
	 */
	if ((session = conn->las_session) == NULL || session->dhandle == NULL)
		return;

	WT_STAT_SET(stats, lookaside_cursor_insert,
	    WT_STAT(&session->dhandle->stats, cursor_insert));
	WT_STAT_SET(stats, lookaside_cursor_insert_bytes,
	    WT_STAT(&session->dhandle->stats, cursor_insert_bytes));
	WT_STAT_SET(stats, lookaside_cursor_remove,
	    WT_STAT(&session->dhandle->stats, cursor_remove));
}

/*
 * __las_drop --
 *	Discard the database's lookaside store.
 */
static int
__las_drop(WT_SESSION_IMPL *session)
{
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL };

	return (__wt_session_drop(session, WT_LASFILE_URI, drop_cfg));
}

/*
 * __las_cursor_create --
 *	Open a new lookaside file cursor.
 */
static int
__las_cursor_create(WT_SESSION_IMPL *session, WT_CURSOR **cursorp)
{
	WT_BTREE *btree;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL };

	WT_RET(__wt_open_cursor(
	    session, WT_LASFILE_URI, NULL, open_cursor_cfg, cursorp));

	/*
	 * Sessions associated with a lookaside cursor should never be tapped
	 * for eviction. Presumably this is getting set when those sessions
	 * are created, but it costs us nothing to do it here as well (and in
	 * the case of the first-created, shared, connection-level lookaside
	 * cursor, it has not yet been done).
	 */
	F_SET(session, WT_SESSION_NO_EVICTION);

	/*
	 * Set special flags for the lookaside file: the lookaside flag (used,
	 * for example, to avoid writing records during reconciliation), also
	 * turn off checkpoints and logging.
	 *
	 * Test flags before setting them so updates can't race in subsequent
	 * opens (the first update is safe because it's single-threaded from
	 * wiredtiger_open).
	 */
	btree = S2BT(session);
	if (!F_ISSET(btree, WT_BTREE_LAS_FILE))
		F_SET(btree, WT_BTREE_LAS_FILE);
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

	conn = S2C(session);

	/*
	 * Done at startup: we cannot do it on demand because we require the
	 * schema lock to create and drop the file, and it may not always be
	 * available.
	 *
	 * Open an internal session, used for the shared lookaside cursor.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "lookaside file", 1, 1, &conn->las_session));
	session = conn->las_session;

	/* Discard any previous incarnation of the file. */
	WT_RET(__las_drop(session));

	/* Re-create the file. */
	WT_RET(__wt_session_create(
	    session, WT_LASFILE_URI, "key_format=u,value_format=QIu"));

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
 *	Flag that the lookaside file has been written.
 */
void
__wt_las_set_written(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	if (conn->las_written == 0)
		conn->las_written = 1;
}

/*
 * __wt_las_is_written --
 *	Return if the lookaside file has been written.
 */
int
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
    WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t *saved_flagsp)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	*cursorp = NULL;

	conn = S2C(session);

	/* Eviction and sweep threads have their own lookaside file cursors. */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR)) {
		if (session->las_cursor == NULL) {
			WT_WITHOUT_DHANDLE(session, ret =
			    __las_cursor_create(session, &session->las_cursor));
			WT_RET(ret);
		}

		*cursorp = session->las_cursor;
		return (0);
	}

	/* Lock the shared lookaside cursor. */
	__wt_spin_lock(session, &conn->las_lock);

	/*
	 * We don't want to get tapped for eviction anywhere in this process;
	 * save a copy of the current flags for when the cursor is closed, and
	 * turn eviction off.
	 */
	*saved_flagsp = session->flags;
	F_SET(session, WT_SESSION_NO_EVICTION);

	*cursorp = conn->las_cursor;
	return (0);
}

/*
 * __wt_las_cursor_close --
 *	Discard a lookaside cursor.
 */
int
__wt_las_cursor_close(
	WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t saved_flags)
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

	/* Eviction and sweep threads have their own lookaside file cursors. */
	if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
		return (ret);

	/*
	 * We turned off eviction while the lookaside cursor was in use, restore
	 * the session's flags.
	 */
	session->flags = saved_flags;

	/* Unlock the shared lookaside cursor. */
	__wt_spin_unlock(session, &conn->las_lock);

	return (ret);
}

/*
 * __las_sweep_reconcile --
 *	Return if reconciliation records in the lookaside file can be deleted.
 */
static int
__las_sweep_reconcile(WT_SESSION_IMPL *session, WT_ITEM *key)
{
	uint64_t txnid;
	uint8_t addr_size;
	void *p;

	/*
	 * Skip to the on-page transaction ID stored in the key; if it's
	 * globally visible, we no longer need this record, the on-page
	 * record is just as good.
	 */
	p = (uint8_t *)key->data;
	p = (uint8_t *)p + sizeof(char);		/* '1' */
	p = (uint8_t *)p + sizeof(uint32_t);		/* file ID */
	addr_size = *(uint8_t *)p;
	p = (uint8_t *)p + sizeof(uint8_t);		/* addr_size */
	p = (uint8_t *)p + addr_size;			/* addr */
	memcpy(&txnid, p, sizeof(uint64_t));

	return (__wt_txn_visible_all(session, txnid));
}

/*
 * __wt_las_sweep --
 *	Sweep the lookaside file.
 */
int
__wt_las_sweep(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM *key;
	uint64_t cnt;
	int notused;

	conn = S2C(session);

	/*
	 * If the sweep thread does not yet have a lookaside file cursor,
	 * create one.
	 */
	if (session->las_cursor == NULL) {
		WT_WITHOUT_DHANDLE(session,
		    ret = __las_cursor_create(session, &session->las_cursor));
		WT_RET(ret);
	}

	cursor = session->las_cursor;
	key = &conn->las_sweep_key;

	/*
	 * If we're not starting a new sweep, position the cursor using the key
	 * from the last call (we don't care if we're before or after the key,
	 * just roughly in the same spot is fine).
	 */
	if (conn->las_sweep_call != 0 && key->data != NULL) {
		cursor->set_key(cursor, key);
		if ((ret =
		    cursor->search_near(cursor, &notused)) == WT_NOTFOUND) {
			WT_ERR(cursor->reset(cursor));
			return (0);
		}
		WT_ERR(ret);
	}

	/*
	 * The sweep server wakes up every 10 seconds (by default), it's a slow
	 * moving thread. Try to review the entire lookaside file once every 5
	 * minutes, or every 30 calls.
	 *
	 * The reason is because the lookaside file exists because we're seeing
	 * cache/eviction pressure (it allows us to trade performance and disk
	 * space for cache space), and it's likely lookaside blocks are being
	 * evicted, and reading them back in doesn't help things. A trickier,
	 * but possibly better, alternative might be to review all lookaside
	 * blocks in the cache in order to get rid of them, and slowly review
	 * lookaside blocks that have already been evicted.
	 *
	 * We can't know for sure how many records are in the lookaside file,
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
		WT_ERR(cursor->get_key(cursor, key));

		switch (((uint8_t *)key->data)[0]) {
		case WT_LAS_RECONCILE_UPDATE:
			if (__las_sweep_reconcile(session, key)) {
				/*
				 * Cursor opened overwrite=true: it won't return
				 * WT_NOTFOUND if another thread removes the
				 * record before we do, and the cursor remains
				 * positioned in that case.
				 */
				WT_ERR(cursor->remove(cursor));
			}
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

	/*
	 * If the loop terminates when we've completed a work unit, we plan to
	 * continue the table sweep next time. Take a local copy of the sweep
	 * key, we're not going to leave the cursor positioned.
	 */
	if (ret == 0 && !WT_DATA_IN_ITEM(key))
		WT_ERR(__wt_buf_set(session, key, key->data, key->size));

	/*
	 * When reaching the lookaside file end or the target number of calls,
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
	if (ret == WT_NOTFOUND)
		conn->las_sweep_call = 0;

	WT_ERR_NOTFOUND_OK(ret);

	if (0) {
err:		__wt_buf_free(session, key);
	}

	WT_TRET(cursor->reset(cursor));

	return (ret);
}
