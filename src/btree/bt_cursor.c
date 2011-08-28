/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_clear --
 *	Reset the cursor.
 */
void
__wt_cursor_clear(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (cbt->page != NULL) {
		__wt_page_release(session, cbt->page);
		cbt->page = NULL;
	}

	/* Reset the cursor iteration state. */
	F_CLR(cbt, WT_CBT_SEARCH_SET);
}

/*
 * __cursor_flags_begin --
 *	Cursor function initial flags handling.
 */
static inline void
__cursor_flags_begin(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/* Release any page references we're holding. */
	__wt_cursor_clear(cbt);

	/* Reset the key/value state. */
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
}

/*
 * __cursor_flags_end --
 *	Cursor function final flags handling.
 */
static inline void
__cursor_flags_end(WT_CURSOR_BTREE *cbt, int ret)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/*
	 * On success, we're returning a key/value pair, and can iterate.
	 * On error, release any page references we're holding.
	 */
	if (ret == 0) {
		F_SET(cbt, WT_CBT_SEARCH_SET);
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	} else
		__wt_cursor_clear(cbt);
}

/*
 * __cursor_deleted --
 *	Return if the item is currently deleted.
 */
static inline int
__cursor_deleted(WT_CURSOR_BTREE *cbt)
{
	WT_INSERT *ins;
	WT_PAGE *page;

	ins = cbt->ins;
	page = cbt->page;

	/*
	 * If we found an item on an insert list, check there, otherwise check
	 * an update in the page slots.
	 */
	if (ins != NULL) {
		if (ins->upd != NULL && WT_UPDATE_DELETED_ISSET(ins->upd))
			return (WT_NOTFOUND);
	} else
		if (page->u.row_leaf.upd != NULL &&
		    page->u.row_leaf.upd[cbt->slot] != NULL &&
		    WT_UPDATE_DELETED_ISSET(page->u.row_leaf.upd[cbt->slot]))
			return (WT_NOTFOUND);
	return (0);
}

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

	__cursor_flags_begin(cbt);

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
		while ((ret = __wt_row_search(session, cbt, 0)) == WT_RESTART)
			;
		if (ret == 0) {
			if (cbt->match == 0)
				ret = WT_NOTFOUND;
			else if ((ret = __cursor_deleted(cbt)) == 0)
				ret = __wt_xxxreturn_value(session, cbt);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__cursor_flags_end(cbt, ret);

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

	__cursor_flags_begin(cbt);

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
		while ((ret = __wt_row_search(session, cbt, 1)) == WT_RESTART)
			;
		if (ret == 0) {
			if (cbt->match == 1 &&
			    !F_ISSET(cursor, WT_CURSTD_OVERWRITE))
				ret = EINVAL;		/* XXX: WRONG ERROR? */
			else
				ret = __wt_row_modify(session, cbt, 0);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__cursor_flags_end(cbt, ret);

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

	__cursor_flags_begin(cbt);

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
		while ((ret = __wt_row_search(session, cbt, 1)) == WT_RESTART)
			;
		if (ret == 0) {
			if (cbt->match == 0)
				ret = WT_NOTFOUND;
			else if ((ret = __cursor_deleted(cbt)) == 0)
				ret  = __wt_row_modify(session, cbt, 1);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__cursor_flags_end(cbt, ret);

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

	__cursor_flags_begin(cbt);

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
		while ((ret = __wt_row_search(session, cbt, 1)) == WT_RESTART)
			;
		if (ret == 0) {
			if (cbt->match == 0)
				ret = WT_NOTFOUND;
			else if ((ret = __cursor_deleted(cbt)) == 0)
				ret  = __wt_row_modify(session, cbt, 0);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	__cursor_flags_end(cbt, ret);

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

	__wt_cursor_clear(cbt);

	__wt_buf_free(session, &cbt->value);

	return (0);
}
