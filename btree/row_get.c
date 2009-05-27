/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_search(DB *, DBT *, WT_PAGE **, WT_INDX **);
static int __wt_bt_search_recno(DB *, u_int64_t, WT_PAGE **, WT_INDX **);

/*
 * __wt_db_get --
 *	Db.get method.
 */
int
__wt_db_get(WT_TOC *toc)
{
	wt_args_db_get_unpack;
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_INDX *indx;
	u_int32_t type;
	int ret, tret;

	env = toc->env;
	idb = db->idb;

	WT_ASSERT(env, pkey == NULL);			/* NOT YET */

	WT_DB_FCHK(db, "Db.get", flags, WT_APIMASK_DB_GET);

	/* Search the primary btree for the key. */
	if ((ret = __wt_bt_search(db, key, &page, &indx)) != 0)
		return (ret);

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(indx->ditem);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(db, key, data, page, indx, 0);

	/* Discard any page other than the root page, which remains pinned. */
	if (page != idb->root_page &&
	    (tret = __wt_bt_page_out(db, STOC_PRIME, page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);

}

/*
 * __wt_db_get_recno --
 *	Db.get_recno method.
 */
int
__wt_db_get_recno(WT_TOC *toc)
{
	wt_args_db_get_recno_unpack;
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_INDX *indx;
	u_int32_t type;
	int ret, tret;

	env = toc->env;
	idb = db->idb;

	WT_ASSERT(env, pkey == NULL);			/* NOT YET */

	WT_DB_FCHK(db, "Db.get_recno", flags, WT_APIMASK_DB_GET_RECNO);

	/* Search the primary btree for the key. */
	if ((ret = __wt_bt_search_recno(db, recno, &page, &indx)) != 0)
		return (ret);

	/*
	 * The Db.get_recno method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(indx->ditem);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get_recno method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(db, key, data, page, indx, 1);

	/* Discard any page other than the root page, which remains pinned. */
	if (page != idb->root_page &&
	    (tret = __wt_bt_page_out(db, STOC_PRIME, page, 0)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_bt_search --
 *	Search the tree for a specific key.
 */
static int
__wt_bt_search(DB *db, DBT *key, WT_PAGE **pagep, WT_INDX **indxp)
{
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, next_isleaf, put_page, ret;

	idb = db->idb;

	/* Check for an empty tree. */
	if ((addr = idb->root_addr) == WT_ADDR_INVALID)
		return (WT_NOTFOUND);

	/* Search the tree. */
	page = idb->root_page;
	isleaf = page->hdr->type == WT_PAGE_LEAF ? 1 : 0;
	for (put_page = 0;; put_page = 1) {
		/*
		 * Do a binary search of the page -- this loop needs to be
		 * tight.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is an overflow, it may not have been
			 * instantiated yet.
			 */
			ip = page->indx + indx;
			if (ip->data == NULL && (ret =
			    __wt_bt_ovfl_to_indx(db, page, ip)) != 0)
				goto err;

			/*
			 * If we're about to compare an application key with
			 * the 0th index on an internal page, pretend the 0th
			 * index sorts less than any application key.  This
			 * test is so we don't have to update internal pages
			 * if the application stores a new, "smallest" key in
			 * the tree.
			 *
			 * For the record, we still maintain the key at the
			 * 0th location because it means tree verification
			 * and other code that processes a level of the tree
			 * doesn't need to know about this hack.
			 */
			if (indx != 0 || isleaf) {
				cmp = db->btree_compare(db, key, (DBT *)ip);
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
				*indxp = ip;
				return (0);
			}
		} else {
			/*
			 * Base is the smallest index greater than key and may
			 * be the 0th index or the (last + 1) indx.  If base
			 * is not the 0th index (remember, the 0th index always
			 * sorts less than any application key), decrement it
			 * to the smallest index less than or equal to key.
			 */
			ip = page->indx + (base == 0 ? base : base - 1);
		}
		addr = WT_INDX_OFFP_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->ditem) == WT_ITEM_OFFP_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (put_page &&
		    (ret = __wt_bt_page_out(db, STOC_PRIME, page, 0)) != 0)
			return (ret);

		/*
		 * Failed to match on a leaf page -- we're done, return the
		 * failure.
		 */
		if (isleaf)
			return (WT_NOTFOUND);
		isleaf = next_isleaf;

		/* Get the next page. */
		if ((ret = __wt_bt_page_in(
		    db, STOC_PRIME, addr, isleaf, 1, &page)) != 0)
			return (ret);
	}
	/* NOTREACHED */

	/* Discard any page we've read other than the root page. */
err:	if (put_page)
		(void)__wt_bt_page_out(db, STOC_PRIME, page, 0);
	return (ret);
}

/*
 * __wt_bt_search_recno --
 *	Search the tree for a specific record-based key.
 */
static int
__wt_bt_search_recno(DB *db, u_int64_t recno, WT_PAGE **pagep, WT_INDX **indxp)
{
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int64_t total;
	u_int32_t addr, i;
	int isleaf, next_isleaf, put_page, ret;

	idb = db->idb;

	/* Check for an empty tree. */
	if ((addr = idb->root_addr) == WT_ADDR_INVALID)
		return (WT_NOTFOUND);

	/* Search the tree. */
	page = idb->root_page;
	isleaf = page->hdr->type == WT_PAGE_LEAF ? 1 : 0;
	for (total = 0, put_page = 0;; put_page = 1) {
		/* Check for a record past the end of the database. */
		if (total == 0 && page->records < recno)
			break;

		/* If it's a leaf page, return the page and index. */
		if (isleaf) {
			*pagep = page;
			*indxp = page->indx + ((recno - total) - 1);
			return (0);
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (total + WT_INDX_OFFP_RECORDS(ip) >= recno)
				break;
			total += WT_INDX_OFFP_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_INDX_OFFP_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->ditem) == WT_ITEM_OFFP_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (put_page &&
		    (ret = __wt_bt_page_out(db, STOC_PRIME, page, 0)) != 0)
			return (ret);

		isleaf = next_isleaf;

		/* Get the next page. */
		if ((ret = __wt_bt_page_in(
		    db, STOC_PRIME, addr, isleaf, 1, &page)) != 0)
			return (ret);
	}

	/* Discard any page we've read other than the root page. */
	if (put_page)
		(void)__wt_bt_page_out(db, STOC_PRIME, page, 0);
	return (WT_NOTFOUND);
}
