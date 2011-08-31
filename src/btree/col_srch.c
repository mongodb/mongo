/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __search_insert --
 *	Search the slot's insert list.
 */
static inline WT_INSERT *
__search_insert(WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *inshead, uint64_t recno)
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
		else				/* Drop down a level */
			cbt->ins_stack[i--] = ins--;
	}

	return (NULL);
}

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_modify)
{
	WT_BTREE *btree;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_PAGE *page;
	uint64_t recno, start_recno;
	uint32_t base, indx, limit;
	int ret;

	__cursor_search_reset(cbt);

	cbt->recno = recno = cbt->iface.recno;

	btree = session->btree;
	cref = NULL;
	start_recno = 0;

	/* Search the internal pages of the tree. */
	for (page = btree->root_page.page; page->type == WT_PAGE_COL_INT;) {
		/* Binary search of internal pages. */
		for (base = 0,
		    limit = page->entries; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			cref = page->u.col_int.t + indx;

			start_recno = cref->recno;
			if (recno == start_recno)
				break;
			if (recno < start_recno)
				continue;
			base = indx + 1;
			--limit;
		}
		WT_ASSERT(session, cref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than recno and may be the
		 * (last + 1) index.  The slot for descent is the one before
		 * base.
		 */
		if (recno != start_recno) {
			/*
			 * We don't have to correct for base == 0 because the
			 * only way for base to be 0 is if recno is the page's
			 * starting recno.
			 */
			WT_ASSERT(session, base > 0);
			cref = page->u.col_int.t + (base - 1);
		}

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, &cref->ref, 0));
		__wt_page_release(session, page);
		page = WT_COL_REF_PAGE(cref);
	}

	/*
	 * Copy the leaf page's write generation value before reading the page.
	 * Use a memory barrier to ensure we read the value before we read any
	 * of the page's contents.
	 */
	if (is_modify) {
		cbt->write_gen = page->write_gen;
		WT_MEMORY_FLUSH;
	}
	cbt->page = page;

	/*
	 * Search the leaf page.  We do not check in the search path for a
	 * record greater than the maximum record in the tree; in that case,
	 * we arrive here with a record that's impossibly large for the page.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (recno >= page->u.col_leaf.recno + page->entries) {
			cbt->match = 0;
			return (0);
		}
		cbt->ins_head =
		    page->u.col_leaf.ins == NULL ? NULL : *page->u.col_leaf.ins;
		cbt->match = 1;
		break;
	case WT_PAGE_COL_VAR:
		if ((cip = __cursor_col_rle_search(page, recno)) == NULL)
			cbt->match = 0;
		else {
			cbt->match = 1;
			cbt->slot = WT_COL_SLOT(page, cip);
			cbt->ins_head = WT_COL_INSERT_SLOT(page, cbt->slot);
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/*
	 * Search the insert list for a match; __search_insert sets the return
	 * insert information appropriately.
	 */
	cbt->ins = __search_insert(cbt, cbt->ins_head, recno);

	return (0);

err:	__wt_page_release(session, page);
	return (ret);
}
