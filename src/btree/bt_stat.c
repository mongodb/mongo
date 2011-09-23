/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int  __stat_page(WT_SESSION_IMPL *, WT_PAGE *);
static int  __stat_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static int  __stat_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);

/*
 * __wt_btree_stat_init --
 *	Initialize the Btree statistics.
 */
int
__wt_btree_stat_init(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	int ret;

	btree = session->btree;

	__wt_block_stat(session);

	WT_BSTAT_SET(session, file_allocsize, btree->allocsize);
	WT_BSTAT_SET(session, file_fixed_len, btree->bitcnt);
	WT_BSTAT_SET(session, file_intlmax, btree->intlmax);
	WT_BSTAT_SET(session, file_intlovfl, btree->intlovfl);
	WT_BSTAT_SET(session, file_leafmax, btree->leafmax);
	WT_BSTAT_SET(session, file_leafovfl, btree->leafovfl);
	WT_BSTAT_SET(session, file_magic, WT_BTREE_MAGIC);
	WT_BSTAT_SET(session, file_major, WT_BTREE_MAJOR_VERSION);
	WT_BSTAT_SET(session, file_minor, WT_BTREE_MINOR_VERSION);
	WT_BSTAT_SET(session, file_size, btree->fh->file_size);

	page = NULL;
	while ((ret = __wt_tree_np(session, &page, 0, 1)) == 0 && page != NULL)
		WT_RET(__stat_page(session, page));
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __stat_page --
 *	Stat any Btree page.
 */
static int
__stat_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_BSTAT_INCR(session, file_col_fix_pages);
		WT_BSTAT_INCRV(session, file_entries, page->entries);
		break;
	case WT_PAGE_COL_INT:
		WT_BSTAT_INCR(session, file_col_int_pages);
		WT_BSTAT_INCRV(session, file_entries, page->entries);
		break;
	case WT_PAGE_COL_VAR:
		WT_RET(__stat_page_col_var(session, page));
		break;
	case WT_PAGE_OVFL:
		WT_BSTAT_INCR(session, file_overflow);
		break;
	case WT_PAGE_ROW_INT:
		WT_BSTAT_INCR(session, file_row_int_pages);
		WT_BSTAT_INCRV(session, file_entries, page->entries);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_RET(__stat_page_row_leaf(session, page));
		break;
	WT_ILLEGAL_FORMAT(session);
	}
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

	WT_BSTAT_INCR(session, file_col_var_pages);

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
			WT_BSTAT_INCR(session, file_col_deleted);
		} else {
			__wt_cell_unpack(cell, unpack);

			orig_deleted = 0;
			WT_BSTAT_INCRV(session, file_entries, unpack->rle);
		}

		/*
		 * Walk the insert list, checking for changes.  For each insert
		 * we find, correct the original count based on its state.
		 */
		WT_SKIP_FOREACH(ins, WT_COL_UPDATE(page, cip)) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (orig_deleted)
					continue;
				WT_BSTAT_INCR(session, file_col_deleted);
				WT_BSTAT_DECR(session, file_entries);
			} else {
				if (!orig_deleted)
					continue;
				WT_BSTAT_DECR(session, file_col_deleted);
				WT_BSTAT_INCR(session, file_entries);
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
__stat_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t cnt, i;

	WT_BSTAT_INCR(session, file_row_leaf_pages);

	/*
	 * Stat any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	cnt = 0;
	WT_SKIP_FOREACH(ins, WT_ROW_INSERT_SMALLEST(page))
		if (!WT_UPDATE_DELETED_ISSET(ins->upd))
			++cnt;

	/* Stat the page's K/V pairs. */
	WT_ROW_FOREACH(page, rip, i) {
		upd = WT_ROW_UPDATE(page, rip);
		if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
			++cnt;

		/* Stat inserted K/V pairs. */
		WT_SKIP_FOREACH(ins, WT_ROW_INSERT(page, rip))
			if (!WT_UPDATE_DELETED_ISSET(ins->upd))
				++cnt;
	}

	WT_BSTAT_INCRV(session, file_entries, cnt);

	return (0);
}
