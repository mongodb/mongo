/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_row_ins_search --
 *	Search the slot's insert list.
 */
static inline int
__insert_search(
    WT_SESSION_IMPL *session, WT_INSERT_HEAD *inshead, WT_ITEM *key)
{
	WT_BTREE *btree;
	WT_INSERT **ins;
	WT_ITEM insert_key;
	int cmp, i, (*compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	/* If there's no insert chain to search, we're done. */
	if (inshead == NULL)
		return (1);

	btree = session->btree;
	compare = btree->btree_compare;

	/*
	 * The insert list is a skip list: start at the highest skip level,
	 * then go as far as possible at each level before stepping down to the
	 * next one.
	 */
	for (i = WT_SKIP_MAXDEPTH - 1, ins = &inshead->head[i]; i >= 0; ) {
		if (*ins == NULL)
			cmp = -1;
		else {
			insert_key.data = WT_INSERT_KEY(*ins);
			insert_key.size = WT_INSERT_KEY_SIZE(*ins);
			cmp = compare(btree, key, &insert_key);
		}
		if (cmp == 0) {
			WT_CLEAR(session->srch.ins);
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
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_SESSION_IMPL *session, WT_ITEM *key, uint32_t flags)
{
	WT_BTREE *btree;
	WT_IKEY *ikey;
	WT_INSERT_HEAD *inshead;
	WT_ITEM *item, _item;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	uint32_t base, indx, limit, slot, write_gen;
	int cmp, ret;
	int (*compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	session->srch.page = NULL;			/* Return values. */
	session->srch.write_gen = 0;
	session->srch.match = 0;
	session->srch.ip = NULL;
	session->srch.vupdate = NULL;
	session->srch.inshead = NULL;
	WT_CLEAR(session->srch.ins);
	session->srch.upd = NULL;
	session->srch.slot = UINT32_MAX;

	btree = session->btree;
	item = &_item;
	rip = NULL;
	compare = btree->btree_compare;

	cmp = -1;				/* Assume we don't match. */

	/* Search the tree. */
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

				cmp = compare(btree, key, item);
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
		if (page != btree->root_page.page)
			__wt_hazard_clear(session, page);
		page = WT_ROW_REF_PAGE(rref);
	}

	/*
	 * Copy the page's write generation value before reading anything on
	 * the page.
	 */
	write_gen = page->write_gen;

	/*
	 * There are 4 pieces of information regarding updates and inserts
	 * that are set in the next few lines of code.
	 *
	 * For an update, we set session->srch.upd and session->srch.slot.
	 * For an insert, we set session->srch.ins and session->srch.slot.
	 * For an exact match, we set session->srch.vupdate.
	 *
	 * The session->srch.slot only serves a single purpose, indicating the
	 * slot in the WT_ROW array where a new update/insert entry goes when
	 * entering the first such item for the page (that is, the slot to use
	 * when allocating the update/insert array itself).
	 *
	 * In other words, we would like to pass back to our caller a pointer
	 * to a pointer to an update/insert structure, in front of which our
	 * caller will insert a new update or insert structure.  The problem is
	 * if the update/insert arrays don't yet exist, in which case we have
	 * to return the WT_ROW array slot information so our caller can first
	 * allocate the update/insert array, and then figure out which slot to
	 * use.
	 *
	 * Do a binary search of the leaf page.
	 */
	for (base = 0, limit = page->entries; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row_leaf.d + indx;

		/* The key may not have been instantiated yet. */
		if (__wt_off_page(page, rip->key)) {
			ikey = rip->key;
			_item.data = WT_IKEY_DATA(ikey);
			_item.size = ikey->size;
			item = &_item;
		} else {
			WT_ERR(
			    __wt_row_key(session, page, rip, &btree->key_srch));
			item = (WT_ITEM *)&btree->key_srch;
		}

		cmp = compare(btree, key, item);
		if (cmp == 0)
			break;
		if (cmp < 0)
			continue;

		base = indx + 1;
		--limit;
	}

	/*
	 * If we found a match in the page on-disk information, set the return
	 * information, we're done.
	 */
	if (cmp == 0) {
		WT_ASSERT(session, rip != NULL);
		session->srch.slot = slot = WT_ROW_SLOT(page, rip);
		if (page->u.row_leaf.upd != NULL) {
			session->srch.upd = &page->u.row_leaf.upd[slot];
			session->srch.vupdate = page->u.row_leaf.upd[slot];
		}
		goto done;
	}

	/*
	 * No match found.
	 *
	 * Base is the smallest index greater than key and may be the 0th index
	 * or the (last + 1) index.  Set the WT_ROW reference to be the largest
	 * index less than the key if that's possible (if base is the 0th index
	 * it means the application is inserting a key before any key found on
	 * the page).
	 */
	rip = page->u.row_leaf.d;
	if (base != 0)
		rip += base - 1;

	/*
	 * Figure out which insert chain to search, and do initial setup of the
	 * return information for the insert chain (we'll correct it as needed
	 * depending on what we find.)
	 *
	 * If inserting a key smaller than any from-disk key found on the page,
	 * use the extra slot of the insert array, otherwise use the usual
	 * one-to-one mapping.
	 */
	if (base == 0) {
		inshead = WT_ROW_INSERT_SMALLEST(page);
		session->srch.slot = page->entries;
	} else {
		inshead = WT_ROW_INSERT(page, rip);
		session->srch.slot = WT_ROW_SLOT(page, rip);
	}
	if (page->u.row_leaf.ins != NULL)
		session->srch.inshead =
		    &page->u.row_leaf.ins[session->srch.slot];

	/*
	 * Search the insert tree for a match -- if we don't find a match, we
	 * fail, unless we're inserting new data.
	 *
	 * No matter how things turn out, __wt_row_ins_search resets
	 * session->srch appropriately, there's no more work to be done.
	 */
	if ((cmp = __insert_search(session, inshead, key)) != 0) {
		/*
		 * No match found.
		 * If not doing an insert, we've failed.
		 */
		if (!LF_ISSET(WT_WRITE))
			goto notfound;
	}

done:	/*
	 * If we found a match and it's not an insert operation, review any
	 * updates to the key's value: a deleted object returns not-found.
	 */
	if (!LF_ISSET(WT_WRITE) &&
	    session->srch.upd != NULL &&
	    *session->srch.upd != NULL &&
	    WT_UPDATE_DELETED_ISSET(*session->srch.upd))
		goto notfound;

	session->srch.page = page;
	session->srch.write_gen = write_gen;
	session->srch.match = (cmp == 0);
	session->srch.ip = rip;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	__wt_page_release(session, page);
	return (ret);
}
