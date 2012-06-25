/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
	CURSOR_API_CALL_NOCONF(cursor, session, insert, btree);
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

	CURSOR_API_CALL_NOCONF(cursor, session, close, btree);
	WT_TRET(__wt_bulk_end(cbulk));
	if (session->btree != NULL)
		WT_TRET(__wt_session_release_btree(session));
	/* The URI is owned by the btree handle. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));
	API_END(session);

	return (ret);
}

/*
 * __wt_curbulk_init --
 *	Initialize a bulk cursor.
 */
int
__wt_curbulk_init(WT_CURSOR_BULK *cbulk)
{
	WT_CURSOR *c = &cbulk->cbt.iface;

	/* Bulk cursors only support insert and close. */
	__wt_cursor_set_notsup(c);
	c->insert = __curbulk_insert;
	c->close = __curbulk_close;

	return (__wt_bulk_init(cbulk));
}
