/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __key_return --
 *	Change the cursor to reference an internal return key.
 */
static inline int
__key_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_ITEM *tmp;
	WT_PAGE *page;
	WT_ROW *rip;

	page = cbt->ref->page;
	cursor = &cbt->iface;

	if (page->type == WT_PAGE_ROW_LEAF) {
		rip = &page->pg_row[cbt->slot];

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
			return (0);
		}

		if (cbt->compare == 0) {
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
			return (0);
		}
		return (__wt_row_leaf_key(
		    session, page, rip, &cursor->key, false));
	}

	/*
	 * WT_PAGE_COL_FIX, WT_PAGE_COL_VAR:
	 *	The interface cursor's record has usually been set, but that
	 * isn't universally true, specifically, cursor.search_near may call
	 * here without first setting the interface cursor.
	 */
	cursor->recno = cbt->recno;
	return (0);
}

/*
 * __value_return --
 *	Change the cursor to reference an internal return value.
 */
static inline int
__value_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_CURSOR *cursor;
	WT_PAGE *page;
	WT_ROW *rip;
	uint8_t v;

	btree = S2BT(session);

	page = cbt->ref->page;
	cursor = &cbt->iface;

	/* If the cursor references a WT_UPDATE item, return it. */
	if (upd != NULL) {
		cursor->value.data = WT_UPDATE_DATA(upd);
		cursor->value.size = upd->size;
		return (0);
	}

	if (page->type == WT_PAGE_ROW_LEAF) {
		rip = &page->pg_row[cbt->slot];

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
		__wt_cell_unpack(cell, &unpack);
		return (__wt_page_cell_data_ref(
		    session, page, &unpack, &cursor->value));

	}

	if (page->type == WT_PAGE_COL_VAR) {
		/* Take the value from the original page cell. */
		cell = WT_COL_PTR(page, &page->pg_var[cbt->slot]);
		__wt_cell_unpack(cell, &unpack);
		return (__wt_page_cell_data_ref(
		    session, page, &unpack, &cursor->value));
	}

	/* WT_PAGE_COL_FIX: Take the value from the original page. */
	v = __bit_getv_recno(cbt->ref, cursor->recno, btree->bitcnt);
	return (__wt_buf_set(session, &cursor->value, &v, 1));
}

/*
 * __wt_key_return --
 *	Change the cursor to reference an internal return key.
 */
int
__wt_key_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/*
	 * We may already have an internal key and the cursor may not be set up
	 * to get another copy, so we have to leave it alone. Consider a cursor
	 * search followed by an update: the update doesn't repeat the search,
	 * it simply updates the currently referenced key's value. We will end
	 * up here with the correct internal key, but we can't "return" the key
	 * again even if we wanted to do the additional work, the cursor isn't
	 * set up for that because we didn't just complete a search.
	 */
	F_CLR(cursor, WT_CURSTD_KEY_EXT);
	if (!F_ISSET(cursor, WT_CURSTD_KEY_INT)) {
		WT_RET(__key_return(session, cbt));
		F_SET(cursor, WT_CURSTD_KEY_INT);
	}
	return (0);
}

/*
 * __wt_kv_return --
 *	Return a page referenced key/value pair to the application.
 */
int
__wt_kv_return(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_UPDATE *upd)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	WT_RET(__wt_key_return(session, cbt));

	F_CLR(cursor, WT_CURSTD_VALUE_EXT);
	WT_RET(__value_return(session, cbt, upd));
	F_SET(cursor, WT_CURSTD_VALUE_INT);

	return (0);
}
