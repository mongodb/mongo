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
 * __wt_las_create --
 *	Initialize the database's lookaside store.
 */
int
__wt_las_create(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	const char *open_cursor_cfg[] = {
	    WT_CONFIG_BASE(session, WT_SESSION_open_cursor),
	    "overwrite=false", NULL };

	conn = S2C(session);

	/* Lock the lookaside table and check for a race. */
	__wt_spin_lock(session, &conn->las_lock);
	if (conn->las_cursor != NULL) {
		__wt_spin_unlock(session, &conn->las_lock);
		return (0);
	}

	/* Open an internal session, used for lookaside cursors. */
	WT_ERR(__wt_open_internal_session(
	    conn, "lookaside table", 1, 1, &conn->las_session));
	session = conn->las_session;

	/* Discard any previous incarnation of the file. */
	WT_ERR(__las_drop(session));

	/* Re-create the file. */
	WT_ERR(__wt_session_create(
	    session, WT_LASFILE_URI, "key_format=u,value_format=u"));

	/*
	 * Open the cursor. (Note the "overwrite=false" configuration, we want
	 * to see errors if we try to remove records that aren't there.)
	 */
	WT_ERR(__wt_open_cursor(
	    session, WT_LASFILE_URI, NULL, open_cursor_cfg, &conn->las_cursor));

	/*
	 * No cache checks.
	 * No lookaside records during reconciliation.
	 * No checkpoints or logging.
	 */
	F_SET(session, WT_SESSION_NO_CACHE_CHECK);
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
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_SESSION *wt_session;

	conn = S2C(session);

	if (conn->las_session == NULL)
		return (0);

	/* Close open cursors. */
	if ((cursor = conn->las_cursor) != NULL)
		WT_TRET(cursor->close(cursor));

	/* Discard any incarnation of the file. */
	WT_TRET(__las_drop(conn->las_session));

	/* Close the session. */
	wt_session = &conn->las_session->iface;
	WT_TRET(wt_session->close(wt_session, NULL));
	conn->las_session = NULL;

	return (ret);
}

/*
 * __wt_las_insert --
 *	Insert a record into the lookaside store.
 */
int
__wt_las_insert(WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	conn = S2C(session);

	/* On the first access, create the lookaside store and cursor. */
	if (conn->las_cursor == NULL)
		WT_RET(__wt_las_create(session));

	/* Lock the lookaside table */
	__wt_spin_lock(session, &conn->las_lock);

	/* Insert the key/value pair. */
	cursor = conn->las_cursor;
	cursor->set_key(cursor, key);
	cursor->set_value(cursor, value);
	ret = cursor->insert(cursor);

	/* Reset the cursor. */
	WT_TRET(cursor->reset(cursor));
	__wt_spin_unlock(session, &conn->las_lock);
	return (ret);
}

/*
 * __wt_las_search --
 *	Search for a record into the lookaside store.
 */
int
__wt_las_search(WT_SESSION_IMPL *session, WT_ITEM *key, WT_ITEM *value)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	int exact;

	conn = S2C(session);

	/* Lock the lookaside table */
	__wt_spin_lock(session, &conn->las_lock);

	cursor = conn->las_cursor;

	cursor->set_key(cursor, key);
	if ((ret = cursor->search_near(cursor, &exact)) == 0) {
		WT_ERR(cursor->get_key(cursor, key));
		WT_ERR(cursor->get_value(cursor, value));
	}

err:	WT_TRET(cursor->reset(cursor));
	__wt_spin_unlock(session, &conn->las_lock);
	return (ret);
}

/*
 * __wt_las_remove --
 *	Remove a record from the lookaside store.
 */
int
__wt_las_remove(WT_SESSION_IMPL *session, WT_ITEM *key)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;

	conn = S2C(session);

	/* Lock the lookaside table */
	__wt_spin_lock(session, &conn->las_lock);

	cursor = conn->las_cursor;

	cursor->set_key(cursor, key);
	ret = cursor->remove(cursor);

	WT_TRET(cursor->reset(cursor));
	__wt_spin_unlock(session, &conn->las_lock);
	return (ret);
}

/*
 * __wt_las_remove_block --
 *	Remove all records matching a key prefix from the lookaside store.
 */
int
__wt_las_remove_block(
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_DECL_ITEM(klas);
	size_t prefix_len;
	int exact;
	uint8_t prefix_key[100];
	void *p;

	conn = S2C(session);
	btree = S2BT(session);

	/*
	 * Called whenever a block is freed; if the lookaside store isn't yet
	 * open, there's no work to do.
	 */
	if (conn->las_cursor == NULL)
		return (0);

	/*
	 * Build the page's unique key prefix we'll search for in the lookaside
	 * table, based on the file's ID and the page's block address.
	 */
	p = prefix_key;
	*(char *)p = WT_LAS_RECONCILE_UPDATE;
	p = (uint8_t *)p + sizeof(char);
	memcpy(p, &btree->id, sizeof(uint32_t));
	p = (uint8_t *)p + sizeof(uint32_t);
	*(uint8_t *)p = (uint8_t)addr_size;
	p = (uint8_t *)p + sizeof(uint8_t);
	memcpy(p, addr, addr_size);
	p = (uint8_t *)p + addr_size;
	prefix_len =
	    sizeof(char) + sizeof(uint32_t) + sizeof(uint8_t) + addr_size;
	WT_ASSERT(session, WT_PTRDIFF(p, prefix_key) == prefix_len);

	/* Copy the unique prefix into the key. */
	WT_RET(__wt_scr_alloc(session, addr_size + 100, &klas));
	memcpy(klas->mem, prefix_key, prefix_len);
	klas->size = prefix_len;

	/* Lock the lookaside table */
	__wt_spin_lock(session, &conn->las_lock);

	cursor = conn->las_cursor;

	cursor->set_key(cursor, klas);
	while ((ret = cursor->search_near(cursor, &exact)) == 0) {
		WT_ERR(cursor->get_key(cursor, klas));

		/*
		 * Confirm the search using the unique prefix; if not a match,
		 * we're done searching for records for this page.
		 */
		if (klas->size <= prefix_len ||
		    memcmp(klas->data, prefix_key, prefix_len) != 0)
			break;

		/* Make sure we have a local copy of the record. */
		if (!WT_DATA_IN_ITEM(klas))
			WT_ERR(__wt_buf_set(
			    session, klas, klas->data, klas->size));

		WT_ERR(cursor->remove(cursor));
		klas->size = prefix_len;
	}
	WT_ERR_NOTFOUND_OK(ret);

err:	WT_TRET(cursor->reset(cursor));
	__wt_spin_unlock(session, &conn->las_lock);

	__wt_scr_free(session, &klas);
	return (ret);
}
