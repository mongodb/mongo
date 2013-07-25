/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_search_insert --
 *	Search a row-store insert list, creating a skiplist stack as we go.
 */
int
__wt_search_insert(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *inshead, WT_ITEM *srch_key)
{
	WT_BTREE *btree;
	WT_INSERT **insp, *last_ins, *ret_ins;
	WT_ITEM insert_key;
	int cmp, i;

	btree = S2BT(session);

	/* If there's no insert chain to search, we're done. */
	if ((ret_ins = WT_SKIP_LAST(inshead)) == NULL) {
		cbt->ins = NULL;
		cbt->next_stack[0] = NULL;
		return (0);
	}

	/* Fast-path appends. */
	insert_key.data = WT_INSERT_KEY(ret_ins);
	insert_key.size = WT_INSERT_KEY_SIZE(ret_ins);
	WT_RET(WT_BTREE_CMP(session, btree, srch_key, &insert_key, cmp));
	if (cmp >= 0) {
		/*
		 * XXX We may race with another appending thread.
		 *
		 * To catch that case, rely on the atomic pointer read above
		 * and set the next stack to NULL here.  If we have raced with
		 * another thread, one of the next pointers will not be NULL by
		 * the time they are checked against the next stack inside the
		 * serialized insert function.
		 */
		for (i = WT_SKIP_MAXDEPTH - 1; i >= 0; i--) {
			cbt->ins_stack[i] = (i == 0) ? &ret_ins->next[0] :
			    (inshead->tail[i] != NULL) ?
			    &inshead->tail[i]->next[i] : &inshead->head[i];
			cbt->next_stack[i] = NULL;
		}
		cbt->compare = -cmp;
		cbt->ins = ret_ins;
		return (0);
	}

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	last_ins = ret_ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;) {
		if ((ret_ins = *insp) == NULL) {
			cbt->next_stack[i] = NULL;
			cbt->ins_stack[i--] = insp--;
			continue;
		}

		/*
		 * Comparisons may be repeated as we drop down skiplist levels;
		 * don't repeat comparisons, they might be expensive.
		 */
		if (ret_ins != last_ins) {
			last_ins = ret_ins;
			insert_key.data = WT_INSERT_KEY(ret_ins);
			insert_key.size = WT_INSERT_KEY_SIZE(ret_ins);
			WT_RET(WT_BTREE_CMP(
			    session, btree, srch_key, &insert_key, cmp));
		}

		if (cmp > 0)		/* Keep going at this level */
			insp = &ret_ins->next[i];
		else if (cmp == 0)
			for (; i >= 0; i--) {
				cbt->next_stack[i] = ret_ins->next[i];
				cbt->ins_stack[i] = &ret_ins->next[i];
			}
		else {			/* Drop down a level */
			cbt->next_stack[i] = ret_ins;
			cbt->ins_stack[i--] = insp--;
		}
	}

	/*
	 * For every insert element we review, we're getting closer to a better
	 * choice; update the compare field to its new value.
	 */
	cbt->compare = -cmp;
	cbt->ins = ret_ins;
	return (0);
}

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, int is_modify)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *item, _item, *srch_key;
	WT_PAGE *page;
	WT_REF *ref;
	WT_ROW *rip;
	uint32_t base, indx, limit, match, skiphigh, skiplow;
	int cmp, depth;

	__cursor_search_clear(cbt);

	srch_key = &cbt->iface.key;

	btree = S2BT(session);
	rip = NULL;

	item = &_item;
	WT_CLEAR(_item);

	/* Search the internal pages of the tree. */
	cmp = -1;
	for (depth = 2,
	    page = btree->root_page; page->type == WT_PAGE_ROW_INT; ++depth) {
		/*
		 * Fast-path internal pages with one child, a common case for
		 * the root page in new trees.
		 */
		base = page->entries;
		ref = &page->u.intl.t[base - 1];
		if (base == 1)
			goto descend;

		/* Fast-path appends. */
		__wt_ref_key(page, ref, &item->data, &item->size);
		WT_ERR(WT_BTREE_CMP(session, btree, srch_key, item, cmp));
		if (cmp >= 0)
			goto descend;

		/*
		 * Binary search of internal pages.
		 *
		 * The row-store search routine uses a different comparison API.
		 * The assumption is we're comparing more than a few keys with
		 * matching prefixes, and it's a win to avoid the memory fetches
		 * by skipping over those prefixes.  That's done by tracking the
		 * length of the prefix match for the lowest and highest keys we
		 * compare.
		 */
		skiphigh = skiplow = 0;
		for (base = 0, ref = NULL,
		    limit = page->entries - 1; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			ref = page->u.intl.t + indx;

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 */
			if (indx != 0) {
				__wt_ref_key(
				    page, ref, &item->data, &item->size);
				match = WT_MIN(skiplow, skiphigh);
				WT_ERR(WT_BTREE_CMP_SKIP(session,
				    btree, srch_key, item, cmp, &match));
				if (cmp == 0)
					break;
				if (cmp < 0) {
					skiplow = match;
					continue;
				}
				skiphigh = match;
			}
			base = indx + 1;
			--limit;
		}

descend:	WT_ASSERT(session, ref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than key and may be the
		 * (last + 1) index.  (Base cannot be the 0th index as the 0th
		 * index always sorts less than any application key).  The slot
		 * for descent is the one before base.
		 */
		if (cmp != 0)
			ref = page->u.intl.t + (base - 1);

		/*
		 * Swap the parent page for the child page; return on error,
		 * the swap function ensures we're holding nothing on failure.
		 *
		 * !!!
		 * Don't use WT_RET, we've already used WT_ERR, and the style
		 * checking code complains if we use WT_RET after a jump to an
		 * error label.
		 */
		if ((ret = __wt_page_swap(session, page, page, ref)) != 0)
			return (ret);
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

	/*
	 * Do a binary search of the leaf page; the page might be empty, reset
	 * the comparison value.
	 */
	cmp = -1;
	skiphigh = skiplow = 0;			/* See internal loop comment. */
	for (base = 0, limit = page->entries; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row.d + indx;

		WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));
		match = WT_MIN(skiplow, skiphigh);
		WT_ERR(WT_BTREE_CMP_SKIP(
		    session, btree, srch_key, item, cmp, &match));
		if (cmp == 0)
			break;
		if (cmp < 0) {
			skiplow = match;
			continue;
		}

		skiphigh = match;
		base = indx + 1;
		--limit;
	}

	/*
	 * We don't expect the search item to have any allocated memory (it's a
	 * performance problem if it does).  Trust, but verify, and complain if
	 * there's a problem.
	 */
	if (item->mem != NULL) {
		static int complain = 1;
		if (complain) {
			__wt_errx(session,
			    "unexpected key item memory allocation in search");
			complain = 0;
		}
		__wt_buf_free(session, item);
	}

	/*
	 * The best case is finding an exact match in the page's WT_ROW slot
	 * array, which is probable for any read-mostly workload.  In that
	 * case, we're not doing any kind of insert, all we can do is update
	 * an existing entry.  Check that case and get out fast.
	 */
	if (cmp == 0) {
		WT_ASSERT(session, rip != NULL);
		cbt->compare = 0;
		cbt->page = page;
		cbt->slot = WT_ROW_SLOT(page, rip);
		return (0);
	}

	/*
	 * We didn't find an exact match in the WT_ROW array.
	 *
	 * Base is the smallest index greater than key and may be the 0th index
	 * or the (last + 1) index.  Set the WT_ROW reference to be the largest
	 * index less than the key if that's possible (if base is the 0th index
	 * it means the application is inserting a key before any key found on
	 * the page).
	 */
	rip = page->u.row.d;
	if (base == 0)
		cbt->compare = 1;
	else {
		rip += base - 1;
		cbt->compare = -1;
	}

	/*
	 * It's still possible there is an exact match, but it's on an insert
	 * list.  Figure out which insert chain to search, and do the initial
	 * setup of the return information for the insert chain (we'll correct
	 * it as needed depending on what we find.)
	 *
	 * If inserting a key smaller than any key found in the WT_ROW array,
	 * use the extra slot of the insert array, otherwise insert lists map
	 * one-to-one to the WT_ROW array.
	 */
	cbt->slot = WT_ROW_SLOT(page, rip);
	if (base == 0) {
		F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
	} else
		cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);

	/*
	 * Search the insert list for a match; __wt_search_insert sets the
	 * return insert information appropriately.
	 */
	cbt->page = page;
	WT_ERR(__wt_search_insert(session, cbt, cbt->ins_head, srch_key));
	return (0);

err:	WT_TRET(__wt_page_release(session, page));
	return (ret);
}

/*
 * __wt_row_random --
 *	Return a random key from a row-store tree.
 */
int
__wt_row_random(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_INSERT *p, *t;
	WT_PAGE *page;
	WT_REF *ref;

	__cursor_search_clear(cbt);

	btree = S2BT(session);

	/* Walk the internal pages of the tree. */
	for (page = btree->root_page; page->type == WT_PAGE_ROW_INT;) {
		ref = page->u.intl.t + __wt_random() % page->entries;

		/*
		 * Swap the parent page for the child page; return on error,
		 * the swap function ensures we're holding nothing on failure.
		 */
		WT_RET(__wt_page_swap(session, page, page, ref));
		page = ref->page;
	}

	if (page->entries != 0) {
		/*
		 * The use case for this call is finding a place to split the
		 * tree.  Cheat (it's not like this is "random", anyway), and
		 * make things easier by returning the first key on the page.
		 * If the caller is attempting to split a newly created tree,
		 * or a tree with just one big page, that's not going to work,
		 * check for that.
		 */
		cbt->page = page;
		cbt->compare = 0;
		cbt->slot =
		    btree->root_page->entries < 2 ?
		    __wt_random() % page->entries : 0;
		return (0);
	}

	/*
	 * If the tree is new (and not empty), it might have a large insert
	 * list, pick the key in the middle of that insert list.
	 */
	F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
	if ((cbt->ins_head = WT_ROW_INSERT_SMALLEST(page)) == NULL)
		WT_ERR(WT_NOTFOUND);
	for (p = t = WT_SKIP_FIRST(cbt->ins_head);;) {
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		t = WT_SKIP_NEXT(t);
	}
	cbt->page = page;
	cbt->compare = 0;
	cbt->ins = t;

	return (0);

err:	WT_TRET(__wt_page_release(session, page));
	return (ret);
}
