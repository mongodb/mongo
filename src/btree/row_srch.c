/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	size_t match, skiphigh, skiplow;
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
	WT_RET(
	    WT_LEX_CMP(session, btree->collator, srch_key, &insert_key, cmp));
	if (cmp >= 0) {
		/*
		 * !!!
		 * We may race with another appending thread.
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
	match = skiphigh = skiplow = 0;
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
			match = WT_MIN(skiplow, skiphigh);
			WT_RET(WT_LEX_CMP_SKIP(session,
			    btree->collator,
			    srch_key, &insert_key, cmp, &match));
		}

		if (cmp > 0) {		/* Keep going at this level */
			insp = &ret_ins->next[i];
			skiplow = match;
		} else if (cmp == 0)
			for (; i >= 0; i--) {
				cbt->next_stack[i] = ret_ins->next[i];
				cbt->ins_stack[i] = &ret_ins->next[i];
			}
		else {			/* Drop down a level */
			cbt->next_stack[i] = ret_ins;
			cbt->ins_stack[i--] = insp--;
			skiphigh = match;
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
__wt_row_search(WT_SESSION_IMPL *session,
    WT_ITEM *srch_key, WT_REF *leaf_page, WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *item, _item;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *child, *parent;
	WT_ROW *rip;
	size_t match, skiphigh, skiplow;
	uint32_t base, indx, limit;
	int cmp, depth;

	btree = S2BT(session);
	rip = NULL;
	match = 0;				/* -Wuninitialized */

	__cursor_search_clear(cbt);

	item = &_item;
	WT_CLEAR_INLINE(WT_ITEM, *item);

restart:
	/*
	 * The row-store search routine uses a different comparison API.
	 * The assumption is we're comparing more than a few keys with
	 * matching prefixes, and it's a win to avoid the memory fetches
	 * by skipping over those prefixes.  That's done by tracking the
	 * length of the prefix match for the lowest and highest keys we
	 * compare as we descend the tree.
	 */
	skiphigh = skiplow = 0;

	/*
	 * In the service of eviction splits, we're only searching a single leaf
	 * page, not a full tree.
	 */
	if (leaf_page != NULL) {
		child = leaf_page;
		goto leaf_only;
	}

	/* Search the internal pages of the tree. */
	cmp = -1;
	parent = child = &btree->root_page;
	for (depth = 2;; ++depth) {
		page = parent->page;
		if (page->type != WT_PAGE_ROW_INT)
			break;

		pindex = page->pg_intl_index;
		base = pindex->entries;
		child = pindex->index[base - 1];

		/*
		 * Fast-path internal pages with one child, a common case for
		 * the root page in new trees.
		 */
		if (base == 1)
			goto descend;

		/* Fast-path appends. */
		__wt_ref_key(page, child, &item->data, &item->size);
		WT_ERR(
		    WT_LEX_CMP(session, btree->collator, srch_key, item, cmp));
		if (cmp >= 0)
			goto descend;

		/*
		 * Two versions of the binary search of internal pages: with and
		 * without application-specified collation.
		 */
		base = 0;
		limit = pindex->entries - 1;
		if (btree->collator == NULL) {
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				child = pindex->index[indx];

				/*
				 * If about to compare an application key with
				 * the 0th index on an internal page, pretend
				 * the 0th index sorts less than any application
				 * key.  This test is so we don't have to update
				 * internal pages if the application stores a
				 * new, "smallest" key in the tree.
				 */
				if (indx != 0) {
					__wt_ref_key(page,
					    child, &item->data, &item->size);
					match = WT_MIN(skiplow, skiphigh);
					cmp = __wt_lex_compare_skip(
					    srch_key, item, &match);
					if (cmp == 0)
						goto descend;
					if (cmp < 0) {
						skiphigh = match;
						continue;
					}
					skiplow = match;
				}
				base = indx + 1;
				--limit;
			}
			/*
			 * Reference the slot used for next step down the tree.
			 *
			 * Base is the smallest index greater than key and may
			 * be the (last + 1) index.  (Base cannot be the 0th
			 * index as the 0th index always sorts less than any
			 * application key).  The slot for descent is the one
			 * before base.
			 */
			if (cmp != 0)
				child = pindex->index[base - 1];
		} else {
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				child = pindex->index[indx];
				/*
				 * If about to compare an application key with
				 * the 0th index on an internal page, pretend
				 * the 0th index sorts less than any application
				 * key.  This test is so we don't have to update
				 * internal pages if the application stores a
				 * new, "smallest" key in the tree.
				 */
				if (indx != 0) {
					__wt_ref_key(page,
					    child, &item->data, &item->size);
					WT_ERR(WT_LEX_CMP_SKIP(
					    session, btree->collator,
					    srch_key, item, cmp, &match));
					if (cmp == 0)
						goto descend;
					if (cmp < 0)
						continue;
				}
				base = indx + 1;
				--limit;
			}
			/*
			 * Reference the slot used for next step down the tree.
			 *
			 * Base is the smallest index greater than key and may
			 * be the (last + 1) index.  (Base cannot be the 0th
			 * index as the 0th index always sorts less than any
			 * application key).  The slot for descent is the one
			 * before base.
			 */
			if (cmp != 0)
				child = pindex->index[base - 1];
		}
descend:	WT_ASSERT(session, child != NULL);

		/*
		 * Swap the parent page for the child page; if the page splits
		 * while we're waiting for it, restart the search, otherwise
		 * return on error, the swap call ensures we're holding nothing
		 * on failure.
		 */
		if ((ret = __wt_page_swap(session, parent, child, 0)) == 0) {
			parent = child;
			continue;
		}
		/*
		 * Restart is returned if we find a page that's been split; the
		 * held page isn't discarded when restart is returned, discard
		 * it and restart the search from the top of the tree.
		 */
		if (ret == WT_RESTART &&
		    (ret = __wt_page_release(session, parent)) == 0)
			goto restart;
		return (ret);
	}

	/* Track how deep the tree gets. */
	if (depth > btree->maximum_depth)
		btree->maximum_depth = depth;

leaf_only:
	page = child->page;

	/*
	 * Binary search of the leaf page.  There are two versions (a default
	 * loop and an application-specified collation loop), because moving
	 * the collation test and error handling inside the loop costs about 5%.
	 *
	 * The page might be empty, reset the comparison value.
	 */
	cmp = -1;
	base = 0;
	limit = page->pg_row_entries;
	if (btree->collator == NULL)
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;

			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));
			match = WT_MIN(skiplow, skiphigh);
			cmp = __wt_lex_compare_skip(srch_key, item, &match);
			if (cmp == 0)
				break;
			if (cmp < 0) {
				skiphigh = match;
				continue;
			}
			skiplow = match;

			base = indx + 1;
			--limit;
		}
	else
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;

			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));
			WT_ERR(WT_LEX_CMP_SKIP(session,
			    btree->collator, srch_key, item, cmp, &match));
			if (cmp == 0)
				break;
			if (cmp < 0)
				continue;

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
		cbt->ref = child;
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
	rip = page->pg_row_d;
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
	cbt->ref = child;
	WT_ERR(__wt_search_insert(session, cbt, cbt->ins_head, srch_key));
	return (0);

err:	WT_TRET(__wt_page_release(session, child));
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
	WT_PAGE_INDEX *pindex;
	WT_REF *child, *parent;

	btree = S2BT(session);

	__cursor_search_clear(cbt);

restart:
	/* Walk the internal pages of the tree. */
	parent = child = &btree->root_page;
	for (;;) {
		page = parent->page;
		if (page->type != WT_PAGE_ROW_INT)
			break;

		pindex = page->pg_intl_index;
		child = pindex->index[__wt_random() % pindex->entries];

		/*
		 * Swap the parent page for the child page; return on error,
		 * the swap function ensures we're holding nothing on failure.
		 */
		if ((ret = __wt_page_swap(session, parent, child, 0)) == 0) {
			parent = child;
			continue;
		}
		/*
		 * Restart is returned if we find a page that's been split; the
		 * held page isn't discarded when restart is returned, discard
		 * it and restart the search from the top of the tree.
		 */
		if (ret == WT_RESTART &&
		    (ret = __wt_page_release(session, parent)) == 0)
			goto restart;
		return (ret);
	}

	if (page->pg_row_entries != 0) {
		/*
		 * The use case for this call is finding a place to split the
		 * tree.  Cheat (it's not like this is "random", anyway), and
		 * make things easier by returning the first key on the page.
		 * If the caller is attempting to split a newly created tree,
		 * or a tree with just one big page, that's not going to work,
		 * check for that.
		 */
		cbt->ref = child;
		cbt->compare = 0;
		cbt->slot =
		    btree->root_page.page->pg_intl_index->entries < 2 ?
		    __wt_random() % page->pg_row_entries : 0;
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
	cbt->ref = child;
	cbt->compare = 0;
	cbt->ins = t;

	return (0);

err:	WT_TRET(__wt_page_release(session, child));
	return (ret);
}
