/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact)
{
	WT_BTREE *btree;
	WT_SESSION_IMPL *session;
	WT_CURSOR *cursor;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, file_reads);

	*exact = 0;
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		while ((ret = __wt_col_search(
		    session, cursor->recno, 0)) == WT_RESTART)
			;
		if (ret == 0) {
			ret = __wt_return_value(session, cursor);
			__wt_page_release(session, session->srch.page);
		}
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_search(
		    session, (WT_ITEM *)&cursor->key, 0)) == WT_RESTART)
			;
		if (ret == 0) {
			ret = __wt_return_value(session, cursor);
			__wt_page_release(session, session->srch.page);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	if (ret == 0)
		F_SET(cursor, WT_CURSTD_VALUE_SET);

	return (ret);
}

/*
 * __wt_btcur_insert --
 *	Insert a record into the tree.
 */
int
__wt_btcur_insert(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, file_inserts);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(session,
		    cursor->recno, (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(session,
		    (WT_ITEM *)&cursor->key,
		    (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, file_updates);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(session,
		    cursor->recno, (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(session,
		    (WT_ITEM *)&cursor->key,
		    (WT_ITEM *)&cursor->value, 1)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_ITEM zero;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, file_removes);

	switch (btree->type) {
	case BTREE_COL_FIX:
		zero.data = "";		/* A zero-byte means delete. */
		zero.size = 1;
		while ((ret = __wt_col_modify(
		    session, cursor->recno, &zero, 0)) == WT_RESTART)
			;
		break;
	case BTREE_COL_VAR:
		while ((ret = __wt_col_modify(
		    session, cursor->recno, NULL, 0)) == WT_RESTART)
			;
		break;
	case BTREE_ROW:
		while ((ret = __wt_row_modify(
		    session, (WT_ITEM *)&cursor->key, NULL, 0)) == WT_RESTART)
			;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt, const char *config)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(config);

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	__wt_walk_end(session, &cbt->walk);
	__wt_buf_free(session, &cbt->value);
	return (0);
}
