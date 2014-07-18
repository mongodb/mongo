/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_search_insert_append --
 *	Fast append search of a row-store insert list, creating a skiplist stack
 * as we go.
 */
static inline int
__wt_search_insert_append(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt, WT_ITEM *srch_key, int *appendp)
{
	WT_BTREE *btree;
	WT_INSERT *ins;
	WT_INSERT_HEAD *inshead;
	WT_ITEM key;
	int cmp, i;

	btree = S2BT(session);
	*appendp = 0;

	inshead = cbt->ins_head;
	if ((ins = WT_SKIP_LAST(inshead)) == NULL)
		return (0);
	key.data = WT_INSERT_KEY(ins);
	key.size = WT_INSERT_KEY_SIZE(ins);

	WT_RET(WT_LEX_CMP(session, btree->collator, srch_key, &key, cmp));
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
			cbt->ins_stack[i] = (i == 0) ? &ins->next[0] :
			    (inshead->tail[i] != NULL) ?
			    &inshead->tail[i]->next[i] : &inshead->head[i];
			cbt->next_stack[i] = NULL;
		}
		cbt->compare = -cmp;
		cbt->ins = ins;
		*appendp = 1;
	}
	return (0);
}

/*
 * __wt_search_insert --
 *	Search a row-store insert list, creating a skiplist stack as we go.
 */
int
__wt_search_insert(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *srch_key)
{
	WT_BTREE *btree;
	WT_INSERT *ins, **insp, *last_ins;
	WT_INSERT_HEAD *inshead;
	WT_ITEM key;
	size_t match, skiphigh, skiplow;
	int cmp, i;

	btree = S2BT(session);
	inshead = cbt->ins_head;
	cmp = 0;				/* -Wuninitialized */

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	match = skiphigh = skiplow = 0;
	ins = last_ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0;) {
		if ((ins = *insp) == NULL) {
			cbt->next_stack[i] = NULL;
			cbt->ins_stack[i--] = insp--;
			continue;
		}

		/*
		 * Comparisons may be repeated as we drop down skiplist levels;
		 * don't repeat comparisons, they might be expensive.
		 */
		if (ins != last_ins) {
			last_ins = ins;
			key.data = WT_INSERT_KEY(ins);
			key.size = WT_INSERT_KEY_SIZE(ins);
			match = WT_MIN(skiplow, skiphigh);
			WT_RET(WT_LEX_CMP_SKIP(session,
			    btree->collator, srch_key, &key, cmp, &match));
		}

		if (cmp > 0) {			/* Keep going at this level */
			insp = &ins->next[i];
			skiplow = match;
		} else if (cmp < 0) {		/* Drop down a level */
			cbt->next_stack[i] = ins;
			cbt->ins_stack[i--] = insp--;
			skiphigh = match;
		} else
			for (; i >= 0; i--) {
				cbt->next_stack[i] = ins->next[i];
				cbt->ins_stack[i] = &ins->next[i];
			}
	}

	/*
	 * For every insert element we review, we're getting closer to a better
	 * choice; update the compare field to its new value.  If we went past
	 * the last item in the list, return the last one: that is used to
	 * decide whether we are positioned in a skiplist.
	 */
	cbt->compare = -cmp;
	cbt->ins = (ins != NULL) ? ins : last_ins;
	return (0);
}

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_SESSION_IMPL *session,
    WT_ITEM *srch_key, WT_REF *leaf, WT_CURSOR_BTREE *cbt, int insert)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM *item;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	WT_REF *child, *parent;
	WT_ROW *rip;
	size_t match, skiphigh, skiplow;
	uint32_t base, indx, limit;
	int append, cmp, depth;

	btree = S2BT(session);
	item = &cbt->search_key;
	rip = NULL;
	match = 0;				/* -Wuninitialized */

	__cursor_pos_clear(cbt);

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
	if (leaf != NULL) {
		child = leaf;
		goto leaf_only;
	}

	/* Search the internal pages of the tree. */
	cmp = -1;
	parent = child = &btree->root;
	for (depth = 2;; ++depth) {
restart:	page = parent->page;
		if (page->type != WT_PAGE_ROW_INT)
			break;

		pindex = WT_INTL_INDEX_COPY(page);

		/*
		 * Fast-path internal pages with one child, a common case for
		 * the root page in new trees.
		 */
		if (pindex->entries == 1) {
			child = pindex->index[0];
			goto descend;
		}

		/* Fast-path appends. */
		if (insert && btree->appending) {
			child = pindex->index[pindex->entries - 1];
			__wt_ref_key(page, child, &item->data, &item->size);
			WT_ERR(WT_LEX_CMP(
			    session, btree->collator, srch_key, item, cmp));
			if (cmp >= 0)
				goto descend;

			btree->appending = 0;
		}

		/*
		 * Binary search of the internal page.  There are two versions
		 * (a default loop and an application-specified collation loop),
		 * because moving the collation test and error handling inside
		 * the loop costs about 5%.
		 *
		 * The 0th key on an internal page is a problem for a couple of
		 * reasons.  First, we have to force the 0th key to sort less
		 * than any application key, so internal pages don't have to be
		 * updated if the application stores a new, "smallest" key in
		 * the tree.  Second, reconciliation is aware of this and will
		 * store a byte of garbage in the 0th key, so the comparison of
		 * an application key and a 0th key is meaningless (but doing
		 * the comparison could still incorrectly modify our tracking
		 * of the leading bytes in each key that we can skip during the
		 * comparison).  For these reasons, skip the 0th key.
		 */
		base = 1;
		limit = pindex->entries - 1;
		if (btree->collator == NULL)
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				child = pindex->index[indx];
				__wt_ref_key(
				    page, child, &item->data, &item->size);

				match = WT_MIN(skiplow, skiphigh);
				cmp = __wt_lex_compare_skip(
				    srch_key, item, &match);
				if (cmp > 0) {
					skiplow = match;
					base = indx + 1;
					--limit;
				} else if (cmp < 0)
					skiphigh = match;
				else
					goto descend;
			}
		else
			for (; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				child = pindex->index[indx];
				__wt_ref_key(
				    page, child, &item->data, &item->size);

				WT_ERR(WT_LEX_CMP(session,
				    btree->collator, srch_key, item, cmp));
				if (cmp > 0) {
					base = indx + 1;
					--limit;
				} else if (cmp == 0)
					goto descend;
			}

		/*
		 * Set the slot to descend the tree: child is already set if
		 * there was an exact match on the page, otherwise, base is
		 * the smallest index greater than key, possibly (last + 1).
		 */
		child = pindex->index[base - 1];

descend:	/*
		 * Swap the parent page for the child page. If the page splits
		 * while we're retrieving it, restart the search in the parent
		 * page; otherwise return on error, the swap call ensures we're
		 * holding nothing on failure.
		 */
		switch (ret = __wt_page_swap(session, parent, child, 0)) {
		case 0:
			parent = child;
			break;
		case WT_RESTART:
			skiphigh = skiplow = 0;
			goto restart;
		default:
			return (ret);
		}
	}

	/* Track how deep the tree gets. */
	if (depth > btree->maximum_depth)
		btree->maximum_depth = depth;

leaf_only:
	page = child->page;
	cbt->ref = child;

	/*
	 * When inserting a new record, if there's a history of appending to
	 * a page, fast-check for a page append into an existing insert list.
	 * (We could do a more general page append check, but an insert list
	 * will exist as soon as any record is appended to the page, there's
	 * little point in optimizing for the first insert.)
	 */
	if (insert && btree->appending) {
		if (page->pg_row_entries == 0) {
			cbt->slot = WT_ROW_SLOT(page, page->pg_row_d);

			F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
			cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
		} else {
			cbt->slot = WT_ROW_SLOT(page,
			    page->pg_row_d + (page->pg_row_entries - 1));

			cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
		}

		WT_ERR(
		    __wt_search_insert_append(session, cbt, srch_key, &append));
		if (append)
			return (0);
		btree->appending = 0;

		/*
		 * Don't leave the insert list head set, code external to the
		 * search uses it.
		 */
		cbt->ins_head = NULL;
	}

	/*
	 * Binary search of the leaf page.  There are two versions (a default
	 * loop and an application-specified collation loop), because moving
	 * the collation test and error handling inside the loop costs about 5%.
	 */
	base = 0;
	limit = page->pg_row_entries;
	if (btree->collator == NULL)
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;
			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));

			match = WT_MIN(skiplow, skiphigh);
			cmp = __wt_lex_compare_skip(srch_key, item, &match);
			if (cmp > 0) {
				skiplow = match;
				base = indx + 1;
				--limit;
			} else if (cmp < 0)
				skiphigh = match;
			else
				goto leaf_match;
		}
	else
		for (; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rip = page->pg_row_d + indx;
			WT_ERR(__wt_row_leaf_key(session, page, rip, item, 1));

			WT_ERR(WT_LEX_CMP(
			    session, btree->collator, srch_key, item, cmp));
			if (cmp > 0) {
				base = indx + 1;
				--limit;
			} else if (cmp == 0)
				goto leaf_match;
		}

	/*
	 * The best case is finding an exact match in the leaf page's WT_ROW
	 * array, probable for any read-mostly workload.  Check that case and
	 * get out fast.
	 */
	if (0) {
leaf_match:	cbt->compare = 0;
		cbt->slot = WT_ROW_SLOT(page, rip);
		return (0);
	}

	/*
	 * We didn't find an exact match in the WT_ROW array.
	 *
	 * Base is the smallest index greater than key and may be the 0th index
	 * or the (last + 1) index.  Set the slot to be the largest index less
	 * than the key if that's possible (if base is the 0th index it means
	 * the application is inserting a key before any key found on the page).
	 *
	 * It's still possible there is an exact match, but it's on an insert
	 * list.  Figure out which insert chain to search and then set up the
	 * return information assuming we'll find nothing in the insert list
	 * (we'll correct as needed inside the search routine, depending on
	 * what we find).
	 *
	 * If inserting a key smaller than any key found in the WT_ROW array,
	 * use the extra slot of the insert array, otherwise the insert array
	 * maps one-to-one to the WT_ROW array.
	 */
	if (base == 0) {
		cbt->compare = 1;
		cbt->slot = WT_ROW_SLOT(page, page->pg_row_d);

		F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
		cbt->ins_head = WT_ROW_INSERT_SMALLEST(page);
	} else {
		cbt->compare = -1;
		cbt->slot = WT_ROW_SLOT(page, page->pg_row_d + (base - 1));

		cbt->ins_head = WT_ROW_INSERT_SLOT(page, cbt->slot);
	}

	/*
	 * We track when we've previously appended to a page and in that case,
	 * do a fast check for an append before the full page/skiplist search,
	 * both here and in the internal page search.
	 *
	 * A search of an internal page can only turn the append flag off: any
	 * search of an internal page that doesn't move past the end of the page
	 * turns the append flag off, so a search inside the tree always turns
	 * off fast-append checks.
	 *
	 * If the internal page search left the append flag on, we do the fast-
	 * append check before searching the leaf page.  If the check fails, we
	 * turn the append flag off, and proceed with the page/skiplist search.
	 *
	 * The code here turns the append flag on for future searches.  If we're
	 * doing an insert, there's no insert list and the page is empty, we're
	 * appending to the page (this is where we end up for empty pages).  If
	 * we ended up past the end of an insert list, past the end of a page,
	 * we're appending to the page.  In both of those cases, turn the append
	 * flag on.  Note we will turn the append flag on even if this leaf page
	 * is in the middle of a tree; it's wasted, a subsequent internal page
	 * search will turn it off again.
	 */
	if (WT_SKIP_FIRST(cbt->ins_head) == NULL) {
		cbt->ins = NULL;
		cbt->next_stack[0] = NULL;
		if (!insert)				/* Common path. */
			return (0);

		if (page->pg_row_entries == 0)
			btree->appending = 1;
	} else {
		WT_ERR(__wt_search_insert(session, cbt, srch_key));
		if (!insert)				/* Common path. */
			return (0);

		if (cbt->compare == -1 && base == page->pg_row_entries) {
			if (!btree->appending)
				btree->appending = 1;
		} else {
			if (btree->appending)
				btree->appending = 0;
		}
	}
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

	__cursor_pos_clear(cbt);

restart:
	/* Walk the internal pages of the tree. */
	parent = child = &btree->root;
	for (;;) {
		page = parent->page;
		if (page->type != WT_PAGE_ROW_INT)
			break;

		pindex = WT_INTL_INDEX_COPY(page);
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
		pindex = WT_INTL_INDEX_COPY(btree->root.page);
		cbt->slot = pindex->entries < 2 ?
		    __wt_random() % page->pg_row_entries : 0;

		return (__wt_row_leaf_key(session,
		    page, page->pg_row_d + cbt->slot, &cbt->search_key, 0));
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
