/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
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
	WT_CELL_UNPACK *unpack, _unpack;
	WT_IKEY *ikey;
	WT_ITEM *item, _item, *srch_key;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	uint32_t base, indx, limit;
	int cmp, ret;
	void *key;

	__cursor_search_clear(cbt);

	srch_key = (WT_ITEM *)&cbt->iface.key;

	btree = session->btree;
	unpack = &_unpack;
	rip = NULL;

	cmp = -1;				/* Assume we don't match. */

	/* Search the internal pages of the tree. */
	item = &_item;
	for (page = btree->root_page.page; page->type == WT_PAGE_ROW_INT;) {
		/* Binary search of internal pages. */
		for (base = 0, rref = NULL,
		    limit = page->entries; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rref = page->u.row_int.t + indx;

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 */
			if (indx != 0) {
				ikey = rref->key;
				item->data = WT_IKEY_DATA(ikey);
				item->size = ikey->size;

				WT_RET(WT_BTREE_CMP(
				    session, btree, srch_key, item, cmp));
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}
		WT_ASSERT(session, rref != NULL);

		/*
		 * Reference the slot used for next step down the tree.
		 *
		 * Base is the smallest index greater than key and may be the
		 * (last + 1) index.  (Base cannot be the 0th index as the 0th
		 * index always sorts less than any application key).  The slot
		 * for descent is the one before base.
		 */
		if (cmp != 0)
			rref = page->u.row_int.t + (base - 1);

		/* Swap the parent page for the child page. */
		WT_ERR(__wt_page_in(session, page, &rref->ref, 0));
		__wt_page_release(session, page);
		page = WT_ROW_REF_PAGE(rref);
	}

	/*
	 * Copy the leaf page's write generation value before reading the page.
	 * Use a read memory barrier to ensure we read the value before we read
	 * any of the page's contents.
	 */
	if (is_modify)
		WT_ORDERED_READ(cbt->write_gen, page->write_gen);
	cbt->page = page;

	/* Do a binary search of the leaf page. */
	for (base = 0, limit = page->entries; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row_leaf.d + indx;

retry:		/*
		 * Multiple threads of control may be searching this page, which
		 * means the key may change underfoot, and here's where it gets
		 * tricky: first, copy the key.  We don't need any barriers, the
		 * key is updated atomically, and we just need a valid copy.
		 */
		key = rip->key;

		/*
		 * Key copied.
		 *
		 * If another thread instantiated the key, we don't have any
		 * work to do.  Figure this out using the key's value:
		 *
		 * If the key points off-page, the key has been instantiated,
		 * just use it.
		 *
		 * If the key points on-page, we have a copy of a WT_CELL value
		 * that can be processed, regardless of what any other thread is
		 * doing.
		 */
		if (__wt_off_page(page, key)) {
			ikey = key;
			_item.data = WT_IKEY_DATA(ikey);
			_item.size = ikey->size;
			item = &_item;
		} else {
			if (btree->huffman_key != NULL) {
				WT_ERR(__wt_row_key(session, page, rip, NULL));
				goto retry;
			}
			__wt_cell_unpack(key, unpack);
			if (unpack->type == WT_CELL_KEY &&
			    unpack->prefix == 0) {
				_item.data = unpack->data;
				_item.size = unpack->size;
				item = &_item;
			} else {
				WT_ERR(__wt_row_key(session, page, rip, NULL));
				goto retry;
			}
		}

		WT_RET(WT_BTREE_CMP(session, btree, srch_key, item, cmp));
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
	rip = page->u.row_leaf.d;
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
	cbt->ins = __wt_search_insert(session, cbt, cbt->ins_head, srch_key);
	return (0);

err:	__wt_page_release(session, page);
	return (ret);
}
