/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	CURSOR_API_CALL(a, session, compare, cbt->btree);

	/*
	 * Check both cursors are a "file:" type then call the underlying
	 * function, it can handle cursors pointing to different objects.
	 */
	if (!WT_PREFIX_MATCH(a->internal_uri, "file:") ||
	    !WT_PREFIX_MATCH(b->internal_uri, "file:"))
		WT_ERR_MSG(session, EINVAL,
		    "Cursors must reference the same object");

	WT_ERR(__cursor_checkkey(a));
	WT_ERR(__cursor_checkkey(b));

	ret = __wt_btcur_compare(
	    (WT_CURSOR_BTREE *)a, (WT_CURSOR_BTREE *)b, cmpp);

err:	API_END_RET(session, ret);
}

/*
 * __curfile_equals --
 *	WT_CURSOR->equals method for the btree cursor type.
 */
static int
__curfile_equals(WT_CURSOR *a, WT_CURSOR *b, int *equalp)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)a;
	CURSOR_API_CALL(a, session, equals, cbt->btree);

	/*
	 * Check both cursors are a "file:" type then call the underlying
	 * function, it can handle cursors pointing to different objects.
	 */
	if (!WT_PREFIX_MATCH(a->internal_uri, "file:") ||
	    !WT_PREFIX_MATCH(b->internal_uri, "file:"))
		WT_ERR_MSG(session, EINVAL,
		    "Cursors must reference the same object");

	WT_ERR(__cursor_checkkey(a));
	WT_ERR(__cursor_checkkey(b));

	ret = __wt_btcur_equals(
	    (WT_CURSOR_BTREE *)a, (WT_CURSOR_BTREE *)b, equalp);

err:	API_END_RET(session, ret);
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

	WT_ERR(__wt_btcur_next(cbt, false));

	/* Next maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
}

/*
 * __wt_curfile_next_random --
 *	WT_CURSOR->next method for the btree cursor type when configured with
 * next_random. This is exported because it is called directly within LSM.
 */
int
__wt_curfile_next_random(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, next, cbt->btree);

	WT_ERR(__wt_btcur_next_random(cbt));

	/* Next-random maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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

	WT_ERR(__wt_btcur_prev(cbt, false));

	/* Prev maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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

	/* Reset maintains no position, key or value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == 0 &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:	API_END_RET(session, ret);
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
	WT_ERR(__cursor_checkkey(cursor));

	WT_ERR(__wt_btcur_search(cbt));

	/* Search maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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
	WT_ERR(__cursor_checkkey(cursor));

	WT_ERR(__wt_btcur_search_near(cbt, exact));

	/* Search-near maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

err:	API_END_RET(session, ret);
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
	CURSOR_UPDATE_API_CALL_BTREE(cursor, session, insert, cbt->btree);

	if (!F_ISSET(cursor, WT_CURSTD_APPEND))
		WT_ERR(__cursor_checkkey(cursor));
	WT_ERR(__cursor_checkvalue(cursor));

	WT_ERR(__wt_btcur_insert(cbt));

	/*
	 * Insert maintains no position, key or value (except for column-store
	 * appends, where we are returning a key).
	 */
	WT_ASSERT(session,
	    (F_ISSET(cursor, WT_CURSTD_APPEND) &&
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT) ||
	    (!F_ISSET(cursor, WT_CURSTD_APPEND) &&
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == 0));

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __wt_curfile_insert_check --
 *	WT_CURSOR->insert_check method for the btree cursor type.
 */
int
__wt_curfile_insert_check(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_UPDATE_API_CALL_BTREE(cursor, session, update, cbt->btree);
	WT_ERR(__cursor_checkkey(cursor));

	ret = __wt_btcur_insert_check(cbt);

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
	CURSOR_UPDATE_API_CALL_BTREE(cursor, session, update, cbt->btree);
	WT_ERR(__cursor_checkkey(cursor));
	WT_ERR(__cursor_checkvalue(cursor));

	WT_ERR(__wt_btcur_update(cbt));

	/* Update maintains a position, key and value. */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT &&
	    F_MASK(cursor, WT_CURSTD_VALUE_SET) == WT_CURSTD_VALUE_INT);

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
	CURSOR_REMOVE_API_CALL(cursor, session, cbt->btree);
	WT_ERR(__cursor_checkkey(cursor));

	WT_ERR(__wt_btcur_remove(cbt));

	/*
	 * Remove with a search-key is fire-and-forget, no position and no key.
	 * Remove starting from a position maintains the position and a key.
	 * We don't know which it was at this layer, so can only assert the key
	 * is not set at all, or internal. There's never a value.
	 */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == 0 ||
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
	WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __curfile_reserve --
 *	WT_CURSOR->reserve method for the btree cursor type.
 */
static int
__curfile_reserve(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_UPDATE_API_CALL_BTREE(cursor, session, reserve, cbt->btree);
	WT_ERR(__cursor_checkkey(cursor));

	WT_ERR(__wt_txn_context_check(session, true));

	WT_ERR(__wt_btcur_reserve(cbt));

	/*
	 * Reserve maintains a position and key, which doesn't match the library
	 * API, where reserve maintains a value. Fix the API by searching after
	 * each successful reserve operation.
	 */
	WT_ASSERT(session,
	    F_MASK(cursor, WT_CURSTD_KEY_SET) == WT_CURSTD_KEY_INT);
	WT_ASSERT(session, F_MASK(cursor, WT_CURSTD_VALUE_SET) == 0);

err:	CURSOR_UPDATE_API_END(session, ret);

	/*
	 * The application might do a WT_CURSOR.get_value call when we return,
	 * so we need a value and the underlying functions didn't set one up.
	 * For various reasons, those functions may not have done a search and
	 * any previous value in the cursor might race with WT_CURSOR.reserve
	 * (and in cases like LSM, the reserve never encountered the original
	 * key). For simplicity, repeat the search here.
	 */
	return (ret == 0 ? cursor->search(cursor) : ret);
}

/*
 * __curfile_close --
 *	WT_CURSOR->close method for the btree cursor type.
 */
static int
__curfile_close(WT_CURSOR *cursor)
{
	WT_CURSOR_BTREE *cbt;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbt = (WT_CURSOR_BTREE *)cursor;
	CURSOR_API_CALL(cursor, session, close, cbt->btree);
	if (F_ISSET(cursor, WT_CURSTD_BULK)) {
		/* Free the bulk-specific resources. */
		cbulk = (WT_CURSOR_BULK *)cbt;
		WT_TRET(__wt_bulk_wrapup(session, cbulk));
		__wt_buf_free(session, &cbulk->last);
	}

	WT_TRET(__wt_btcur_close(cbt, false));
	/* The URI is owned by the btree handle. */
	cursor->internal_uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));

	/*
	 * Note: release the data handle last so that cursor statistics are
	 * updated correctly.
	 */
	if (session->dhandle != NULL) {
		/* Decrement the data-source's in-use counter. */
		__wt_cursor_dhandle_decr_use(session);
		WT_TRET(__wt_session_release_btree(session));
	}

err:	API_END_RET(session, ret);
}

/*
 * __curfile_create --
 *	Open a cursor for a given btree handle.
 */
static int
__curfile_create(WT_SESSION_IMPL *session,
    WT_CURSOR *owner, const char *cfg[], bool bulk, bool bitmap,
    WT_CURSOR **cursorp)
{
	WT_CURSOR_STATIC_INIT(iface,
	    __wt_cursor_get_key,		/* get-key */
	    __wt_cursor_get_value,		/* get-value */
	    __wt_cursor_set_key,		/* set-key */
	    __wt_cursor_set_value,		/* set-value */
	    __curfile_compare,			/* compare */
	    __curfile_equals,			/* equals */
	    __curfile_next,			/* next */
	    __curfile_prev,			/* prev */
	    __curfile_reset,			/* reset */
	    __curfile_search,			/* search */
	    __curfile_search_near,		/* search-near */
	    __curfile_insert,			/* insert */
	    __wt_cursor_modify_notsup,		/* modify */
	    __curfile_update,			/* update */
	    __curfile_remove,			/* remove */
	    __curfile_reserve,			/* reserve */
	    __wt_cursor_reconfigure,		/* reconfigure */
	    __curfile_close);			/* close */
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_BTREE *cbt;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	size_t csize;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_BTREE, iface) == 0);

	cbt = NULL;

	btree = S2BT(session);
	WT_ASSERT(session, btree != NULL);

	csize = bulk ? sizeof(WT_CURSOR_BULK) : sizeof(WT_CURSOR_BTREE);
	WT_RET(__wt_calloc(session, 1, csize, &cbt));

	cursor = &cbt->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->internal_uri = btree->dhandle->name;
	cursor->key_format = btree->key_format;
	cursor->value_format = btree->value_format;
	cbt->btree = btree;

	/*
	 * Increment the data-source's in-use counter; done now because closing
	 * the cursor will decrement it, and all failure paths from here close
	 * the cursor.
	 */
	__wt_cursor_dhandle_incr_use(session);

	if (session->dhandle->checkpoint != NULL)
		F_SET(cbt, WT_CBT_NO_TXN);

	if (bulk) {
		F_SET(cursor, WT_CURSTD_BULK);

		cbulk = (WT_CURSOR_BULK *)cbt;

		/* Optionally skip the validation of each bulk-loaded key. */
		WT_ERR(__wt_config_gets_def(
		    session, cfg, "skip_sort_check", 0, &cval));
		WT_ERR(__wt_curbulk_init(
		    session, cbulk, bitmap, cval.val == 0 ? 0 : 1));
	}

	/*
	 * Random retrieval, row-store only.
	 * Random retrieval cursors support a limited set of methods.
	 */
	WT_ERR(__wt_config_gets_def(session, cfg, "next_random", 0, &cval));
	if (cval.val != 0) {
		if (WT_CURSOR_RECNO(cursor))
			WT_ERR_MSG(session, ENOTSUP,
			    "next_random configuration not supported for "
			    "column-store objects");

		__wt_cursor_set_notsup(cursor);
		cursor->next = __wt_curfile_next_random;
		cursor->reset = __curfile_reset;

		WT_ERR(__wt_config_gets_def(
		    session, cfg, "next_random_sample_size", 0, &cval));
		if (cval.val != 0)
			cbt->next_random_sample_size = (u_int)cval.val;
	}

	/* Underlying btree initialization. */
	__wt_btcur_open(cbt);

	WT_ERR(__wt_cursor_init(
	    cursor, cursor->internal_uri, owner, cfg, cursorp));

	WT_STAT_CONN_INCR(session, cursor_create);
	WT_STAT_DATA_INCR(session, cursor_create);

	if (0) {
err:		/*
		 * Our caller expects to release the data handle if we fail.
		 * Disconnect it from the cursor before closing.
		 */
		if (session->dhandle != NULL)
			__wt_cursor_dhandle_decr_use(session);
		cbt->btree = NULL;
		WT_TRET(__curfile_close(cursor));
		*cursorp = NULL;
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
	uint32_t flags;
	bool bitmap, bulk, checkpoint_wait;

	bitmap = bulk = false;
	checkpoint_wait = true;
	flags = 0;

	/*
	 * Decode the bulk configuration settings. In memory databases
	 * ignore bulk load.
	 */
	if (!F_ISSET(S2C(session), WT_CONN_IN_MEMORY)) {
		WT_RET(__wt_config_gets_def(session, cfg, "bulk", 0, &cval));
		if (cval.type == WT_CONFIG_ITEM_BOOL ||
		    (cval.type == WT_CONFIG_ITEM_NUM &&
		    (cval.val == 0 || cval.val == 1))) {
			bitmap = false;
			bulk = cval.val != 0;
		} else if (WT_STRING_MATCH("bitmap", cval.str, cval.len))
			bitmap = bulk = true;
			/*
			 * Unordered bulk insert is a special case used
			 * internally by index creation on existing tables. It
			 * doesn't enforce any special semantics at the file
			 * level. It primarily exists to avoid some locking
			 * problems between LSM and index creation.
			 */
		else if (!WT_STRING_MATCH("unordered", cval.str, cval.len))
			WT_RET_MSG(session, EINVAL,
			    "Value for 'bulk' must be a boolean or 'bitmap'");

		if (bulk) {
			WT_RET(__wt_config_gets(session,
			    cfg, "checkpoint_wait", &cval));
			checkpoint_wait = cval.val != 0;
		}
	}

	/* Bulk handles require exclusive access. */
	if (bulk)
		LF_SET(WT_BTREE_BULK | WT_DHANDLE_EXCLUSIVE);

	/* Get the handle and lock it while the cursor is using it. */
	if (WT_PREFIX_MATCH(uri, "file:")) {
		/*
		 * If we are opening exclusive and don't want a bulk cursor
		 * open to fail with EBUSY due to a database-wide checkpoint,
		 * get the handle while holding the checkpoint lock.
		 */
		if (LF_ISSET(WT_DHANDLE_EXCLUSIVE) && checkpoint_wait)
			WT_WITH_CHECKPOINT_LOCK(session,
			    ret = __wt_session_get_btree_ckpt(
			    session, uri, cfg, flags));
		else
			ret = __wt_session_get_btree_ckpt(
			    session, uri, cfg, flags);
		WT_RET(ret);
	} else
		WT_RET(__wt_bad_object_type(session, uri));

	WT_ERR(__curfile_create(session, owner, cfg, bulk, bitmap, cursorp));

	return (0);

err:	/* If the cursor could not be opened, release the handle. */
	WT_TRET(__wt_session_release_btree(session));
	return (ret);
}
