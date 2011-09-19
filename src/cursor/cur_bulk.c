/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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
	WT_SESSION_IMPL *session;
	int ret;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;
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
__curbulk_close(WT_CURSOR *cursor, const char *config)
{
	WT_BTREE *btree;
	WT_CURSOR_BULK *cbulk;
	WT_SESSION_IMPL *session;
	int ret;

	cbulk = (WT_CURSOR_BULK *)cursor;
	btree = cbulk->cbt.btree;

	CURSOR_API_CALL_CONF(cursor, session, close, btree, config, cfg);
	WT_UNUSED(cfg);
	WT_TRET(__wt_bulk_end(cbulk));
	if (session->btree != NULL)
		WT_TRET(__wt_session_release_btree(session));
	WT_TRET(__wt_cursor_close(cursor, config));
err:	API_END(session);

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

	c->insert = __curbulk_insert;
	c->close = __curbulk_close;

	return (__wt_bulk_init(cbulk));
}
