/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_search(WT_TOC *, DBT *, WT_PAGE **, WT_INDX **);
static int __wt_bt_put_serial_func(WT_TOC *);

/*
 * __wt_db_get --
 *	Db.get method.
 */
int
__wt_db_get(DB *db, WT_TOC *toc, DBT *key, DBT *pkey, DBT *data)
{
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int32_t type;
	int ret;

	WT_ASSERT(toc->env, pkey == NULL);		/* NOT YET */

	idb = db->idb;

	WT_STAT_INCR(idb->stats, DB_READ_BY_KEY);

	/*
	 * Initialize the thread-of-control structure.
	 * We're will to re-start if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get");

	/* Search the primary btree for the key. */
	WT_ERR(__wt_bt_search(toc, key, &page, &ip));

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(ip->page_data);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(toc, key, data, page, ip, 0);

	/* Discard the page. */
	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

err:	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_db_put --
 *	Db.put method.
 */
int
__wt_db_put(DB *db, WT_TOC *toc, DBT *key, DBT *data)
{
	ENV *env;
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	WT_REPL *repl;
	u_int32_t repl_cnt;
	void *p;
	int ret;

	env = db->env;
	idb = db->idb;
	repl = NULL;
	repl_cnt = 0;
	p = NULL;
	ret = 0;

	WT_STAT_INCR(idb->stats, DB_WRITE_BY_KEY);

	WT_TOC_DB_INIT(toc, db, "Db.put");

	/*
	 * Search the primary btree for the key, replace the item on the
	 * page, and discard the page.
	 */
	WT_ERR(__wt_bt_search(toc, key, &page, &ip));

	/*
	 * If the page doesn't yet have a replacement array, or it's not big
	 * enough to hold a new entry, create one.  We might have to free it
	 * in the workQ thread, if two threads were to both allocate an array
	 * array, but the alternative is to call calloc in the workQ thread,
	 * and I'd like to avoid that.
	 */
	if (ip->repl != NULL)
		for (; repl_cnt < ip->repl_size; ++repl_cnt)
			if (ip->repl[repl_cnt].data == NULL)
				break;
	if (repl_cnt == ip->repl_size) {
		repl_cnt += 10;
		WT_ERR(__wt_calloc(env, repl_cnt, sizeof(WT_REPL), &repl));
	} else
		repl = NULL;

	/* Get a local copy of the data item. */
	WT_ERR(__wt_calloc(env, 1, data->size, &p));
	memcpy(p, data->data, data->size);

	/* Update the item. */
	__wt_bt_put_serial(toc, ip, repl, repl_cnt, p, data->size);
	if ((ret = toc->serial_ret) != 0)
		goto err;

	if (0) {
err:		if (p != NULL)
			__wt_free(env, p, data->size);
		if (repl != NULL)
			__wt_free(env, repl, repl_cnt * sizeof(WT_REPL));
	}

	if (page != idb->root_page)
		WT_TRET(__wt_bt_page_out(toc, page, 0));

	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * __wt_bt_put_serial_func --
 *	Server function to write bytes onto a page.
 */
static int
__wt_bt_put_serial_func(WT_TOC *toc)
{
	ENV *env;
	WT_INDX *ip;
	WT_REPL *repl;
	void *data;
	u_int32_t repl_cnt, repl_size, size;

	__wt_bt_put_unpack(toc, ip, repl, repl_size, data, size);
	env = toc->env;

	/* If passed a new replacement array, use it or lose it. */
	if (repl != NULL)
		if (ip->repl == NULL) {
			ip->repl = repl;
			ip->repl_size = repl_size;
		} else
			__wt_free(env, repl, repl_size * sizeof(WT_REPL));

	/* Update the replacement array. */
	for (repl_cnt = 0,
	    repl = ip->repl; repl_cnt < ip->repl_size; ++repl_cnt, ++repl)
		if (repl->data == NULL) {
			/*
			 * The data field makes this change visible to the
			 * rest of the system; flush memory before setting
			 * it.
			 */
			repl->size = size;
			WT_MEMORY_FLUSH;
			repl->data = data;
			return (0);
		}

	/*
	 * It's possible the replacement array was filled while the thread
	 * waited for serialization.   That's OK, just try again.
	 */
	return (WT_RESTART);
}

/*
 * __wt_bt_search --
 *	Search the tree for a specific key.
 */
static int
__wt_bt_search(WT_TOC *toc, DBT *key, WT_PAGE **pagep, WT_INDX **ipp)
{
	DB *db;
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	u_int32_t addr, base, indx, limit;
	int cmp, isleaf, next_isleaf, ret;

	db = toc->db;
	idb = db->idb;

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_ROW_LEAF ? 1 : 0;

	/* Search the tree. */
	for (;;) {
		/*
		 * Do a binary search of the page -- this loop must be tight.
		 */
		for (base = 0,
		    limit = page->indx_count; limit != 0; limit >>= 1) {
			indx = base + (limit >> 1);

			/*
			 * If the key is compressed or an overflow, it may not
			 * have been instantiated yet.
			 */
			ip = page->indx + indx;
			if (WT_INDX_NEED_PROCESS(ip))
				WT_ERR(__wt_bt_key_to_indx(toc, page, ip));

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
				*ipp = ip;
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
		addr = WT_ROW_OFF_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->page_data) == WT_ITEM_OFF_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		/*
		 * Failed to match on a leaf page -- we're done, return the
		 * failure.
		 */
		if (isleaf)
			return (WT_NOTFOUND);
		isleaf = next_isleaf;

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}
	/* NOTREACHED */

	/* Discard any page we've read other than the root page. */
err:	if (page != idb->root_page)
		(void)__wt_bt_page_out(toc, page, 0);
	return (ret);
}
