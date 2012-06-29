/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_kv_return --
 *	Return a page referenced key/value pair to the application.
 */
int
__wt_kv_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_CURSOR *cursor;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint8_t v;

	btree = session->btree;

	page = cbt->page;
	cursor = &cbt->iface;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		cursor->recno = cbt->recno;

		/*
		 * If the cursor references a WT_INSERT item, take the related
		 * WT_UPDATE item.
		 */
		if (cbt->ins != NULL &&
		    (upd = __wt_txn_read(session, cbt->ins->upd)) != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}
		v = __bit_getv_recno(page, cbt->iface.recno, btree->bitcnt);
		return (__wt_buf_set(session, &cursor->value, &v, 1));
	case WT_PAGE_COL_VAR:
		cursor->recno = cbt->recno;

		/*
		 * If the cursor references a WT_INSERT item, take the related
		 * WT_UPDATE item.
		 */
		if (cbt->ins != NULL &&
		    (upd = __wt_txn_read(session, cbt->ins->upd)) != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}
		cell = WT_COL_PTR(page, &page->u.col_var.d[cbt->slot]);
		break;
	case WT_PAGE_ROW_LEAF:
		rip = &page->u.row.d[cbt->slot];

		/*
		 * If the cursor references a WT_INSERT item, take the key and
		 * related WT_UPDATE item.   Otherwise, take the key from the
		 * original page, and the value from any related WT_UPDATE item,
		 * or the page if the key was never updated.
		 */
		if (cbt->ins != NULL &&
		    (upd = __wt_txn_read(session, cbt->ins->upd)) != NULL) {
			cursor->key.data = WT_INSERT_KEY(cbt->ins);
			cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
		} else {
			WT_RET(
			    __wt_row_key(session, page, rip, &cursor->key, 0));
			upd = __wt_txn_read(session, WT_ROW_UPDATE(page, rip));
		}
		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}

		/* Take the original cell (which may be empty). */
		if ((cell = __wt_row_value(page, rip)) == NULL) {
			cursor->value.size = 0;
			return (0);
		}
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/* The value is an on-page cell, unpack and expand it as necessary. */
	__wt_cell_unpack(cell, &unpack);
	if (btree->huffman_value == NULL && unpack.type == WT_CELL_VALUE) {
		cursor->value.data = unpack.data;
		cursor->value.size = unpack.size;
		return (0);
	}
	return (__wt_cell_unpack_copy(session, &unpack, &cursor->value));
}
