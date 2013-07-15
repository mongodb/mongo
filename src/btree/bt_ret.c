/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
	WT_DECL_RET;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint8_t v;

	btree = S2BT(session);

	page = cbt->page;
	cursor = &cbt->iface;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * The interface cursor's record has usually been set, but that
		 * isn't universally true, specifically, cursor.search_near may
		 * call here without first setting the interface cursor.
		 */
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
		/*
		 * The interface cursor's record has usually been set, but that
		 * isn't universally true, specifically, cursor.search_near may
		 * call here without first setting the interface cursor.
		 */
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
			WT_RET(__wt_row_leaf_key(
			    session, page, rip, &cursor->key, 0));
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
	__wt_cell_unpack(cell, page->type, &unpack);
	ret =
	    __wt_cell_unpack_ref(session, page->type, &unpack, &cursor->value);

	/*
	 * Restart for a variable-length column-store.  We could catch restart
	 * higher up the call-stack but there's no point to it: unlike row-store
	 * (where a normal search path finds cached overflow values), we have to
	 * access the page's reconciliation structures, and that's as easy here
	 * as higher up the stack.
	 */
	if (ret == WT_RESTART && page->type == WT_PAGE_COL_VAR)
		ret = __wt_ovfl_cache_col_restart(
		    session, page, &unpack, &cursor->value);
	return (ret);
}
