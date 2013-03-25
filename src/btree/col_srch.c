/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_modify)
{
	WT_BTREE *btree;
	WT_COL *cip;
	WT_DECL_RET;
	WT_INSERT *ins;
	WT_INSERT_HEAD *ins_head;
	WT_PAGE *page;
	WT_REF *ref;
	uint64_t recno;
	uint32_t base, indx, limit;
	int depth;

	__cursor_search_clear(cbt);

	recno = cbt->iface.recno;

	btree = S2BT(session);
	ref = NULL;

	/* Search the internal pages of the tree. */
	for (depth = 2,
	    page = btree->root_page; page->type == WT_PAGE_COL_INT; ++depth) {
		WT_ASSERT(session, ref == NULL ||
		    ref->u.recno == page->u.intl.recno);

		/* Fast path appends. */
		base = page->entries;
		ref = &page->u.intl.t[base - 1];
		if (recno >= ref->u.recno)
			goto descend;

		/* Binary search of internal pages. */
		for (base = 0, ref = NULL,
		    limit = page->entries - 1; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			ref = page->u.intl.t + indx;

			if (recno == ref->u.recno)
				break;
			if (recno < ref->u.recno)
				continue;
			base = indx + 1;
			--limit;
		}

descend:	WT_ASSERT(session, ref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than recno and may be the
		 * (last + 1) index.  The slot for descent is the one before
		 * base.
		 */
		if (recno != ref->u.recno) {
			/*
			 * We don't have to correct for base == 0 because the
			 * only way for base to be 0 is if recno is the page's
			 * starting recno.
			 */
			WT_ASSERT(session, base > 0);
			ref = page->u.intl.t + (base - 1);
		}

		/*
		 * Swap the parent page for the child page; return on error,
		 * the swap function ensures we're holding nothing on failure.
		 */
		WT_RET(__wt_page_swap(session, page, page, ref));
		page = ref->page;
	}

	/*
	 * We want to know how deep the tree gets because excessive depth can
	 * happen because of how WiredTiger splits.
	 */
	if (depth > btree->maximum_depth)
		btree->maximum_depth = depth;

	/*
	 * Copy the leaf page's write generation value before reading the page.
	 * Use a read memory barrier to ensure we read the value before we read
	 * any of the page's contents.
	 */
	if (is_modify) {
		/* Initialize the page's modification information */
		WT_ERR(__wt_page_modify_init(session, page));

		WT_ORDERED_READ(cbt->write_gen, page->modify->write_gen);
	}

	cbt->page = page;
	cbt->recno = recno;
	cbt->compare = 0;

	/*
	 * Search the leaf page.  We do not check in the search path for a
	 * record greater than the maximum record in the tree; in that case,
	 * we arrive here with a record that's impossibly large for the page.
	 */
	if (page->type == WT_PAGE_COL_FIX) {
		if (recno >= page->u.col_fix.recno + page->entries) {
			cbt->recno = page->u.col_fix.recno + page->entries;
			goto past_end;
		} else
			ins_head = WT_COL_UPDATE_SINGLE(page);
	} else
		if ((cip = __col_var_search(page, recno)) == NULL) {
			cbt->recno = __col_last_recno(page);
			goto past_end;
		} else {
			cbt->slot = WT_COL_SLOT(page, cip);
			ins_head = WT_COL_UPDATE_SLOT(page, cbt->slot);
		}

	/*
	 * We have a match on the page, check for an update.  Check the page's
	 * update list (fixed-length), or slot's update list (variable-length)
	 * for a better match.  The only better match we can find is an exact
	 * match, otherwise the existing match on the page is the one we want.
	 * For that reason, don't set the cursor's WT_INSERT_HEAD/WT_INSERT pair
	 * until we know we have a useful entry.
	 */
	if ((ins = __col_insert_search(
	    ins_head, cbt->ins_stack, cbt->next_stack, recno)) != NULL)
		if (recno == WT_INSERT_RECNO(ins)) {
			cbt->ins_head = ins_head;
			cbt->ins = ins;
		}
	return (0);

past_end:
	/*
	 * A record past the end of the page's standard information.  Check the
	 * append list; by definition, any record on the append list is closer
	 * than the last record on the page, so it's a better choice for return.
	 * This is a rarely used path: we normally find exact matches, because
	 * column-store files are dense, but in this case the caller searched
	 * past the end of the table.
	 */
	cbt->ins_head = WT_COL_APPEND(page);
	if ((cbt->ins = __col_insert_search(
	    cbt->ins_head, cbt->ins_stack, cbt->next_stack, recno)) == NULL)
		cbt->compare = -1;
	else {
		cbt->recno = WT_INSERT_RECNO(cbt->ins);
		if (recno == cbt->recno)
			cbt->compare = 0;
		else if (recno < cbt->recno)
			cbt->compare = 1;
		else
			cbt->compare = -1;
	}

	/*
	 * Note if the record is past the maximum record in the tree, the cursor
	 * search functions need to know for fixed-length column-stores because
	 * appended records implicitly create any skipped records, and cursor
	 * search functions have to handle that case.
	 */
	if (cbt->compare == -1)
		F_SET(cbt, WT_CBT_MAX_RECORD);
	return (0);

err:	WT_TRET(__wt_page_release(session, page));
	return (ret);
}
