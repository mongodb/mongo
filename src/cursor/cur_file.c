/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * WT_BTREE_CURSOR_SAVE_AND_RESTORE
 *	Save the cursor's key/value data/size fields, call an underlying btree
 * function, and then consistently handle failure and success.
 */
#define	WT_BTREE_CURSOR_SAVE_AND_RESTORE(cursor, f, ret) do {		\
	const void *__key_data = (cursor)->key.data;			\
	const void *__value_data = (cursor)->value.data;		\
	uint64_t __recno = (cursor)->recno;				\
	uint32_t __key_size = (cursor)->key.size;			\
	uint32_t __value_size = (cursor)->value.size;			\
	if (((ret) = (f)) == 0) {					\
		F_CLR(cursor, WT_CURSTD_KEY_APP | WT_CURSTD_VALUE_APP);	\
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);	\
	} else {							\
		if (F_ISSET(cursor, WT_CURSTD_KEY_APP)) {		\
			(cursor)->recno = __recno;			\
			(cursor)->key.data = __key_data;		\
			(cursor)->key.size = __key_size;		\
		}							\
		if (F_ISSET(cursor, WT_CURSTD_VALUE_APP)) {		\
			(cursor)->value.data = __value_data;		\
			(cursor)->value.size = __value_size;		\
		}							\
		F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);	\
	}								\
} while (0)

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
	CURSOR_API_CALL(a, session, compare, cbt->btree);

	/*
	 * Confirm both cursors refer to the same source and have keys, then
	 * call the underlying object to compare them.
	 */
	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL,
		    "Cursors must reference the same object");

	WT_CURSOR_NEEDKEY(a);
	WT_CURSOR_NEEDKEY(b);

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
	CURSOR_API_CALL(cursor, session, next, cbt->btree);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if ((ret = __wt_btcur_next(cbt, 0)) == 0)
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

err:	API_END(session);
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
	CURSOR_API_CALL(cursor, session, next, cbt->btree);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if ((ret = __wt_btcur_next_random(cbt)) == 0)
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

err:	API_END(session);
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
	CURSOR_API_CALL(cursor, session, prev, cbt->btree);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if ((ret = __wt_btcur_prev(cbt, 0)) == 0)
		F_SET(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

err:	API_END(session);
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
	CURSOR_API_CALL(cursor, session, reset, cbt->btree);

	ret = __wt_btcur_reset(cbt);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

err:	API_END(session);
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
	CURSOR_API_CALL(cursor, session, search, cbt->btree);

	WT_CURSOR_NEEDKEY(cursor);

	WT_BTREE_CURSOR_SAVE_AND_RESTORE(cursor, __wt_btcur_search(cbt), ret);

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
	CURSOR_API_CALL(cursor, session, search_near, cbt->btree);

	WT_CURSOR_NEEDKEY(cursor);

	WT_BTREE_CURSOR_SAVE_AND_RESTORE(
	    cursor, __wt_btcur_search_near(cbt, exact), ret);

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
	CURSOR_UPDATE_API_CALL(cursor, session, insert, cbt->btree);
	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);

	WT_BTREE_CURSOR_SAVE_AND_RESTORE(cursor, __wt_btcur_insert(cbt), ret);

err:	CURSOR_UPDATE_API_END(session, ret);
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
	CURSOR_UPDATE_API_CALL(cursor, session, update, cbt->btree);

	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);

	WT_BTREE_CURSOR_SAVE_AND_RESTORE(cursor, __wt_btcur_update(cbt), ret);

err:	CURSOR_UPDATE_API_END(session, ret);
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
	CURSOR_UPDATE_API_CALL(cursor, session, remove, cbt->btree);

	WT_CURSOR_NEEDKEY(cursor);

	WT_BTREE_CURSOR_SAVE_AND_RESTORE(cursor, __wt_btcur_remove(cbt), ret);

	/*
	 * After a successful remove, the key and value are not available.
	 * This has to come after the call to resolve the cursor, it sets
	 * the same flags we're clearing.
	 */
	if (ret == 0)
		F_CLR(cursor, WT_CURSTD_KEY_RET | WT_CURSTD_VALUE_RET);

err:	CURSOR_UPDATE_API_END(session, ret);
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
	WT_CURSOR_BTREE *cursor;
	WT_DECL_RET;

	/*
	 * !!!
	 * We're doing a cursor operation but in the service of the session API;
	 * set the session handle to reference the appropriate Btree, but don't
	 * do any of the other "standard" cursor API setup.
	 */
	cursor = (WT_CURSOR_BTREE *)(start == NULL ? stop : start);
	WT_WITH_BTREE(session, cursor->btree,
	    ret = __wt_btcur_truncate(
	    (WT_CURSOR_BTREE *)start, (WT_CURSOR_BTREE *)stop));

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
	CURSOR_API_CALL(cursor, session, close, cbt->btree);
	WT_TRET(__wt_btcur_close(cbt));
	if (cbt->btree != NULL)
		WT_TRET(__wt_session_release_btree(session));
	/* The URI is owned by the btree handle. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curfile_create --
 *	Open a cursor for a given btree handle.
 */
int
__wt_curfile_create(WT_SESSION_IMPL *session,
    WT_CURSOR *owner, const char *cfg[], int bulk, int bitmap,
    WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    __curfile_compare,		/* compare */
	    __curfile_next,		/* next */
	    __curfile_prev,		/* prev */
	    __curfile_reset,		/* reset */
	    __curfile_search,		/* search */
	    __curfile_search_near,	/* search-near */
	    __curfile_insert,		/* insert */
	    __curfile_update,		/* update */
	    __curfile_remove,		/* remove */
	    __curfile_close);		/* close */
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	size_t csize;

	cbt = NULL;

	btree = S2BT(session);
	WT_ASSERT(session, btree != NULL);

	csize = bulk ? sizeof(WT_CURSOR_BULK) : sizeof(WT_CURSOR_BTREE);
	WT_RET(__wt_calloc(session, 1, csize, &cbt));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = btree->dhandle->name;
	cursor->key_format = btree->key_format;
	cursor->value_format = btree->value_format;

	cbt->btree = btree;
	if (bulk)
		WT_ERR(__wt_curbulk_init((WT_CURSOR_BULK *)cbt, bitmap));

	/*
	 * random_retrieval
	 * Random retrieval cursors only support next, reset and close.
	 */
	WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
	if (cval.val != 0) {
		__wt_cursor_set_notsup(cursor);
		cursor->next = __curfile_next_random;
		cursor->reset = __curfile_reset;
	}

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	STATIC_ASSERT(offsetof(WT_CURSOR_BTREE, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

	WT_CSTAT_INCR(session, cursor_create);
	WT_DSTAT_INCR(session, cursor_create);

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
	int bitmap, bulk;
	uint32_t flags;

	flags = 0;

	WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
	if (cval.type == WT_CONFIG_ITEM_BOOL ||
	    (cval.type == WT_CONFIG_ITEM_NUM &&
	    (cval.val == 0 || cval.val == 1))) {
		bitmap = 0;
		bulk = (cval.val != 0);
	} else if (WT_STRING_MATCH("bitmap", cval.str, cval.len))
		bitmap = bulk = 1;
	else
		WT_RET_MSG(session, EINVAL,
		    "Value for 'bulk' must be a boolean or 'bitmap'");

	/* Bulk handles require exclusive access. */
	if (bulk)
		LF_SET(WT_BTREE_BULK | WT_DHANDLE_EXCLUSIVE);

	/* Get the handle and lock it while the cursor is using it. */
	if (WT_PREFIX_MATCH(uri, "file:"))
		WT_RET(__wt_session_get_btree_ckpt(session, uri, cfg, flags));
	else
		WT_RET(__wt_bad_object_type(session, uri));

	WT_ERR(__wt_curfile_create(session, owner, cfg, bulk, bitmap, cursorp));
	return (0);

err:	/* If the cursor could not be opened, release the handle. */
	WT_TRET(__wt_session_release_btree(session));
	return (ret);
}
