/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_stat_page_col_fix(WT_TOC *, WT_PAGE *);
static int __wt_bt_stat_page_col_rcc(WT_TOC *, WT_PAGE *);
static int __wt_bt_stat_page_col_var(WT_TOC *, WT_PAGE *);
static int __wt_bt_stat_page_dup_leaf(WT_TOC *, WT_PAGE *);
static int __wt_bt_stat_page_row_leaf(WT_TOC *, WT_PAGE *, void *);

/*
 * __wt_bt_tree_walk --
 *	Depth-first recursive walk of a btree, calling a worker function on
 *	each page.
 */
int
__wt_bt_tree_walk(WT_TOC *toc, uint32_t addr,
    uint32_t size, int (*work)(WT_TOC *, WT_PAGE *, void *), void *arg)
{
	DB *db;
	WT_COL *cip;
	WT_PAGE *page;
	WT_REPL *repl;
	WT_ROW *rip;
	uint32_t i;
	int ret;

	db = toc->db;
	page = NULL;
	ret = 0;

	/*
	 * If we can't get the page, including if WT_RESTART is returned, fail
	 * because we don't know the source of the addr/size pair; our caller
	 * will have to deal with it.  (In almost all cases, however, our caller
	 * is a previous incarnation of this function -- which means our caller
	 * has a pinned page it's reading through.)
	 */
	WT_ERR(__wt_bt_page_in(toc, addr, size, 1, &page));

	/* Call the worker function for the page. */
	WT_ERR(work(toc, page, arg));

	/*
	 * Walk any internal pages, descending through any off-page reference
	 * in the local tree (that is, NOT including off-page duplicate trees).
	 * We could handle off-page duplicate trees by walking the page in this
	 * function, but that would be slower than recursively calling this
	 * function from the worker function, which is already walking the page.
	 *
	 * If the page were to be rewritten/discarded from the cache while
	 * we're getting it, we can re-try -- re-trying is safe because our
	 * addr/size information is from a page which can't be discarded
	 * because of our hazard reference.  If the page was re-written, our
	 * on-page overflow information will have been updated to the overflow
	 * page's new address.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_COL_INT:
		WT_INDX_FOREACH(page, cip, i) {
			/*
			 * cip references the subtree containing the record;
			 * check for an update.
			 */
			do {
				if ((repl = WT_COL_REPL(page, cip)) != NULL) {
					addr = WT_REPL_DATA_OFF_ADDR(repl);
					size = WT_REPL_DATA_OFF_SIZE(repl);
				} else {
					addr = WT_COL_OFF_ADDR(cip);
					size = WT_COL_OFF_SIZE(cip);
				}
			} while ((ret = __wt_bt_tree_walk(
			    toc, addr, size, work, arg)) == WT_RESTART);
			if (ret != 0)
				break;
		}
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		WT_INDX_FOREACH(page, rip, i) {
			/*
			 * rip references the subtree containing the record;
			 * check for an update.
			 */
			do {
				if ((repl = WT_ROW_REPL(page, rip)) != NULL) {
					addr = WT_REPL_DATA_OFF_ADDR(repl);
					size = WT_REPL_DATA_OFF_SIZE(repl);
				} else {
					addr = WT_ROW_OFF_ADDR(rip);
					size = WT_ROW_OFF_SIZE(rip);
				}
			} while ((ret = __wt_bt_tree_walk(
			    toc, addr, size, work, arg)) == WT_RESTART);
			if (ret != 0)
				break;
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_RCC:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

err: if (page != NULL)
		__wt_bt_page_out(toc, &page, 0);

	return (ret);
}

/*
 * __wt_bt_stat_page --
 *	Stat any Btree page.
 */
int
__wt_bt_stat_page(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	IDB *idb;
	WT_PAGE_HDR *hdr;
	WT_STATS *stats;

	db = toc->db;
	idb = db->idb;
	hdr = page->hdr;
	stats = idb->dstats;

	/* Count the free space. */
	WT_STAT_INCRV(stats, PAGE_FREE, page->space_avail);

	/*
	 * All internal pages and overflow pages are trivial, all we track is
	 * a count of the page type.
	 */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		WT_STAT_INCR(stats, PAGE_COL_FIX);
		WT_RET(__wt_bt_stat_page_col_fix(toc, page));
		break;
	case WT_PAGE_COL_INT:
		WT_STAT_INCR(stats, PAGE_COL_INTERNAL);
		break;
	case WT_PAGE_COL_RCC:
		WT_STAT_INCR(stats, PAGE_COL_RCC);
		WT_RET(__wt_bt_stat_page_col_rcc(toc, page));
		break;
	case WT_PAGE_COL_VAR:
		WT_STAT_INCR(stats, PAGE_COL_VARIABLE);
		WT_RET(__wt_bt_stat_page_col_var(toc, page));
		break;
	case WT_PAGE_DUP_INT:
		WT_STAT_INCR(stats, PAGE_DUP_INTERNAL);
		break;
	case WT_PAGE_DUP_LEAF:
		WT_STAT_INCR(stats, PAGE_DUP_LEAF);
		WT_RET(__wt_bt_stat_page_dup_leaf(toc, page));
		break;
	case WT_PAGE_OVFL:
		WT_STAT_INCR(stats, PAGE_OVERFLOW);
		break;
	case WT_PAGE_ROW_INT:
		WT_STAT_INCR(stats, PAGE_ROW_INTERNAL);
		break;
	case WT_PAGE_ROW_LEAF:
		WT_STAT_INCR(stats, PAGE_ROW_LEAF);
		WT_RET(__wt_bt_stat_page_row_leaf(toc, page, arg));
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (0);
}

/*
 * __wt_bt_stat_page_col_fix --
 *	Stat a WT_PAGE_COL_FIX page.
 */
static int
__wt_bt_stat_page_col_fix(WT_TOC *toc, WT_PAGE *page)
{
	WT_COL *cip;
	WT_REPL *repl;
	WT_STATS *stats;
	uint32_t i;

	stats = toc->db->idb->dstats;

	/* Walk the page, counting data items. */
	WT_INDX_FOREACH(page, cip, i) {
		if ((repl = WT_COL_REPL(page, cip)) == NULL)
			if (WT_FIX_DELETE_ISSET(cip->data))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
		else
			if (WT_REPL_DELETED_ISSET(repl))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
	}
	return (0);
}

/*
 * __wt_bt_stat_page_col_rcc --
 *	Stat a WT_PAGE_COL_RCC page.
 */
static int
__wt_bt_stat_page_col_rcc(WT_TOC *toc, WT_PAGE *page)
{
	WT_COL *cip;
	WT_RCC_EXPAND *exp;
	WT_REPL *repl;
	WT_STATS *stats;
	uint32_t i;

	stats = toc->db->idb->dstats;

	/* Walk the page, counting data items. */
	WT_INDX_FOREACH(page, cip, i) {
		if (WT_FIX_DELETE_ISSET(WT_RCC_REPEAT_DATA(cip->data)))
			WT_STAT_INCRV(stats,
			    ITEM_COL_DELETED, WT_RCC_REPEAT_COUNT(cip->data));
		else
			WT_STAT_INCRV(stats,
			    ITEM_TOTAL_DATA, WT_RCC_REPEAT_COUNT(cip->data));

		/*
		 * Check for corrections.
		 *
		 * XXX
		 * This gets the count wrong if an application changes existing
		 * records, or updates a deleted record two times in a row --
		 * we'll incorrectly count the records as unique, when they are
		 * changes to the same record.  I'm not fixing it as I don't
		 * expect the WT_COL_RCCEXP data structure to be permanent, it's
		 * too likely to become a linked list in bad cases.
		 */
		for (exp =
		    WT_COL_RCCEXP(page, cip); exp != NULL; exp = exp->next) {
			repl = exp->repl;
			if (WT_REPL_DELETED_ISSET(repl))
				WT_STAT_INCR(stats, ITEM_COL_DELETED);
			else
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
		}
	}
	return (0);
}

/*
 * __wt_bt_stat_page_col_var --
 *	Stat a WT_PAGE_COL_VAR page.
 */
static int
__wt_bt_stat_page_col_var(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	WT_COL *cip;
	WT_REPL *repl;
	WT_STATS *stats;
	uint32_t i;

	db = toc->db;
	stats = db->idb->dstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any replacements weren't deletions.  If the item has been
	 * replaced, assume it was replaced by an item of the same size (it's
	 * to expensive to figure out if it will require the same space or not,
	 * especially if there's Huffman encoding).
	 */
	WT_INDX_FOREACH(page, cip, i) {
		switch (WT_ITEM_TYPE(cip->data)) {
		case WT_ITEM_DATA:
			repl = WT_COL_REPL(page, cip);
			if (repl == NULL || !WT_REPL_DELETED_ISSET(repl))
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_OVFL:
			repl = WT_COL_REPL(page, cip);
			if (repl == NULL || !WT_REPL_DELETED_ISSET(repl)) {
				WT_STAT_INCR(stats, ITEM_DATA_OVFL);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
			break;
		case WT_ITEM_DEL:
			WT_STAT_INCR(stats, ITEM_COL_DELETED);
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}
	return (0);
}

/*
 * __wt_bt_stat_page_dup_leaf --
 *	Stat a WT_PAGE_DUP_LEAF page.
 */
static int
__wt_bt_stat_page_dup_leaf(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	WT_REPL *repl;
	WT_ROW *rip;
	WT_STATS *stats;
	uint32_t i;

	db = toc->db;
	stats = db->idb->dstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any replacements weren't deletions.  If the item has been
	 * replaced, assume it was replaced by an item of the same size (it's
	 * to expensive to figure out if it will require the same space or not,
	 * especially if there's Huffman encoding).
	 */
	WT_INDX_FOREACH(page, rip, i) {
		switch (WT_ITEM_TYPE(rip->data)) {
		case WT_ITEM_DATA_DUP:
			repl = WT_ROW_REPL(page, rip);
			if (repl == NULL || !WT_REPL_DELETED_ISSET(repl)) {
				WT_STAT_INCR(stats, ITEM_DUP_DATA);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
			break;
		case WT_ITEM_DATA_DUP_OVFL:
			repl = WT_ROW_REPL(page, rip);
			if (repl == NULL || !WT_REPL_DELETED_ISSET(repl)) {
				WT_STAT_INCR(stats, ITEM_DUP_DATA);
				WT_STAT_INCR(stats, ITEM_DATA_OVFL);
				WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			}
			break;
		WT_ILLEGAL_FORMAT(db);
		}
	}
	return (0);
}

/*
 * __wt_bt_stat_page_row_leaf --
 *	Stat a WT_PAGE_ROW_LEAF page.
 */
static int
__wt_bt_stat_page_row_leaf(WT_TOC *toc, WT_PAGE *page, void *arg)
{
	DB *db;
	WT_REPL *repl;
	WT_ROW *rip;
	WT_STATS *stats;
	uint32_t i;

	db = toc->db;
	stats = db->idb->dstats;

	/*
	 * Walk the page, counting regular and overflow data items, and checking
	 * to be sure any replacements weren't deletions.  If the item has been
	 * replaced, assume it was replaced by an item of the same size (it's
	 * to expensive to figure out if it will require the same space or not,
	 * especially if there's Huffman encoding).
	 */
	WT_INDX_FOREACH(page, rip, i) {
		switch (WT_ITEM_TYPE(rip->data)) {
		case WT_ITEM_DATA:
			repl = WT_ROW_REPL(page, rip);
			if (repl != NULL && WT_REPL_DELETED_ISSET(repl))
				continue;
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_OVFL:
			repl = WT_ROW_REPL(page, rip);
			if (repl != NULL && WT_REPL_DELETED_ISSET(repl))
				continue;
			WT_STAT_INCR(stats, ITEM_DATA_OVFL);
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_DUP:
			repl = WT_ROW_REPL(page, rip);
			if (repl != NULL && WT_REPL_DELETED_ISSET(repl))
				continue;
			WT_STAT_INCR(stats, ITEM_DUP_DATA);
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_DATA_DUP_OVFL:
			repl = WT_ROW_REPL(page, rip);
			if (repl != NULL && WT_REPL_DELETED_ISSET(repl))
				continue;
			WT_STAT_INCR(stats, ITEM_DUP_DATA);
			WT_STAT_INCR(stats, ITEM_DATA_OVFL);
			WT_STAT_INCR(stats, ITEM_TOTAL_DATA);
			break;
		case WT_ITEM_OFF:
			WT_STAT_INCR(stats, DUP_TREE);
			/*
			 * Recursively call the tree-walk function for the
			 * off-page duplicate tree.
			 */
			WT_RET_RESTART(__wt_bt_tree_walk(toc,
			    WT_ROW_OFF_ADDR(rip), WT_ROW_OFF_SIZE(rip),
			    __wt_bt_stat_page, arg));
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		/*
		 * If the data item wasn't deleted, count the key.
		 *
		 * If we have processed the key, we have lost the information as
		 * to whether or not it's an overflow key -- we can figure out
		 * if it's Huffman encoded by looking at the huffman key, but
		 * that doesn't tell us if it's an overflow key or not.  To fix
		 * this we'd have to maintain a reference to the on-page key and
		 * check it, and I'm not willing to spend the additional pointer
		 * in the WT_ROW structure.
		 */
		if (WT_KEY_PROCESS(rip))
			switch (WT_ITEM_TYPE(rip->key)) {
			case WT_ITEM_KEY_OVFL:
				WT_STAT_INCR(stats, ITEM_KEY_OVFL);
				/* FALLTHROUGH */
			case WT_ITEM_KEY:
				WT_STAT_INCR(stats, ITEM_TOTAL_KEY);
				break;
			WT_ILLEGAL_FORMAT(db);
			}
		else
			WT_STAT_INCR(stats, ITEM_TOTAL_KEY);

	}
	return (0);
}
