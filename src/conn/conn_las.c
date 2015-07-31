/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

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
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor),
	    "overwrite=false", NULL };

	/*
	 * Open the cursor (note the "overwrite=false" configuration, we want
	 * to see errors if we try to remove records that aren't there).
	 */
	return (__wt_open_cursor(
	    session, WT_LASFILE_URI, NULL, open_cursor_cfg, cursorp));
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

	/* Lock the lookaside file and check if we won the race. */
	__wt_spin_lock(session, &conn->las_lock);
	if (conn->las_cursor != NULL) {
		__wt_spin_unlock(session, &conn->las_lock);
		return (0);
	}

	/* Open an internal session, used for the shared lookaside cursor. */
	WT_ERR(__wt_open_internal_session(
	    conn, "lookaside file", 1, 1, &conn->las_session));
	session = conn->las_session;

	/* Discard any previous incarnation of the file. */
	WT_ERR(__las_drop(session));

	/* Re-create the file. */
	WT_ERR(__wt_session_create(
	    session, WT_LASFILE_URI, "key_format=u,value_format=QIu"));

	/* Open the shared cursor. */
	WT_ERR(__las_cursor_create(session, &conn->las_cursor));

	/*
	 * Configure the session:
	 *	Don't tap for eviction.
	 * Configure the session and file handles:
	 *	Force discard on release.
	 *	Flag as a lookaside handle (used, for example, to avoid writing
	 *	lookaside records for the lookaside file during reconciliation).
	 *	No checkpoints or logging.
	 */
	F_SET(session, WT_SESSION_NO_EVICTION);
	F_SET(session->dhandle, WT_DHANDLE_DISCARD_FORCE);
	F_SET(S2BT(session),
	    WT_BTREE_LAS_FILE | WT_BTREE_NO_CHECKPOINT | WT_BTREE_NO_LOGGING);

err:	__wt_spin_unlock(session, &conn->las_lock);
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

	/* Close the session, closing the open cursor. */
	wt_session = &conn->las_session->iface;
	WT_TRET(wt_session->close(wt_session, NULL));

	/*
	 * Clear the references (this isn't just for clarity, the underlying
	 * code uses the non-NULL cursor to determine if information in the
	 * lookaside file needs to be updated as blocks are freed).
	 */
	conn->las_cursor = NULL;
	conn->las_session = NULL;

	/*
	 * Discard any incarnation of the file.
	 *
	 * KEITH: I'm not sure about this: if WT_DHANDLE_DISCARD_FORCE isn't set
	 * on the lookaside data handle, we drop core removing the Btree handle
	 * from the connection session's hash list. We want to forcibly discard
	 * the file, but I'm concerned I'm using sessions in some illegal way.
	 */
	WT_TRET(__las_drop(session));

	return (ret);
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
	uint32_t saved_flags;

	*cursorp = NULL;

	conn = S2C(session);

	/*
	 * We don't want to get tapped for eviction anywhere in this process;
	 * immediately save a copy of the current flags for error or when the
	 * cursor is closed, and turn eviction off.
	 */
	saved_flags = session->flags;
	F_SET(session, WT_SESSION_NO_EVICTION);

	/* On the first access, create the shared lookaside store and cursor. */
	if (conn->las_cursor == NULL)
		WT_ERR(__wt_las_create(session));

	/* Eviction worker threads get their own lookaside file cursors. */
	if (F_ISSET(session, WT_SESSION_EVICTION_WORKER)) {
		if (session->las_cursor == NULL)
			WT_ERR(
			    __las_cursor_create(session, &session->las_cursor));

		*cursorp = session->las_cursor;
	} else {
		__wt_spin_lock(session, &conn->las_lock);

		*cursorp = conn->las_cursor;
	}
	*saved_flagsp = saved_flags;

	if (0) {
err:		session->flags = saved_flags;
	}
	return (ret);
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

	/* Unlock the shared cursor. */
	if (cursor == conn->las_cursor)
		__wt_spin_unlock(session, &conn->las_lock);

	/*
	 * We don't want to get tapped for eviction anywhere in this process;
	 * the last thing we do is turn eviction back on.
	 */
	session->flags = saved_flags;
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
	WT_CURSOR *cursor;
	WT_DECL_ITEM(klas);
	WT_DECL_RET;

	/*
	 * If the lookaside file isn't yet open, there's no work to do.
	 */
	if (S2C(session)->las_cursor == NULL)
		return (0);

	/*
	 * If the sweep thread does not yet have a lookaside file cursor,
	 * create one.
	 */
	if (session->las_cursor == NULL)
		WT_RET(__las_cursor_create(session, &session->las_cursor));
	cursor = session->las_cursor;

	/*
	 * KEITH
	 * There's some tuning questions: we should track the total number of
	 * records in the lookaside file, and only sweep if there are enough
	 * records to make sweeping worthwhile, and only sweep part of the file
	 * if there are too many to sweep at once. We might also want to sweep
	 * the records in the cache more frequently, and out-of-cache records
	 * less frequently.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &klas));

	/* Walk the file. */
	while ((ret = cursor->next(cursor)) == 0) {
		WT_ERR(cursor->get_key(cursor, klas));

		switch (((uint8_t *)klas->data)[0]) {
		case WT_LAS_RECONCILE_UPDATE:
			if (!__las_sweep_reconcile(session, klas))
				continue;
			break;
		WT_ILLEGAL_VALUE_ERR(session);
		}

		/*
		 * Make sure we have a local copy of the record.
		 *
		 * KEITH: Why is this necessary?
		 */
		if (!WT_DATA_IN_ITEM(klas))
			WT_ERR(__wt_buf_set(
			    session, klas, klas->data, klas->size));

		WT_ERR_NOTFOUND_OK(cursor->remove(cursor));
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* Don't leave the cursor positioned. */
err:	WT_TRET(cursor->reset(cursor));

	__wt_scr_free(session, &klas);
	return (ret);
}
