/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static inline int __wt_key_build(WT_TOC *, WT_ROW *);

/*
 * __wt_key_build --
 *	Instantiate an overflow or compressed key into a WT_ROW structure.
 */
static inline int
__wt_key_build(WT_TOC *toc, WT_ROW *rip_arg)
{
	DBT *dbt, _dbt;
	WT_ITEM *item;

	WT_CLEAR(_dbt);
	dbt = &_dbt;

	item = rip_arg->key;
	WT_RET(__wt_item_process(toc, item, dbt));

	/* Update the WT_ROW reference with the processed key. */
	__wt_key_set(rip_arg, dbt->data, dbt->size);

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
	WT_OFF *off;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_REF *ref;
	WT_ROW *rip, *t;
	WT_REPL *repl;
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
		isleaf = dsk->type == WT_PAGE_ROW_LEAF ? 1 : 0;
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			rip = page->u.irow + indx;
			if (__wt_key_process(rip))
				WT_ERR(__wt_key_build(toc, rip));

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
			if (indx != 0 || isleaf) {
				cmp = db->btree_compare(db, key, (DBT *)rip);
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}

		/*
		 * Reference the slot used for next step down the tree.  We do
		 * this on leaf pages too, because it's simpler to code, and we
		 * only care if there's an exact match on leaf pages; setting
		 * rip doesn't matter for leaf pages because we always return
		 * WT_NOTFOUND if there's no match.
		 *
		 * Base is the smallest index greater than key and may be the
		 * 0th index or the (last + 1) indx.  If base is not the 0th
		 * index (remember, the 0th index always sorts less than any
		 * application key), decrement it to the smallest index less
		 * than or equal to key.
		 */
		if (cmp != 0)
			rip = page->u.irow + (base == 0 ? 0 : base - 1);

		/*
		 * If we've reached the leaf page, or we've reached the level
		 * requested by our caller, we're done.
		 */
		if (isleaf || level == dsk->level)
			break;

deleted_retry:	/* rip references the subtree containing the record. */
		ref = WT_ROW_REF(page, rip);
		off = WT_ROW_OFF(rip);
		switch (ret = __wt_page_in(toc, page, ref, off, 0)) {
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
			for (t = rip,
			    indx = WT_ROW_SLOT(page, rip);
			    indx > 0; --indx, --t)
				if (WT_ROW_OFF(t)->addr != WT_ADDR_DELETED) {
					rip = t;
					goto deleted_retry;
				}
			for (t = rip,
			    indx = WT_ROW_SLOT(page, rip);
			    indx < page->indx_count; ++indx, ++t)
				if (WT_ROW_OFF(t)->addr != WT_ADDR_DELETED) {
					rip = t;
					goto deleted_retry;
				}
			ret = WT_RESTART;
			/* FALLTHROUGH */
		default:
			goto err;
		}
		page = ref->page;
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
