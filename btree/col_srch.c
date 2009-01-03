/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_btree_search(DB *, DBT *, WT_PAGE **, WT_INDX **);

/*
 * __wt_db_get --
 *	Get a key/data pair.
 */
int
__wt_db_get(DB *db, DBT *key, DBT *pkey, DBT *data, u_int32_t flags)
{
	WT_PAGE *page;
	WT_INDX *indx;
	WT_ITEM *item;
	int ret, tret;

	DB_FLAG_CHK(db, "Db.get", flags, WT_APIMASK_DB_GET);

	WT_ASSERT(db->ienv, pkey == NULL);

	/* Search the primary btree for the key. */
	if ((ret = __wt_btree_search(db, key, &page, &indx)) != 0)
		return (ret);

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	item = indx->ditem;
	if (item->type != WT_ITEM_DATA && item->type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method to return keys "
		    "with duplicate data items");
		goto err;
	}

	ret = __wt_dbt_return(db, data, page, indx);

	if (0) {
err:		ret = WT_ERROR;
	}
	if ((tret = __wt_db_page_out(db, page, 0)) != 0 && ret == 0)
		ret = tret;
	return (ret);

}

/*
 * __wt_btree_search --
 *	Search the tree for a specific key.
 */
static int
__wt_btree_search(DB *db, DBT *key, WT_PAGE **pagep, WT_INDX **indxp)
{
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, ret;

	idb = db->idb;

	/* Check for an empty tree. */
	if ((addr = idb->root_addr) == WT_ADDR_INVALID)
		return (WT_NOTFOUND);

	/* Search the tree. */
	for (;;) {
		if ((ret = __wt_db_page_in(
		    db, addr, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
			return (ret);
		hdr = page->hdr;
		isleaf = hdr->type == WT_PAGE_LEAF ||
		    hdr->type == WT_PAGE_DUP_LEAF ? 1 : 0;

		/*
		 * Do a binary search of the page -- this loop needs to be
		 * tight.
		 *
		 * We don't have to handle overflow keys here, overflow keys
		 * are always instantiated into internal pages.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If we're about to compare an application key with
			 * the 0th index on an internal page; pretend the 0th
			 * index sorts less than any application key.  This
			 * test is so we don't have to update internal pages
			 * if the application stores a new, "smallest" key in
			 * the tree.
			 */
			if (indx != 0 || isleaf) {
				cmp = db->btree_compare(
				    db, key, (DBT *)(page->indx + indx));
				if (cmp == 0)
					break;
				if (cmp < 0)
					continue;
			}
			base = indx + 1;
			--limit;
		}

		/*
		 * If matched on a leaf page, return the page and the matching
		 * index.
		 *
		 * If matched on an internal page, continue searching down the
		 * tree from indx.
		 */
		if (cmp == 0) {
			if (isleaf) {
				*pagep = page;
				*indxp = page->indx + indx;
				return (0);
			} else
				addr = (page->indx + indx)->addr;
		} else {
			/*
			 * Base is the smallest index greater than key and may
			 * be the 0th index or the (last + 1) indx.  If base
			 * is not the 0th index (remember, the 0th index always
			 * sorts less than any application key), decrement it
			 * to the smallest index less than or equal to key.
			 */
			addr =
			    (page->indx + (base == 0 ? base : base - 1))->addr;
		}

		/* We're done with the page. */
		if ((ret = __wt_db_page_out(db, page, 0)) != 0)
			return (ret);

		/*
		 * Failed to match on a leaf page -- we're done, return the
		 * failure.
		 */
		if (isleaf)
			break;
	}
	return (WT_NOTFOUND);
}
