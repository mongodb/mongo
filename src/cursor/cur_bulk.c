/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curbulk_insert --
 *	WT_CURSOR->insert for the bulk cursor type.
 */
static int
__curbulk_insert(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;
	/*
	 * Bulk cursor inserts are updates, but don't need auto-commit
	 * transactions because they are single-threaded and not visible until
	 * the bulk cursor is closed.
	 */
	CURSOR_API_CALL(cursor, session, insert, btree);
	if (btree->type == BTREE_ROW)
		WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);
	WT_ERR(__wt_bulk_insert(cbulk));

err:	API_END(session);
	return (ret);
}

/*
 * __curbulk_close --
 *	WT_CURSOR->close for the bulk cursor type.
 */
static int
__curbulk_close(WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	CURSOR_API_CALL(cursor, session, close, btree);
	WT_TRET(__wt_bulk_end(cbulk));
	if (btree != NULL)
		WT_TRET(__wt_session_release_btree(session));
	/* The URI is owned by the btree handle. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END(session);
	return (ret);
}

/*
 * __wt_curbulk_init --
 *	Initialize a bulk cursor.
 */
int
__wt_curbulk_init(WT_CURSOR_BULK *cbulk, int bitmap)
{
	WT_CURSOR *c = &cbulk->cbt.iface;

	/*
	 * Bulk cursors only support insert and close (reset is a no-op).
	 * This is slightly tricky because cursor.reset is called during
	 * checkpoints, which means checkpoints have to handle open bulk
	 * cursors.
	 */
	__wt_cursor_set_notsup(c);
	c->insert = __curbulk_insert;
	c->close = __curbulk_close;

	cbulk->bitmap = bitmap;
	if (bitmap)
		F_SET(c, WT_CURSTD_RAW);

	return (__wt_bulk_init(cbulk));
}
