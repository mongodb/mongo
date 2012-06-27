/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_search_insert --
 *	Search a row-store insert list, creating a skiplist stack as we go.
 */
WT_INSERT *
__wt_search_insert(WT_SESSION_IMPL *session,
    WT_CURSOR_BTREE *cbt, WT_INSERT_HEAD *inshead, WT_ITEM *srch_key)
{
	WT_BTREE *btree;
	WT_INSERT **insp, *ret_ins;
	WT_ITEM insert_key;
	int cmp, i;

	/* If there's no insert chain to search, we're done. */
	if ((ret_ins = WT_SKIP_LAST(inshead)) == NULL)
		return (NULL);

	btree = session->btree;

	/* Fast-path appends. */
	insert_key.data = WT_INSERT_KEY(ret_ins);
	insert_key.size = WT_INSERT_KEY_SIZE(ret_ins);
	(void)WT_BTREE_CMP(session, btree, srch_key, &insert_key, cmp);
	if (cmp >= 0) {
		for (i = WT_SKIP_MAXDEPTH - 1; i >= 0; i--)
			cbt->ins_stack[i] = (inshead->tail[i] != NULL) ?
			    &inshead->tail[i]->next[i] :
			    &inshead->head[i];
		cbt->compare = -cmp;
		return (ret_ins);
	}

	/*
	 * The insert list is a skip list: start at the highest skip level, then
	 * go as far as possible at each level before stepping down to the next.
	 */
	ret_ins = NULL;
	for (i = WT_SKIP_MAXDEPTH - 1, insp = &inshead->head[i]; i >= 0; ) {
		if (*insp == NULL) {
			cbt->ins_stack[i--] = insp--;
			continue;
		}

		/*
		 * Comparisons may be repeated as we drop down skiplist levels;
		 * don't repeat comparisons, they might be expensive.
		 */
		if (ret_ins != *insp) {
			ret_ins = *insp;
			insert_key.data = WT_INSERT_KEY(ret_ins);
			insert_key.size = WT_INSERT_KEY_SIZE(ret_ins);
			(void)WT_BTREE_CMP(session, btree,
			    srch_key, &insert_key, cmp);
		}

		if (cmp > 0)		/* Keep going at this level */
			insp = &ret_ins->next[i];
		else if (cmp == 0)
			for (; i >= 0; i--)
				cbt->ins_stack[i] = &ret_ins->next[i];
		else			/* Drop down a level */
			cbt->ins_stack[i--] = insp--;
	}

	/*
	 * For every insert element we review, we're getting closer to a better
	 * choice; update the compare field to its new value.
	 */
	cbt->compare = -cmp;
	return (ret_ins);
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
	WT_IKEY *ikey;
	WT_ITEM *item, _item, *srch_key;
	WT_PAGE *page;
	WT_REF *ref;
	WT_ROW *rip;
	uint32_t base, indx, limit;
	int cmp;

	__cursor_search_clear(cbt);

	srch_key = &cbt->iface.key;

	btree = session->btree;
	rip = NULL;

	cmp = -1;				/* Assume we don't match. */

	/* Search the internal pages of the tree. */
	item = &_item;
	for (page = btree->root_page; page->type == WT_PAGE_ROW_INT;) {
		/* Binary search of internal pages. */
		for (base = 0, ref = NULL,
		    limit = page->entries; limit != 0; limit >>= 1) {
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
				ikey = ref->u.key;
				item->data = WT_IKEY_DATA(ikey);
				item->size = ikey->size;

				WT_ERR(WT_BTREE_CMP(
				    session, btree, srch_key, item, cmp));
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}
		WT_ASSERT(session, ref != NULL);

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

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, ref));
		__wt_page_release(session, page);
		page = ref->page;
	}

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

	/* Do a binary search of the leaf page. */
	for (base = 0, limit = page->entries; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row.d + indx;

		WT_ERR(__wt_row_key(session, page, rip, item, 1));
		WT_ERR(WT_BTREE_CMP(session, btree, srch_key, item, cmp));
		if (cmp == 0)
			break;
		if (cmp < 0)
			continue;

		base = indx + 1;
		--limit;
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
	cbt->ins = __wt_search_insert(session, cbt, cbt->ins_head, srch_key);
	return (0);

err:	__wt_page_release(session, page);
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

	btree = session->btree;

	/* Walk the internal pages of the tree. */
	for (page = btree->root_page; page->type == WT_PAGE_ROW_INT;) {
		ref = page->u.intl.t + __wt_random() % page->entries;

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, ref));
		__wt_page_release(session, page);
		page = ref->page;
	}

	cbt->page = page;
	cbt->compare = 0;

	if (page->entries != 0) {
		/*
		 * The use case for this call is finding a place to split the
		 * tree.  Cheat (it's not like this is "random", anyway), and
		 * make things easier by returning the first key on the page.
		 * If the caller is attempting to split a newly created tree,
		 * or a tree with just one big page, that's not going to work,
		 * check for that.
		 */
		cbt->slot =
		    btree-> root_page->entries < 2 ?
		    __wt_random() % page->entries : 0;
		return (0);
	}

	/*
	 * If the tree is new (and not empty), it might have a large insert
	 * list, pick the key in the middle of that insert list.
	 */
	F_SET(cbt, WT_CBT_SEARCH_SMALLEST);
	if ((cbt->ins_head = WT_ROW_INSERT_SMALLEST(page)) == NULL)
		return (WT_NOTFOUND);
	for (p = t = WT_SKIP_FIRST(cbt->ins_head);;) {
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		if ((p = WT_SKIP_NEXT(p)) == NULL)
			break;
		t = WT_SKIP_NEXT(t);
	}
	cbt->ins = t;

	return (0);

err:	__wt_page_release(session, page);
	return (ret);
}
