/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static inline int __wt_key_build(WT_TOC *, void *);

/*
 * __wt_key_build --
 *	Instantiate an overflow or compressed key into a WT_ROW structure.
 */
static inline int
__wt_key_build(WT_TOC *toc, void *ref)
{
	DBT *dbt, _dbt;
	WT_ITEM *item;

	WT_CLEAR(_dbt);
	dbt = &_dbt;

	/*
	 * Passed both WT_ROW_REF and WT_ROW structures; the first two fields
	 * of the structures are a void *data/uint32_t size pair.
	 */
	item = ((WT_ROW *)ref)->key;
	WT_RET(__wt_item_process(toc, item, dbt));

	/* Update the WT_ROW reference with the processed key. */
	__wt_key_set(ref, dbt->data, dbt->size);

	return (0);
}

/*
 * __wt_row_search --
 *	Search a row-store tree for a specific key.
 */
int
__wt_row_search(WT_TOC *toc, DBT *key, uint32_t level, uint32_t flags)
{
	DB *db;
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_REPL *repl;
	WT_ROW *rip;
	WT_ROW_REF *rref, *t;
	uint32_t base, indx, limit, write_gen;
	int cmp, isleaf, ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;
	toc->srch_repl = NULL;
	toc->srch_exp = NULL;
	toc->srch_write_gen = 0;

	cmp = 0;
	db = toc->db;
	idb = db->idb;
	rip = NULL;

	WT_DB_FCHK(db, "__wt_row_search", flags, WT_APIMASK_BT_SEARCH_KEY_ROW);

	/* Search the tree. */
	for (page = idb->root_page.page;;) {
		/*
		 * Copy the page's write generation value before reading
		 * anything on the page.
		 */
		write_gen = page->write_gen;

		dsk = page->dsk;
		switch (dsk->type) {
		case WT_PAGE_ROW_INT:
			isleaf = 0;
			for (base = 0,
			    limit = page->indx_count; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				rref = page->u.row_int.t + indx;

				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				if (__wt_key_process(rref))
					WT_ERR(__wt_key_build(toc, rref));

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
					cmp = db->
					    btree_compare(db, key, (DBT *)rref);
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
			isleaf = 1;
			for (base = 0,
			    limit = page->indx_count; limit != 0; limit >>= 1) {
				indx = base + (limit >> 1);
				rip = page->u.row_leaf.d + indx;

				/*
				 * If the key is compressed or an overflow, it
				 * may not have been instantiated yet.
				 */
				if (__wt_key_process(rip))
					WT_ERR(__wt_key_build(toc, rip));

				cmp = db->btree_compare(db, key, (DBT *)rip);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;

				base = indx + 1;
				--limit;
			}
			break;
		}

		/*
		 * If we've reached the leaf page, or we've reached the level
		 * requested by our caller, we're done.
		 */
		if (isleaf || level == dsk->level)
			break;

deleted_retry:	/* rref references the subtree containing the record. */
		switch (ret =
		    __wt_page_in(toc, page, &rref->ref, rref->off, 0)) {
		case 0:				/* Valid page */
			/* Swap the parent page for the child page. */
			if (page != idb->root_page.page)
				__wt_hazard_clear(toc, page);
			break;
		case WT_PAGE_DELETED:
			/*
			 * !!!
			 * See __wt_rec_page_delete() for an explanation of page
			 * deletion.  In this code, we first do an easy test --
			 * if we're a reader, we're done because we know the
			 * key/data pair doesn't exist.
			 */
			if (!LF_ISSET(WT_INSERT))
				goto notfound;
			/*
			 * If we're a writer, there are 3 steps: (1) move to a
			 * lower valid page entry; (2) if that isn't possible,
			 * move to a larger valid page entry; (3) if that isn't
			 * possible, restart the operation.
			 */
			for (t = rref,
			    indx = WT_ROW_REF_SLOT(page, rref);
			    indx > 0; --indx, --t)
				if (WT_ROW_REF_ADDR(t) != WT_ADDR_DELETED) {
					rref = t;
					goto deleted_retry;
				}
			for (t = rref,
			    indx = WT_ROW_REF_SLOT(page, rref);
			    indx < page->indx_count; ++indx, ++t)
				if (WT_ROW_REF_ADDR(t) != WT_ADDR_DELETED) {
					rref = t;
					goto deleted_retry;
				}
			ret = WT_RESTART;
			/* FALLTHROUGH */
		default:
			goto err;
		}
		page = WT_ROW_REF_PAGE(rref);
	}

	/*
	 * We've got the right on-page WT_ROW structure (an exact match in the
	 * case of a lookup, or the smallest key on the page less than or equal
	 * to the specified key in the case of an insert).   If it's an insert,
	 * we're done, return the information.   Otherwise, check to see if the
	 * item was modified/deleted.
	 */
	switch (dsk->type) {
	case WT_PAGE_ROW_LEAF:
		if (LF_ISSET(WT_INSERT))
			break;
		if (cmp != 0)				/* No match */
			goto notfound;
							/* Deleted match. */
		if ((repl = WT_ROW_REPL(page, rip)) != NULL) {
			if (WT_REPL_DELETED_ISSET(repl))
				goto notfound;
			toc->srch_repl = repl;
		}
		break;
	case WT_PAGE_ROW_INT:
		/*
		 * When returning internal pages, set the item's WT_REPL slot
		 * if it exists, otherwise we're done.
		 */
		toc->srch_repl = WT_ROW_REPL(page, rip);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	toc->srch_page = page;
	toc->srch_ip = rip;
	toc->srch_write_gen = write_gen;
	return (0);

notfound:
	ret = WT_NOTFOUND;

err:	WT_PAGE_OUT(toc, page);
	return (ret);
}
