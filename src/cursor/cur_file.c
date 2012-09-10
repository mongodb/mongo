/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curfile_compare --
 *	WT_CURSOR->compare method for the btree cursor type.
 */
static int
__curfile_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)a;
	CURSOR_API_CALL_NOCONF(a, session, compare, cbt->btree);

	/*
	 * Confirm both cursors refer to the same source, then call the
	 * underlying object to compare them.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "comparison method cursors must reference the same object");

	ret = __wt_btcur_compare(
	    (WT_CURSOR_BTREE *)a, (WT_CURSOR_BTREE *)b, cmpp);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_next --
 *	WT_CURSOR->next method for the btree cursor type.
 */
static int
__curfile_next(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, next, cbt->btree);
	ret = __wt_btcur_next((WT_CURSOR_BTREE *)cursor, 0);
	API_END(session);

	return (ret);
}

/*
 * __curfile_next_random --
 *	WT_CURSOR->next method for the btree cursor type when configured with
 * next_random.
 */
static int
__curfile_next_random(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, next, cbt->btree);
	ret = __wt_btcur_next_random(cbt);
	API_END(session);

	return (ret);
}

/*
 * __curfile_prev --
 *	WT_CURSOR->prev method for the btree cursor type.
 */
static int
__curfile_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, prev, cbt->btree);
	ret = __wt_btcur_prev((WT_CURSOR_BTREE *)cursor, 0);
	API_END(session);

	return (ret);
}

/*
 * __curfile_reset --
 *	WT_CURSOR->reset method for the btree cursor type.
 */
static int
__curfile_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, reset, cbt->btree);
	ret = __wt_btcur_reset(cbt);
	API_END(session);

	return (ret);
}

/*
 * __curfile_search --
 *	WT_CURSOR->search method for the btree cursor type.
 */
static int
__curfile_search(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, search, cbt->btree);
	WT_ERR(WT_CURSOR_NEEDKEY(cursor));
	ret = __wt_btcur_search(cbt);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_search_near --
 *	WT_CURSOR->search_near method for the btree cursor type.
 */
static int
__curfile_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, search_near, cbt->btree);
	WT_ERR(WT_CURSOR_NEEDKEY(cursor));
	ret = __wt_btcur_search_near(cbt, exact);
err:	API_END(session);

	return (ret);
}

/*
 * __curfile_insert --
 *	WT_CURSOR->insert method for the btree cursor type.
 */
static int
__curfile_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, insert, cbt->btree);
	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_ERR(WT_CURSOR_NEEDKEY(cursor));
	WT_ERR(WT_CURSOR_NEEDVALUE(cursor));
	ret = __wt_btcur_insert((WT_CURSOR_BTREE *)cursor);
err:	API_END_TXN_ERROR(session, ret);

	return (ret);
}

/*
 * __curfile_update --
 *	WT_CURSOR->update method for the btree cursor type.
 */
static int
__curfile_update(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, update, cbt->btree);
	WT_ERR(WT_CURSOR_NEEDKEY(cursor));
	WT_ERR(WT_CURSOR_NEEDVALUE(cursor));
	ret = __wt_btcur_update((WT_CURSOR_BTREE *)cursor);
err:	API_END_TXN_ERROR(session, ret);

	return (ret);
}

/*
 * __curfile_remove --
 *	WT_CURSOR->remove method for the btree cursor type.
 */
static int
__curfile_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, remove, cbt->btree);
	WT_ERR(WT_CURSOR_NEEDKEY(cursor));
	ret = __wt_btcur_remove((WT_CURSOR_BTREE *)cursor);
err:	API_END_TXN_ERROR(session, ret);

	return (ret);
}

/*
 * __wt_curfile_truncate --
 *	WT_SESSION.truncate support when file cursors are specified.
 */
int
__wt_curfile_truncate(
    WT_SESSION_IMPL *session, WT_CURSOR *start, WT_CURSOR *stop)
{
	WT_BTREE *saved_btree;
	WT_CURSOR_BTREE *cursor;
	WT_DECL_RET;
	int cmp, is_column;

	/*
	 * We're called by either the session layer or the table-cursor truncate
	 * code: in both cases, the key must have been set but the cursor itself
	 * may not be positioned.
	 */
	if (start != NULL)
		WT_RET(WT_CURSOR_NEEDKEY(start));
	if (stop != NULL)
		WT_RET(WT_CURSOR_NEEDKEY(stop));

	/*
	 * If both cursors set, check they're correctly ordered with respect to
	 * each other.  We have to test this before any column-store search, the
	 * search can change the initial cursor position.
	 */
	if (start != NULL && stop != NULL) {
		WT_RET(__curfile_compare(start, stop, &cmp));
		if (cmp > 0)
			WT_RET_MSG(session, EINVAL,
			    "the start cursor position is after the stop "
			    "cursor position");
	}

	/*
	 * Column-store cursors might not reference a valid record: applications
	 * can specify records larger than the current maximum record and create
	 * implicit records (variable-length column-store deleted records, or
	 * fixed-length column-store records with a value of 0).  Column-store
	 * calls search-near for this reason.  That's currently only necessary
	 * for variable-length column-store because fixed-length column-store
	 * returns the implicitly created records, but it's simpler to test for
	 * column-store than to test for the value type.
	 *
	 * Additionally, column-store corrects after search-near positioning the
	 * start/stop cursors on the next record greater-than/less-than or equal
	 * to the original key.  If the start/stop cursors hit the beginning/end
	 * of the object, or the start/stop record numbers cross, we're done as
	 * the range is empty.
	 */
	if (start == NULL)
		is_column = WT_CURSOR_RECNO(stop);
	else
		is_column = WT_CURSOR_RECNO(start);
	if (is_column) {
		if (start != NULL) {
			WT_RET(start->search_near(start, &cmp));
			if (cmp < 0 && (ret = start->next(start)) != 0)
				return (ret == WT_NOTFOUND ? 0 : ret);
		}
		if (stop != NULL) {
			WT_RET(stop->search_near(stop, &cmp));
			if (cmp > 0 && (ret = stop->prev(stop)) != 0)
				return (ret == WT_NOTFOUND ? 0 : ret);

			/* Check for crossing key/record numbers. */
			if (start != NULL && start->recno > stop->recno)
				return (0);
		}
	}

	/*
	 * !!!
	 * We're doing a cursor operation but in the service of the session API;
	 * set the session handle to reference the appropriate Btree, but don't
	 * do any of the other "standard" cursor API setup.
	 */
	cursor = (WT_CURSOR_BTREE *)(start == NULL ? stop : start);
	saved_btree = session->btree;
	session->btree = cursor->btree;
	ret = __wt_btcur_truncate(
	    (WT_CURSOR_BTREE *)start, (WT_CURSOR_BTREE *)stop);
	session->btree = saved_btree;

	return (ret);
}

/*
 * __curfile_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curfile_close(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, close, cbt->btree);
	WT_TRET(__wt_btcur_close(cbt));
	if (session->btree != NULL)
		WT_TRET(__wt_session_release_btree(session));
	/* The URI is owned by the btree handle. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));
	API_END_TXN_ERROR(session, ret);

	return (ret);
}

/*
 * __wt_curfile_create --
 *	Open a cursor for a given btree handle.
 */
int
__wt_curfile_create(WT_SESSION_IMPL *session,
    WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__curfile_next,
		__curfile_prev,
		__curfile_reset,
		__curfile_search,
		__curfile_search_near,
		__curfile_insert,
		__curfile_update,
		__curfile_remove,
		__curfile_close,
		__curfile_compare,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },			/* recno raw buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	size_t csize;
	int bulk;

	cbt = NULL;

	btree = session->btree;
	WT_ASSERT(session, btree != NULL);

	WT_RET(__wt_config_gets_defno(session, cfg, "bulk", &cval));
	bulk = (cval.val != 0);

	csize = bulk ? sizeof(WT_CURSOR_BULK) : sizeof(WT_CURSOR_BTREE);
	WT_RET(__wt_calloc(session, 1, csize, &cbt));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = btree->name;
	cursor->key_format = btree->key_format;
	cursor->value_format = btree->value_format;

	cbt->btree = session->btree;
	if (bulk)
		WT_ERR(__wt_curbulk_init((WT_CURSOR_BULK *)cbt));

	/*
	 * random_retrieval
	 * Random retrieval cursors only support next and close.
	 */
	WT_ERR(__wt_config_gets_defno(session, cfg, "next_random", &cval));
	if (cval.val != 0) {
		__wt_cursor_set_notsup(cursor);
		cursor->next = __curfile_next_random;
	}

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	STATIC_ASSERT(offsetof(WT_CURSOR_BTREE, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

	if (0) {
err:		__wt_free(session, cbt);
	}

	return (ret);
}

/*
 * __wt_curfile_open --
 *	WT_SESSION->open_cursor method for the btree cursor type.
 */
int
__wt_curfile_open(WT_SESSION_IMPL *session, const char *uri,
    WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	int bulk;

	WT_RET(__wt_config_gets_defno(session, cfg, "bulk", &cval));
	bulk = (cval.val != 0);

	/* TODO: handle projections. */

	/* Get the handle and lock it while the cursor is using it. */
	if (WT_PREFIX_MATCH(uri, "colgroup:") || WT_PREFIX_MATCH(uri, "index:"))
		WT_RET(__wt_schema_get_btree(session, uri, strlen(uri), cfg,
		    bulk ? WT_BTREE_BULK | WT_BTREE_EXCLUSIVE : 0));
	else if (WT_PREFIX_MATCH(uri, "file:"))
		WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg,
		    bulk ? WT_BTREE_BULK | WT_BTREE_EXCLUSIVE : 0));
	else
		WT_RET(__wt_bad_object_type(session, uri));

	WT_ERR(__wt_curfile_create(session, owner, cfg, cursorp));
	return (0);

err:	WT_WITH_SCHEMA_LOCK(session, (void)__wt_session_release_btree(session));
	return (ret);
}
