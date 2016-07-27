/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int  __stat_page(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_col_var(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_row_int(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void
	__stat_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);

/*
 * __wt_btree_stat_init --
 *	Initialize the Btree statistics.
 */
int
__wt_btree_stat_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_DSRC_STATS **stats;
	WT_REF *next_walk;

	btree = S2BT(session);
	bm = btree->bm;
	stats = btree->dhandle->stats;

	WT_RET(bm->stat(bm, session, stats[0]));

	WT_STAT_SET(session, stats, btree_fixed_len, btree->bitcnt);
	WT_STAT_SET(session, stats, btree_maximum_depth, btree->maximum_depth);
	WT_STAT_SET(session, stats, btree_maxintlkey, btree->maxintlkey);
	WT_STAT_SET(session, stats, btree_maxintlpage, btree->maxintlpage);
	WT_STAT_SET(session, stats, btree_maxleafkey, btree->maxleafkey);
	WT_STAT_SET(session, stats, btree_maxleafpage, btree->maxleafpage);
	WT_STAT_SET(session, stats, btree_maxleafvalue, btree->maxleafvalue);

	/* Everything else is really, really expensive. */
	if (!F_ISSET(cst, WT_CONN_STAT_ALL))
		return (0);

	/*
	 * Clear the statistics we're about to count.
	 */
	WT_STAT_SET(session, stats, btree_column_deleted, 0);
	WT_STAT_SET(session, stats, btree_column_fix, 0);
	WT_STAT_SET(session, stats, btree_column_internal, 0);
	WT_STAT_SET(session, stats, btree_column_rle, 0);
	WT_STAT_SET(session, stats, btree_column_variable, 0);
	WT_STAT_SET(session, stats, btree_entries, 0);
	WT_STAT_SET(session, stats, btree_overflow, 0);
	WT_STAT_SET(session, stats, btree_row_internal, 0);
	WT_STAT_SET(session, stats, btree_row_leaf, 0);

	next_walk = NULL;
	while ((ret = __wt_tree_walk(
	    session, &next_walk, 0)) == 0 && next_walk != NULL) {
		WT_WITH_PAGE_INDEX(session,
		    ret = __stat_page(session, next_walk->page, stats));
		WT_RET(ret);
	}
	return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __stat_page --
 *	Stat any Btree page.
 */
static int
__stat_page(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(session, stats, btree_column_fix);
		WT_STAT_INCRV(
		    session, stats, btree_entries, page->pg_fix_entries);
		break;
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(session, stats, btree_column_internal);
		break;
	case WT_PAGE_COL_VAR:
		__stat_page_col_var(session, page, stats);
		break;
	case WT_PAGE_ROW_INT:
		__stat_page_row_int(session, page, stats);
		break;
	case WT_PAGE_ROW_LEAF:
		__stat_page_row_leaf(session, page, stats);
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * __stat_page_col_var --
 *	Stat a WT_PAGE_COL_VAR page.
 */
static void
__stat_page_col_var(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_UPDATE *upd;
	uint64_t deleted_cnt, entry_cnt, ovfl_cnt, rle_cnt;
	uint32_t i;
	bool orig_deleted;

	unpack = &_unpack;
	deleted_cnt = entry_cnt = ovfl_cnt = rle_cnt = 0;

	WT_STAT_INCR(session, stats, btree_column_variable);

	/*
	 * Walk the page counting regular items, adjusting if the item has been
	 * subsequently deleted or not. This is a mess because 10-item RLE might
	 * have 3 of the items subsequently deleted. Overflow items are harder,
	 * we can't know if an updated item will be an overflow item or not; do
	 * our best, and simply count every overflow item (or RLE set of items)
	 * we see.
	 */
	WT_COL_FOREACH(page, cip, i) {
		if ((cell = WT_COL_PTR(page, cip)) == NULL) {
			orig_deleted = true;
			++deleted_cnt;
		} else {
			orig_deleted = false;
			__wt_cell_unpack(cell, unpack);
			if (unpack->type == WT_CELL_ADDR_DEL)
				orig_deleted = true;
			else {
				entry_cnt += __wt_cell_rle(unpack);
				rle_cnt += __wt_cell_rle(unpack) - 1;
			}
			if (unpack->ovfl)
				++ovfl_cnt;
		}

		/*
		 * Walk the insert list, checking for changes.  For each insert
		 * we find, correct the original count based on its state.
		 */
		WT_SKIP_FOREACH(ins, WT_COL_UPDATE(page, cip)) {
			upd = ins->upd;
			if (WT_UPDATE_DELETED_ISSET(upd)) {
				if (!orig_deleted) {
					++deleted_cnt;
					--entry_cnt;
				}
			} else
				if (orig_deleted) {
					--deleted_cnt;
					++entry_cnt;
				}
		}
	}

	/* Walk any append list. */
	WT_SKIP_FOREACH(ins, WT_COL_APPEND(page))
		if (WT_UPDATE_DELETED_ISSET(ins->upd))
			++deleted_cnt;
		else
			++entry_cnt;

	WT_STAT_INCRV(session, stats, btree_column_deleted, deleted_cnt);
	WT_STAT_INCRV(session, stats, btree_column_rle, rle_cnt);
	WT_STAT_INCRV(session, stats, btree_entries, entry_cnt);
	WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
}

/*
 * __stat_page_row_int --
 *	Stat a WT_PAGE_ROW_INT page.
 */
static void
__stat_page_row_int(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	uint32_t i, ovfl_cnt;

	btree = S2BT(session);
	ovfl_cnt = 0;

	WT_STAT_INCR(session, stats, btree_row_internal);

	/*
	 * Overflow keys are hard: we have to walk the disk image to count them,
	 * the in-memory representation of the page doesn't necessarily contain
	 * a reference to the original cell.
	 */
	if (page->dsk != NULL)
		WT_CELL_FOREACH(btree, page->dsk, cell, &unpack, i) {
			__wt_cell_unpack(cell, &unpack);
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
				++ovfl_cnt;
		}

	WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
}

/*
 * __stat_page_row_leaf --
 *	Stat a WT_PAGE_ROW_LEAF page.
 */
static void
__stat_page_row_leaf(
    WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_INSERT *ins;
	WT_ROW *rip;
	WT_UPDATE *upd;
	uint32_t entry_cnt, i, ovfl_cnt;

	btree = S2BT(session);
	entry_cnt = ovfl_cnt = 0;

	WT_STAT_INCR(session, stats, btree_row_leaf);

	/*
	 * Walk any K/V pairs inserted into the page before the first from-disk
	 * key on the page.
	 */
	WT_SKIP_FOREACH(ins, WT_ROW_INSERT_SMALLEST(page))
		if (!WT_UPDATE_DELETED_ISSET(ins->upd))
			++entry_cnt;

	/*
	 * Walk the page's K/V pairs. Count overflow values, where an overflow
	 * item is any on-disk overflow item that hasn't been updated.
	 */
	WT_ROW_FOREACH(page, rip, i) {
		upd = WT_ROW_UPDATE(page, rip);
		if (upd == NULL || !WT_UPDATE_DELETED_ISSET(upd))
			++entry_cnt;
		if (upd == NULL && (cell =
		    __wt_row_leaf_value_cell(page, rip, NULL)) != NULL &&
		    __wt_cell_type(cell) == WT_CELL_VALUE_OVFL)
				++ovfl_cnt;

		/* Walk K/V pairs inserted after the on-page K/V pair. */
		WT_SKIP_FOREACH(ins, WT_ROW_INSERT(page, rip))
			if (!WT_UPDATE_DELETED_ISSET(ins->upd))
				++entry_cnt;
	}

	/*
	 * Overflow keys are hard: we have to walk the disk image to count them,
	 * the in-memory representation of the page doesn't necessarily contain
	 * a reference to the original cell.
	 */
	if (page->dsk != NULL)
		WT_CELL_FOREACH(btree, page->dsk, cell, &unpack, i) {
			__wt_cell_unpack(cell, &unpack);
			if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL)
				++ovfl_cnt;
		}

	WT_STAT_INCRV(session, stats, btree_entries, entry_cnt);
	WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
}
