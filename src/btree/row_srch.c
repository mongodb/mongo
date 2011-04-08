/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static inline int __wt_ins_search(SESSION *, WT_INSERT *, WT_ITEM *);

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(SESSION *session, WT_ITEM *key, uint32_t flags)
{
	BTREE *btree;
	WT_INSERT *ins;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	uint32_t base, indx, limit, slot, write_gen;
	int cmp, ret;

	session->srch_page = NULL;			/* Return values. */
	session->srch_write_gen = 0;
	session->srch_match = 0;
	session->srch_ip = NULL;
	session->srch_vupdate = NULL;
	session->srch_ins = NULL;
	session->srch_upd = NULL;
	session->srch_slot = UINT32_MAX;

	/* Assume we don't match in case we're searching an empty tree. */
	cmp = -1;
	btree = session->btree;
	rip = NULL;
	rref = NULL;

	WT_DB_FCHK(btree,
	    "__wt_row_search", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	/* Search the tree. */
	for (page = btree->root_page.page; page->type == WT_PAGE_ROW_INT;) {
		/* Binary search of internal pages. */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);
			rref = page->u.row_int.t + indx;

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			if (__wt_key_process(rref))
				WT_ERR(
				    __wt_key_build(session, page, rref, NULL));

			/*
			 * If we're about to compare an application key with the
			 * 0th index on an internal page, pretend the 0th index
			 * sorts less than any application key.  This test is so
			 * we don't have to update internal pages if the
			 * application stores a new, "smallest" key in the tree.
			 *
			 * For the record, we still maintain the key at the 0th
			 * location because it means tree verification and other
			 * code that processes a level of the tree doesn't need
			 * to know about this hack.
			 */
			if (indx != 0) {
				cmp = btree->btree_compare(
				    btree, key, (WT_ITEM *)rref);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}

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

		WT_ASSERT(session, rref != NULL);

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
	 * For an update, we set session->srch_upd and session->srch_slot.
	 * For an insert, we set session->srch_ins and session->srch_slot.
	 * For an exact match, we set session->srch_vupdate.
	 *
	 * The session->srch_slot only serves a single purpose, indicating the
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
	for (base = 0, limit = page->indx_count; limit != 0; limit >>= 1) {
		indx = base + (limit >> 1);
		rip = page->u.row_leaf.d + indx;

		/*
		 * If the key is compressed or an overflow, it may not have
		 * been instantiated yet.
		 */
		if (__wt_key_process(rip))
			WT_ERR(__wt_key_build(session, page, rip, NULL));

		cmp = btree->btree_compare(btree, key, (WT_ITEM *)rip);
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
		session->srch_slot = slot = WT_ROW_INDX_SLOT(page, rip);
		if (page->u.row_leaf.upd != NULL) {
			session->srch_upd = &page->u.row_leaf.upd[slot];
			session->srch_vupdate = page->u.row_leaf.upd[slot];
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
	 *
	 * Figure out which insert chain to search, and do initial setup of the
	 * return information for the insert chain (we'll correct it as needed
	 * depending on what we find.)
	 *
	 * If inserting a key smaller than any from-disk key found on the page,
	 * use the extra slot of the insert array, otherwise use the usual
	 * one-to-one mapping.
	 */
	if (base == 0) {
		ins = WT_ROW_INSERT_SMALLEST(page);
		session->srch_slot = page->indx_count;
	} else {
		ins = WT_ROW_INSERT(page, rip);
		session->srch_slot = WT_ROW_INDX_SLOT(page, rip);
	}
	if (page->u.row_leaf.ins != NULL)
		session->srch_ins = &page->u.row_leaf.ins[session->srch_slot];

	/*
	 * If there's no insert chain to search, we're done.
	 *
	 * If not doing an insert, we've failed.
	 * If doing an insert, srch_slot and srch_ins have been set, we're done.
	 */
	if (ins == NULL) {
		if (!LF_ISSET(WT_WRITE))
			goto notfound;
	} else {
		/*
		 * Search the insert tree for a match -- if we don't find a
		 * match, we fail, unless we're inserting new data.
		 *
		 * No matter how things turn out, __wt_ins_search resets the
		 * session->srch_XXX fields appropriately, there's no more
		 * work to be done.
		 */
		if ((cmp = __wt_ins_search(session, ins, key)) != 0) {
			/*
			 * No match found.
			 * If not doing an insert, we've failed.
			 */
			if (!LF_ISSET(WT_WRITE))
				goto notfound;
		}
	}

done:	/*
	 * If we found a match and it's not an insert operation, review any
	 * updates to the key's value: a deleted object returns not-found.
	 */
	if (!LF_ISSET(WT_WRITE) &&
	    session->srch_upd != NULL &&
	    *session->srch_upd != NULL &&
	    WT_UPDATE_DELETED_ISSET(*session->srch_upd))
		goto notfound;

	session->srch_page = page;
	session->srch_write_gen = write_gen;
	session->srch_match = cmp == 0 ? 1 : 0;
	session->srch_ip = rip;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	WT_PAGE_OUT(session, page);
	return (ret);
}

/*
 * __wt_ins_search --
 *	Search the slot's insert list.
 */
static inline int
__wt_ins_search(SESSION *session, WT_INSERT *ins, WT_ITEM *key)
{
	WT_ITEM insert_key;
	BTREE *btree;
	int cmp;

	btree = session->btree;

	/*
	 * The insert list is a sorted, forward-linked list -- on average, we
	 * have to search half of it.
	 */
	for (; ins != NULL; ins = ins->next) {
		insert_key.data = WT_INSERT_KEY(ins);
		insert_key.size = WT_INSERT_KEY_SIZE(ins);
		cmp = btree->btree_compare(btree, key, &insert_key);
		if (cmp == 0) {
			session->srch_ins = NULL;
			session->srch_vupdate = ins->upd;
			session->srch_upd = &ins->upd;
			return (0);
		}
		if (cmp < 0)
			break;
		session->srch_ins = &ins->next;
	}
	return (1);
}
