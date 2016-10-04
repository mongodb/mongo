/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
__wt_kv_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_CURSOR *cursor;
	WT_ITEM *tmp;
	WT_PAGE *page;
	WT_ROW *rip;
	uint8_t v;

	btree = S2BT(session);

	page = cbt->ref->page;
	cursor = &cbt->iface;

	switch (page->type) {
	case WT_PAGE_COL_FIX:
		/*
		 * The interface cursor's record has usually been set, but that
		 * isn't universally true, specifically, cursor.search_near may
		 * call here without first setting the interface cursor.
		 */
		cursor->recno = cbt->recno;

		/* If the cursor references a WT_UPDATE item, return it. */
		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}

		/* Take the value from the original page. */
		v = __bit_getv_recno(cbt->ref, cursor->recno, btree->bitcnt);
		return (__wt_buf_set(session, &cursor->value, &v, 1));
	case WT_PAGE_COL_VAR:
		/*
		 * The interface cursor's record has usually been set, but that
		 * isn't universally true, specifically, cursor.search_near may
		 * call here without first setting the interface cursor.
		 */
		cursor->recno = cbt->recno;

		/* If the cursor references a WT_UPDATE item, return it. */
		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}

		/* Take the value from the original page cell. */
		cell = WT_COL_PTR(page, &page->pg_var_d[cbt->slot]);
		break;
	case WT_PAGE_ROW_LEAF:
		rip = &page->pg_row_d[cbt->slot];

		/*
		 * If the cursor references a WT_INSERT item, take its key.
		 * Else, if we have an exact match, we copied the key in the
		 * search function, take it from there.
		 * If we don't have an exact match, take the key from the
		 * original page.
		 */
		if (cbt->ins != NULL) {
			cursor->key.data = WT_INSERT_KEY(cbt->ins);
			cursor->key.size = WT_INSERT_KEY_SIZE(cbt->ins);
		} else if (cbt->compare == 0) {
			/*
			 * If not in an insert list and there's an exact match,
			 * the row-store search function built the key we want
			 * to return in the cursor's temporary buffer. Swap the
			 * cursor's search-key and temporary buffers so we can
			 * return it (it's unsafe to return the temporary buffer
			 * itself because our caller might do another search in
			 * this table using the key we return, and we'd corrupt
			 * the search key during any subsequent search that used
			 * the temporary buffer.
			 */
			tmp = cbt->row_key;
			cbt->row_key = cbt->tmp;
			cbt->tmp = tmp;

			cursor->key.data = cbt->row_key->data;
			cursor->key.size = cbt->row_key->size;
		} else
			WT_RET(__wt_row_leaf_key(
			    session, page, rip, &cursor->key, false));

		/* If the cursor references a WT_UPDATE item, return it. */
		if (upd != NULL) {
			cursor->value.data = WT_UPDATE_DATA(upd);
			cursor->value.size = upd->size;
			return (0);
		}

		/* Simple values have their location encoded in the WT_ROW. */
		if (__wt_row_leaf_value(page, rip, &cursor->value))
			return (0);

		/*
		 * Take the value from the original page cell (which may be
		 * empty).
		 */
		if ((cell =
		    __wt_row_leaf_value_cell(page, rip, NULL)) == NULL) {
			cursor->value.size = 0;
			return (0);
		}
		break;
	WT_ILLEGAL_VALUE(session);
	}

	/* The value is an on-page cell, unpack and expand it as necessary. */
	__wt_cell_unpack(cell, &unpack);
	WT_RET(__wt_page_cell_data_ref(session, page, &unpack, &cursor->value));

	return (0);
}
