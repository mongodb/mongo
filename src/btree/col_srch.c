/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_col_ins_search --
 *	Search the slot's insert list.
 */
static inline int
__wt_col_ins_search(
    WT_SESSION_IMPL *session, WT_INSERT_HEAD *inshead, uint64_t recno)
{
	WT_INSERT **ins;
	uint64_t ins_recno;
	int cmp, i;

	if (inshead == NULL)
		return (1);

	/*
	 * The insert list is a skip list: start at the highest skip level,
	 * then go as far as possible at each level before stepping down to the
	 * next one.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL)
			cmp = -1;
		else {
			ins_recno = WT_INSERT_RECNO(*ins);
			cmp = (recno == ins_recno) ? 0 :
			    (recno < ins_recno) ? -1 : 1;
		}
		if (cmp == 0) {
			/* Clear the ins array, we're not going to insert. */
			memset(session->srch.ins, 0, sizeof(session->srch.ins));
			session->srch.vupdate = (*ins)->upd;
			session->srch.upd = &(*ins)->upd;
			return (0);
		} else if (cmp > 0)
			/* Keep going on this level. */
			ins = &(*ins)->next[i];
		else
			/* Go down a level in the skiplist. */
			session->srch.ins[i--] = ins--;
	}

	return (1);
}

/*
 * __wt_col_search --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_col_search(WT_SESSION_IMPL *session, uint64_t recno, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_COL *cip;
	WT_COL_REF *cref;
	WT_COL_RLE *repeat;
	WT_PAGE *page;
	uint64_t start_recno;
	uint32_t base, indx, limit, match, start_indx, write_gen;
	int ret;

	unpack = &_unpack;
	cip = NULL;
	cref = NULL;
	start_recno = 0;

	/* Return values. */
	WT_CLEAR(session->srch);
	session->srch.slot = UINT32_MAX;

	btree = session->btree;

	/* Search the tree. */
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
		if (page != btree->root_page.page)
			__wt_hazard_clear(session, page);
		page = WT_COL_REF_PAGE(cref);
	}

	/*
	 * Copy the page's write generation value before reading anything on
	 * the page.
	 */
	write_gen = page->write_gen;

	/*
	 * Search the leaf page.  We do not check in the search path for a
	 * record greater than the maximum record in the tree; in that case,
	 * we arrive here with a record that's impossibly large for the page.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:
		if (recno >= page->u.col_leaf.recno + page->entries) {
			if (LF_ISSET(WT_WRITE))
				goto append;
			goto notfound;
		}

		/*
		 * Search the WT_COL's insert list for the record's WT_INSERT
		 * slot.
		 */
		if (page->u.col_leaf.ins == NULL)
			match = 0;
		else {
			session->srch.inshead = page->u.col_leaf.ins;
			match = (__wt_col_ins_search(session,
			    *session->srch.inshead, recno) == 0);
		}

		/*
		 * Fixed-length column store entries are never deleted, bits are
		 * just set to 0.  If we didn't find an update structure, return
		 * the original value.
		 */
		if (!match)
			session->srch.v =
			    __bit_getv_recno(page, recno, btree->bitcnt);
		break;
	case WT_PAGE_COL_VAR:
		/*
		 * Find the matching slot.
		 *
		 * This is done in two stages: first, we do a binary search
		 * among any repeating records to find largest repeating
		 * less than the search key.  Once there, we can do a simple
		 * offset calculation to find the correct slot for this record
		 * number, because we know any intervening records will have
		 * repeat counts of 1.
		 */
		cip = NULL;
		for (base = 0, limit = page->u.col_leaf.nrepeats;
		    limit != 0;
		    limit >>= 1) {
			indx = base + (limit >> 1);

			repeat = page->u.col_leaf.repeats + indx;
			if (recno >= repeat->recno &&
			    recno < repeat->recno + repeat->rle) {
				cip = page->u.col_leaf.d + repeat->indx;
				break;
			}
			if (recno < repeat->recno)
				continue;
			base = indx + 1;
			--limit;
		}

		/*
		 * If we didn't find an exact match, take the largest repeat
		 * less than the search key.
		 */
		if (cip == NULL) {
			if (base == 0) {
				start_indx = 0;
				start_recno = page->u.col_leaf.recno;
			} else {
				repeat = page->u.col_leaf.repeats + (base - 1);
				start_indx = repeat->indx + 1;
				start_recno = repeat->recno + repeat->rle;
			}

			if (recno >= start_recno +
			    (page->entries - start_indx)) {
				if (LF_ISSET(WT_WRITE))
					goto append;
				goto notfound;
			}

			cip = page->u.col_leaf.d + start_indx +
			    (uint32_t)(recno - start_recno);
		}

		/* Now we have a slot, look up the cell and unpack. */
		if ((cell = WT_COL_PTR(page, cip)) != NULL)
			__wt_cell_unpack(cell, unpack);

		/*
		 * We have the right WT_COL slot: if it's a write, set up the
		 * return information in session->{srch.upd,slot}.  If it's a
		 * read, set up the return information in session->srch.vupdate.
		 *
		 * Search the WT_COL's insert list for the record's WT_INSERT
		 * slot.  The insert list is a sorted, forward-linked list: on
		 * average, we have to search half of it.
		 *
		 * Do an initial setup of the return information (we'll correct
		 * it as needed depending on what we find).
		 */
		session->srch.slot = WT_COL_SLOT(page, cip);
		if (page->u.col_leaf.ins == NULL)
			match = 0;
		else {
			session->srch.inshead =
			    &page->u.col_leaf.ins[session->srch.slot];
			match = (__wt_col_ins_search(session,
			    *session->srch.inshead, recno) == 0);
		}

		/*
		 * If we're not updating an existing data item, check to see if
		 * the item has been deleted.  If we found a match, use the
		 * WT_INSERT's WT_UPDATE value.  If we didn't find a match, use
		 * use the original data.
		 */
		if (LF_ISSET(WT_WRITE))
			break;

		if (match) {
			if (WT_UPDATE_DELETED_ISSET(session->srch.vupdate))
				goto notfound;
		} else
			if (cell != NULL && unpack->type == WT_CELL_DEL)
				goto notfound;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	session->srch.match = 1;
	if (0) {
append:		session->srch.match = 0;
	}
	session->srch.page = page;
	session->srch.write_gen = write_gen;
	session->srch.ip = cip;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	__wt_page_release(session, page);
	return (ret);
}
