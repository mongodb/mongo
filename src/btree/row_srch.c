/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

static inline int __wt_key_build(SESSION *, WT_PAGE *, void *);

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(SESSION *session, WT_ITEM *key, uint32_t flags)
{
	BTREE *btree;
	WT_PAGE *page;
	WT_ROW *rip;
	WT_ROW_REF *rref;
	WT_UPDATE *upd;
	uint32_t base, indx, limit, write_gen;
	int cmp, ret;

	session->srch_page = NULL;			/* Return values. */
	session->srch_ip = NULL;
	session->srch_upd = NULL;
	session->srch_exp = NULL;
	session->srch_write_gen = 0;

	cmp = 0;
	btree = session->btree;
	rip = NULL;

	WT_DB_FCHK(btree,
	    "__wt_row_search", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	/* Search the tree. */
	for (page = btree->root_page.page;;) {
		/*
		 * Copy the page's write generation value before reading
		 * anything on the page.
		 */
		write_gen = page->write_gen;

		switch (page->type) {
		case WT_PAGE_ROW_INT:
			for (base = 0,
			    limit = page->indx_count; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				rref = page->u.row_int.t + indx;

				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				if (__wt_key_process(rref))
					WT_ERR(__wt_key_build(
					    session, page, rref));

				/*
				 * If we're about to compare an application key
				 * with the 0th index on an internal page,
				 * pretend the 0th index sorts less than any
				 * application key.  This test is so we don't
				 * have to update internal pages if the
				 * application stores a new, "smallest" key in
				 * the tree.
				 *
				 * For the record, we still maintain the key at
				 * the 0th location because it means tree
				 * verification and other code that processes a
				 * level of the tree doesn't need to know about
				 * this hack.
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
			 * Base is the smallest index greater than key and may
			 * be the 0th index or the (last + 1) indx.  If base is
			 * not the 0th index (remember, the 0th index always
			 * sorts less than any application key), decrement it
			 * to the smallest index less than or equal to key.
			 */
			if (cmp != 0)
				rref = page->u.row_int.t +
				    (base == 0 ? 0 : base - 1);
			break;
		case WT_PAGE_ROW_LEAF:
			for (base = 0,
			    limit = page->indx_count; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				rip = page->u.row_leaf.d + indx;

				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				if (__wt_key_process(rip))
					WT_ERR(
					    __wt_key_build(session, page, rip));

				cmp = btree->btree_compare(
				    btree, key, (WT_ITEM *)rip);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;

				base = indx + 1;
				--limit;
			}
			goto done;
		}

		/* rref references the subtree containing the record. */
		WT_ERR(__wt_page_in(session, page, &rref->ref, 0));

		/* Swap the parent page for the child page. */
		if (page != btree->root_page.page)
			__wt_hazard_clear(session, page);
		page = WT_ROW_REF_PAGE(rref);
	}

done:	/*
	 * We've got the right on-page WT_ROW structure (an exact match in the
	 * case of a lookup, or the smallest key on the page less than or equal
	 * to the specified key in the case of an insert).
	 */
	if (!LF_ISSET(WT_INSERT)) {
		if (cmp != 0)				/* No match */
			goto notfound;
							/* Deleted match. */
		if ((upd = WT_ROW_UPDATE(page, rip)) != NULL) {
			if (WT_UPDATE_DELETED_ISSET(upd))
				goto notfound;
			session->srch_upd = upd;
		}
	}

	session->srch_page = page;
	session->srch_ip = rip;
	session->srch_write_gen = write_gen;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	WT_PAGE_OUT(session, page);
	return (ret);
}

/*
 * __wt_key_build --
 *	Instantiate an overflow or compressed key into a WT_ROW structure.
 */
static int
__wt_key_build(SESSION *session, WT_PAGE *page, void *key_arg)
{
	WT_BUF tmp;
	WT_CELL *cell;
	WT_ROW *key;
	int ret;

	WT_CLEAR(tmp);

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	key = key_arg;
	cell = key->key;

	/*
	 * Multiple threads of control may be searching this page, which means
	 * we have to serialize instantiating this key, and here's where it
	 * gets tricky.  A few instructions ago we noted the key size was 0,
	 * which meant the key required processing, and we just copied the key.
	 * If another thread instantiated the key while we were doing that,
	 * then the key may have already been instantiated, otherwise, we still
	 * need to proceed.
	 *
	 * We don't want the serialization function to call malloc, which means
	 * we want to instantiate the key here, and only call the serialization
	 * function to swap the key into place.  Check the pointer -- if it's
	 * off-page, we have a key that can be processed, regardless of what any
	 * other thread is doing.   If it's on-page, we raced and we're done.
	 */
	if (__wt_ref_off_page(page, cell))
		return (0);

	/* Instantiate the key. */
	WT_RET(__wt_cell_process(session, cell, &tmp));

	/* Serialize the swap of the key into place. */
	__wt_key_build_serial(session, key_arg, &tmp, ret);

	/* If the workQ didn't use our buffer's memory for the key, free it. */
	if (key->key != tmp.item.data)
		__wt_free(session, tmp.mem);

	return (ret);
}

/*
 * __wt_key_build_serial_func --
 *	Server function to instantiate a key during a row-store search.
 */
int
__wt_key_build_serial_func(SESSION *session)
{
	WT_BUF *tmp;
	WT_ROW *key;
	void *key_arg;

	__wt_key_build_unpack(session, key_arg, tmp);

	/*
	 * We don't care about the page's write generation -- there's a simpler
	 * test, if the key we're interested in still needs to be instantiated,
	 * because it can only be in one of two states.
	 *
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	key = key_arg;
	if (__wt_key_process(key)) {
		/*
		 * Update the key, flush memory, and then update the size.  Done
		 * in that order so any other thread is guaranteed to either see
		 * a size of 0 (indicating the key needs processing, which means
		 * we'll resolve it all here), or see a non-zero size and valid
		 * pointer pair.
		 */
		key->key = tmp->item.data;
		WT_MEMORY_FLUSH;
		key->size = tmp->item.size;
	}

	__wt_session_serialize_wrapup(session, NULL, 0);
	return (0);
}
