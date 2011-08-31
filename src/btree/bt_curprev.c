/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __search_insert --
 *	Search an insert list.
 */
static inline WT_INSERT *
__search_insert(WT_INSERT_HEAD *inshead, uint64_t recno)
{
	WT_INSERT **ins;
	uint64_t ins_recno;
	int cmp, i;

	/* If there's no insert chain to search, we're done. */
	if (inshead == NULL)
		return (NULL);

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL)
			cmp = -1;
		else {
			ins_recno = WT_INSERT_RECNO(*ins);
			cmp = (recno == ins_recno) ? 0 :
			    (recno < ins_recno) ? -1 : 1;
		}
		if (cmp == 0)			/* Exact match: return */
			return (*ins);
		else if (cmp > 0)		/* Keep going at this level */
			ins = &(*ins)->next[i];
		else {				/* Drop down a level */
			--i;
			--ins;
		}
	}

	return (NULL);
}

/*
 * __cursor_fix_prev --
 *	Move to the previous, fixed-length column-store item.
 */
static inline int
__cursor_fix_prev(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BTREE *btree;
	WT_BUF *val;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	uint64_t *recnop;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	btree = session->btree;

	recnop = &cbt->iface.recno;
	val = &cbt->iface.value;

	/*
	 * Reset the insert list reference so any subsequent cursor next
	 * works correctly.
	 */
	cbt->ins = NULL;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->recno =
		    cbt->page->u.col_leaf.recno + (cbt->page->entries - 1);
		goto new_page;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		if (cbt->recno == cbt->page->u.col_leaf.recno)
			return (WT_NOTFOUND);
		--cbt->recno;
new_page:	*recnop = cbt->recno;

		/*
		 * Check any insert list for a matching record.  Insert lists
		 * are in forward sorted order, in a last-to-first walk we have
		 * to search the entire list.  We use the skiplist structure,
		 * rather than doing it linearly.
		 */
		if ((ins = __search_insert(
		    WT_COL_INSERT_SINGLE(cbt->page), cbt->recno)) != NULL) {
			val->data = WT_UPDATE_DATA(ins->upd);
			val->size = 1;
			return (0);
		}

		cbt->v = __bit_getv_recno(cbt->page, cbt->recno, btree->bitcnt);
		val->data = &cbt->v;
		val->size = 1;
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __cursor_var_prev --
 *	Move to the previous, variable-length column-store item.
 */
static inline int
__cursor_var_prev(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BUF *val;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_SESSION_IMPL *session;
	uint64_t *recnop;
	uint32_t slot;

	session = (WT_SESSION_IMPL *)cbt->iface.session;
	recnop = &cbt->iface.recno;
	val = &cbt->iface.value;

	/*
	 * Reset the insert list reference so any subsequent cursor next
	 * works correctly.
	 */
	cbt->ins = NULL;

	/* Initialize for each new page. */
	if (newpage) {
		cbt->recno = __cursor_col_rle_last(cbt->page);
		cbt->vslot = UINT32_MAX;
		goto new_page;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		if (cbt->recno == cbt->page->u.col_leaf.recno)
			return (WT_NOTFOUND);
		--cbt->recno;
new_page:	*recnop = cbt->recno;

		/* Find the matching WT_COL slot. */
		if ((cip =
		    __cursor_col_rle_search(cbt->page, cbt->recno)) == NULL)
			return (WT_NOTFOUND);
		slot = WT_COL_SLOT(cbt->page, cip);

		/*
		 * Check any insert list for a matching record.  Insert lists
		 * are in forward sorted order, in a last-to-first walk we have
		 * to search the entire list.  We use the skiplist structure,
		 * rather than doing it linearly.
		 */
		if ((ins = __search_insert(
		    WT_COL_INSERT(cbt->page, cip), cbt->recno)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(ins->upd))
				continue;
			val->data = WT_UPDATE_DATA(ins->upd);
			val->size = ins->upd->size;
			return (0);
		}

		/*
		 * If we're at the same slot as the last reference and there's
		 * no matching insert list item, re-use the return information.
		 * Otherwise, unpack the cell and build the return information.
		 */
		if (slot != cbt->vslot) {
			if ((cell = WT_COL_PTR(cbt->page, cip)) == NULL)
				continue;
			__wt_cell_unpack(cell, &unpack);
			switch (unpack.type) {
			case WT_CELL_DEL:
				continue;
			case WT_CELL_VALUE:
				if (session->btree->huffman_value == NULL) {
					cbt->value.data = unpack.data;
					cbt->value.size = unpack.size;
					break;
				}
				/* FALLTHROUGH */
			default:
				WT_RET(__wt_cell_unpack_copy(
				    session, &unpack, &cbt->value));
			}
			cbt->vslot = slot;
		}
		val->data = cbt->value.data;
		val->size = cbt->value.size;
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __cursor_row_prev --
 *	Move to the previous row-store item.
 */
static inline int
__cursor_row_prev(WT_CURSOR_BTREE *cbt, int newpage)
{
	WT_BUF *key, *val;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t i;

	key = &cbt->iface.key;
	val = &cbt->iface.value;

	/*
	 * For row-store pages, we need a single item that tells us the part
	 * of the page we're walking (otherwise switching from next to prev
	 * and vice-versa is just too complicated), so we map the WT_ROW and
	 * WT_INSERT_HEAD array slots into a single name space: slot 1 is the
	 * "smallest key insert list", slot 2 is WT_ROW[0], slot 3 is
	 * WT_INSERT_HEAD[0], and so on.  This means WT_INSERT lists are
	 * odd-numbered slots, and WT_ROW array slots are even-numbered slots.
	 *
	 * New page configuration.
	 */
	if (newpage) {
		cbt->ins_head =
		    WT_ROW_INSERT_SLOT(cbt->page, cbt->page->entries - 1);
		cbt->ins_entry_cnt = 0;
		WT_SKIP_FOREACH(ins, cbt->ins_head)
			++cbt->ins_entry_cnt;
		cbt->slot = cbt->page->entries * 2 + 1;
		goto new_insert;
	}

	/* Move to the previous entry and return the item. */
	for (;;) {
		/*
		 * Continue traversing any insert list.  Insert lists are in
		 * forward sorted order; in a last-to-first walk we have walk
		 * the list from the end to the beginning.  Maintain the
		 * reference to the current insert element in case we switch
		 * to a cursor next movement.
		 */
		if (cbt->ins_head != NULL && cbt->ins_entry_cnt > 0)
			--cbt->ins_entry_cnt;

new_insert:	if (cbt->ins_head != NULL && cbt->ins_entry_cnt > 0) {
			for (i = cbt->ins_entry_cnt,
			    ins = WT_SKIP_FIRST(cbt->ins_head); i > 1; --i)
				ins = WT_SKIP_NEXT(ins);
			cbt->ins = ins;

			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd))
				continue;
			key->data = WT_INSERT_KEY(ins);
			key->size = WT_INSERT_KEY_SIZE(ins);
			val->data = WT_UPDATE_DATA(upd);
			val->size = upd->size;
			return (0);
		}

		/* Check for the beginning of the page. */
		if (cbt->slot == 1)
			return (WT_NOTFOUND);
		--cbt->slot;

		/*
		 * Odd-numbered slots configure as WT_INSERT_HEAD entries,
		 * even-numbered slots configure as WT_ROW entries.
		 */
		if (cbt->slot & 0x01) {
			cbt->ins_head = cbt->slot == 1 ?
			    WT_ROW_INSERT_SMALLEST(cbt->page) :
			    WT_ROW_INSERT_SLOT(cbt->page, cbt->slot / 2 - 1);
			cbt->ins_entry_cnt = 0;
			WT_SKIP_FOREACH(ins, cbt->ins_head)
				++cbt->ins_entry_cnt;
			goto new_insert;
		}
		cbt->ins_head = NULL;
		cbt->ins = NULL;

		rip = &cbt->page->u.row_leaf.d[cbt->slot / 2 - 1];
		upd = WT_ROW_UPDATE(cbt->page, rip);
		if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
			continue;

		return (__cursor_row_slot_return(cbt, rip));
	}
	/* NOTREACHED */
}

/*
 * __wt_btcur_last --
 *	Move to the last record in the tree.
 */
int
__wt_btcur_last(WT_CURSOR_BTREE *cbt)
{
	__wt_cursor_clear(cbt);

	return (__wt_btcur_prev(cbt));
}

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int newpage, ret;

	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_readprev);

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	/* If iterating from a search position, there's some setup to do. */
	if (F_ISSET(cbt, WT_CBT_SEARCH_SET))
		WT_RET(__wt_btcur_search_setup(cbt));

	/*
	 * Walk any page we're holding until the underlying call returns not-
	 * found.  Then, move to the previous page, until we reach the start
	 * of the file.
	 */
	for (newpage = 0;; newpage = 1) {
		if (cbt->page != NULL) {
			switch (cbt->page->type) {
			case WT_PAGE_COL_FIX:
				ret = __cursor_fix_prev(cbt, newpage);
				break;
			case WT_PAGE_COL_VAR:
				ret = __cursor_var_prev(cbt, newpage);
				break;
			case WT_PAGE_ROW_LEAF:
				ret = __cursor_row_prev(cbt, newpage);
				break;
			WT_ILLEGAL_FORMAT_ERR(session);
			}
			if (ret != WT_NOTFOUND)
				break;
		}

		do {
			WT_ERR(__wt_tree_np(session, &cbt->page, 0));
			WT_ERR_TEST(cbt->page == NULL, WT_NOTFOUND);
		} while (
		    cbt->page->type == WT_PAGE_COL_INT ||
		    cbt->page->type == WT_PAGE_ROW_INT);
	}

err:	if (ret != 0)
		return (ret);

	F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	return (0);
}
