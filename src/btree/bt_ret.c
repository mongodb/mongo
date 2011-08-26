/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_return_value --
 *	Return a page referenced value item to the application.
 */
int
__wt_return_value(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_UPDATE *upd;

	page = session->srch.page;
	cip = session->srch.ip;
	rip = session->srch.ip;
	upd = session->srch.vupdate;

	btree = session->btree;
	unpack = &_unpack;

	/* If the item was ever updated, take the last update. */
	if (upd != NULL)
		return (__wt_buf_set(
		    session, &cursor->value, WT_UPDATE_DATA(upd), upd->size));

	/* Else, take the item from the original page. */
	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		if ((cell = __wt_row_value(page, rip)) == NULL) {
			cursor->value.size = 0;
			return (0);
		}
		break;
	case WT_PAGE_COL_FIX:
		return (__wt_buf_set(
		    session, &cursor->value, &session->srch.v, 1));
	case WT_PAGE_COL_VAR:
		cell = WT_COL_PTR(page, cip);
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* It's a cell, unpack and expand it as necessary. */
	__wt_cell_unpack(cell, unpack);
	if (btree->huffman_value == NULL && unpack->type == WT_CELL_VALUE)
		return (__wt_buf_set(
		    session, &cursor->value, unpack->data, unpack->size));
	else
		return (__wt_cell_unpack_copy(session, unpack, &cursor->value));
}

/*
 * __wt_xxxreturn_value --
 *	Return a page referenced value item to the application.
 */
int
__wt_xxxreturn_value(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_CURSOR *cursor;
	WT_PAGE *page;
	WT_UPDATE *upd;

	btree = session->btree;
	unpack = &_unpack;

	page = cbt->page;
	cursor = &cbt->iface;

	switch (page->type) {
	case WT_PAGE_ROW_LEAF:
		/*
		 * If the cursor references a WT_INSERT item, or if the original
		 * item was updated, take the related WT_UPDATE item.
		 */
		if (cbt->ins == NULL)
			upd = WT_ROW_UPDATE(page, cbt->rip);
		else
			upd = cbt->ins->upd;
		if (upd != NULL)
			return (__wt_buf_set(session,
			    &cursor->value, WT_UPDATE_DATA(upd), upd->size));

		/* Otherwise, take the original cell (which may be empty). */
		if ((cell = __wt_row_value(page, cbt->rip)) == NULL) {
			cursor->value.size = 0;
			return (0);
		}
		break;
	case WT_PAGE_COL_FIX:
		WT_FAILURE(session, "xxreturn_value: PAGE_COL_FIX");
	case WT_PAGE_COL_VAR:
		WT_FAILURE(session, "xxreturn_value: PAGE_COL_VAR");
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* It's a cell, unpack and expand it as necessary. */
	__wt_cell_unpack(cell, unpack);
	if (btree->huffman_value == NULL && unpack->type == WT_CELL_VALUE)
		return (__wt_buf_set(
		    session, &cursor->value, unpack->data, unpack->size));
	else
		return (__wt_cell_unpack_copy(session, unpack, &cursor->value));
}
