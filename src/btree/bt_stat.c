/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __stat_page(WT_SESSION_IMPL *, WT_PAGE *, void *);
static int __stat_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static int __stat_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static int __stat_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, void *);

/*
 * __wt_btree_stat_init --
 *	Initialize the Btree statistics.
 */
int
__wt_btree_stat_init(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_BSTAT_SET(session, file_allocsize, btree->allocsize);
	WT_BSTAT_SET(session, file_fixed_len, btree->bitcnt);
	WT_BSTAT_SET(session, file_freelist_entries, btree->freelist_entries);
	WT_BSTAT_SET(session, file_intlmax, btree->intlmax);
	WT_BSTAT_SET(session, file_intlmin, btree->intlmin);
	WT_BSTAT_SET(session, file_leafmax, btree->leafmax);
	WT_BSTAT_SET(session, file_leafmin, btree->leafmin);
	WT_BSTAT_SET(session, file_magic, WT_BTREE_MAGIC);
	WT_BSTAT_SET(session, file_major, WT_BTREE_MAJOR_VERSION);
	WT_BSTAT_SET(session, file_minor, WT_BTREE_MINOR_VERSION);

	WT_RET(__wt_tree_walk(session, NULL, __stat_page, NULL));

	return (0);
}

/*
 * __stat_page --
 *	Stat any Btree page.
 */
static int
__stat_page(WT_SESSION_IMPL *session, WT_PAGE *page, void *arg)
{
	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_BSTAT_INCR(session, file_col_fix);
		__stat_page_col_fix(session, page);
		break;
	case WT_PAGE_COL_INT:
		WT_BSTAT_INCR(session, file_col_internal);
		break;
	case WT_PAGE_COL_VAR:
		WT_BSTAT_INCR(session, file_col_variable);
		WT_RET(__stat_page_col_var(session, page));
		break;
	case WT_PAGE_OVFL:
		WT_BSTAT_INCR(session, file_overflow);
		break;
	case WT_PAGE_ROW_INT:
		WT_BSTAT_INCR(session, file_row_internal);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_BSTAT_INCR(session, file_row_leaf);
		WT_RET(__stat_page_row_leaf(session, page, arg));
		break;
	WT_ILLEGAL_FORMAT(session);
	}
	return (0);
}

/*
 * __stat_page_col_fix --
 *	Stat a WT_PAGE_COL_FIX page.
 */
static int
__stat_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_BSTAT_INCRV(session, file_item_total_value, page->entries);
	return (0);
}

/*
 * __stat_page_col_var --
 *	Stat a WT_PAGE_COL_VAR page.
 */
static int
__stat_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint32_t i;
	int orig_deleted;

	unpack = &_unpack;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any updates weren't deletions.  If the item was updated,
	 * assume it was updated by an item of the same size (it's expensive to
	 * figure out if it will require the same space or not, especially if
	 * there's Huffman encoding).
	 */
	WT_COL_FOREACH(page, cip, i) {
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			orig_deleted = 1;
			WT_BSTAT_INCR(session, file_item_col_deleted);
		} else {
			__wt_cell_unpack(cell, unpack);

			orig_deleted = 0;
			WT_BSTAT_INCRV(
			    session, file_item_total_value, unpack->rle);
		}

		/*
		 * Walk the insert list, checking for changes.  For each insert
		 * we find, correct the original count based on its state.
		 */
		for (ins =
		    WT_COL_INSERT(page, cip); ins != NULL; ins = ins->next) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (orig_deleted)
					continue;
				WT_BSTAT_INCR(session, file_item_col_deleted);
				WT_BSTAT_DECR(session, file_item_total_value);
			} else {
				if (!orig_deleted)
					continue;
				WT_BSTAT_DECR(session, file_item_col_deleted);
				WT_BSTAT_INCR(session, file_item_total_value);
			}
		}
	}
	return (0);
}

/*
 * __stat_page_row_leaf --
 *	Stat a WT_PAGE_ROW_LEAF page.
 */
static int
__stat_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, void *arg)
{
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t cnt, i;

	WT_UNUSED(arg);

	/*
	 * Stat any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	cnt = 0;
	for (ins = WT_ROW_INSERT_SMALLEST(page); ins != NULL; ins = ins->next)
		if (!WT_UPDATE_DELETED_ISSET(ins->upd))
			++cnt;

	/* Stat the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		upd = WT_ROW_UPDATE(page, rip);
		if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
			++cnt;

		/* Stat inserted K/V pairs. */
		for (ins =
		    WT_ROW_INSERT(page, rip); ins != NULL; ins = ins->next)
			if (!WT_UPDATE_DELETED_ISSET(ins->upd))
				++cnt;
	}

	WT_BSTAT_INCRV(session, file_item_total_key, cnt);
	WT_BSTAT_INCRV(session, file_item_total_value, cnt);

	return (0);
}
