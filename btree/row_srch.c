/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_page_put(WT_TOC *, DBT *, WT_PAGE *, WT_INDX *);
static int __wt_bt_search(WT_TOC *, DBT *, WT_PAGE **, WT_INDX **);
static int __wt_put_serial_func(WT_TOC *);

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

	WT_STAT_INCR(idb->stats,
	    DB_READ_BY_KEY, "database read-by-key operations");

	/*
	 * Initialize the thread-of-control structure.
	 * We're will to re-start if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.get");

	/* Search the primary btree for the key. */
	F_SET(toc, WT_CACHE_LOCK_RESTART);
	while ((ret = __wt_bt_search(toc, key, &page, &ip)) == WT_RESTART) {
		WT_STAT_INCR(idb->stats, DB_READ_BY_KEY_RESTART,
		    "database read-by-key operation restarted");
		__wt_toc_serialize_wait(toc, NULL);
	}
	F_CLR(toc, WT_CACHE_LOCK_RESTART);
	if (ret != 0)
		goto err;

	/*
	 * The Db.get method can only return single key/data pairs.
	 * If that's not what we found, we're done.
	 */
	type = WT_ITEM_TYPE(ip->ditem);
	if (type != WT_ITEM_DATA && type != WT_ITEM_DATA_OVFL) {
		__wt_db_errx(db,
		    "the Db.get method cannot return keys with duplicate "
		    "data items; use the Db.cursor method instead");
		ret = WT_ERROR;
	} else
		ret = __wt_bt_dbt_return(toc, key, data, page, ip, 0);

	/* Discard the page. */
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
	IDB *idb;
	WT_INDX *ip;
	WT_PAGE *page;
	int ret;

	idb = db->idb;

	WT_STAT_INCR(idb->stats,
	    DB_WRITE_BY_KEY, "database put-by-key operations");

	/*
	 * Initialize the thread-of-control structure.
	 * We're willing to restart if the cache is too full.
	 */
	WT_TOC_DB_INIT(toc, db, "Db.put");

	/*
	 * Search the primary btree for the key, and replace the item on the
	 * page.
	 */
	for (;;) {
		F_SET(toc, WT_CACHE_LOCK_RESTART);
		ret = __wt_bt_search(toc, key, &page, &ip);
		F_CLR(toc, WT_CACHE_LOCK_RESTART);
		if (ret != 0 && ret != WT_RESTART)
			break;

		if (ret == 0) {
			ret = __wt_bt_page_put(toc, data, page, ip);
			if (ret != WT_RESTART)
				break;
		}

		WT_STAT_INCR(idb->stats, DB_WRITE_BY_KEY_RESTART,
		    "database write-by-key operation restarted");
		__wt_toc_serialize_wait(toc, NULL);
	}

	/* Discard the returned page. */
	WT_TRET(__wt_bt_page_out(toc, page, ret == 0 ? WT_MODIFIED : 0));

	WT_TOC_DB_CLEAR(toc);

	return (ret);
}

/*
 * Page modification serialization support.
 */
typedef struct {
	WT_PAGE  *page;				/* Leaf page to modify */
	WT_PAGE  *page_ovfl;			/* Overflow page to modify */
	DBT *from;				/* Source data */
	void *to;				/* Target data */
} __wt_put_args;
#define	__wt_put_serial(toc, _page, _page_ovfl, _from, _to) do {	\
	__wt_put_args _args;						\
	_args.page = _page;						\
	_args.page_ovfl = _page_ovfl;					\
	_args.from = _from;						\
	_args.to = _to;							\
	__wt_toc_serialize_request(toc,					\
	    __wt_put_serial_func, &_args, &(_page)->serial_private);	\
} while (0);
#define	__wt_put_unpack(toc, _page, _page_ovfl, _from, _to) do {	\
	_page =	((__wt_put_args *)(toc)->serial_args)->page;		\
	_page_ovfl =							\
	    ((__wt_put_args *)(toc)->serial_args)->page_ovfl;		\
	_from = ((__wt_put_args *)(toc)->serial_args)->from;		\
	_to = ((__wt_put_args *)(toc)->serial_args)->to;		\
} while (0)

/*
 * __wt_bt_page_put --
 *	Insert data onto a page.
 */
static int
__wt_bt_page_put(WT_TOC *toc, DBT *data, WT_PAGE *page, WT_INDX *ip)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	WT_PAGE *page_ovfl;
	u_int32_t psize;
	void *pdata;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	/* Optional Huffman compression. */
	if (idb->huffman_data != NULL) {
		WT_RET(__wt_huffman_encode(idb->huffman_data,
		    data->data, data->size, &toc->scratch.data,
		    &toc->scratch.data_len, &toc->scratch.size));
		data = &toc->scratch;
	}

	item = ip->ditem;
	page_ovfl = NULL;
	switch (page->hdr->type) {
	case WT_PAGE_LEAF:
		if (WT_ITEM_TYPE(item) == WT_ITEM_DATA) {
			pdata = WT_ITEM_BYTE(item);
			psize = WT_ITEM_LEN(item);
			break;
		}
		goto overflow;
	case WT_PAGE_DUP_LEAF:
		if (WT_ITEM_TYPE(item) == WT_ITEM_DUP) {
			pdata = ip->data;
			psize = ip->size;
			break;
		}
overflow:	ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(ip->ditem);
		WT_RET(__wt_bt_ovfl_in(toc, ovfl->addr, ovfl->len, &page_ovfl));
		pdata = WT_PAGE_BYTE(page_ovfl);
		psize = ovfl->len;
		break;
	WT_DEFAULT_FORMAT(db);
	}

	/*
	 * Update the on-page data.
	 * For now we can only handle overwriting items of the same size.
	 */
	WT_ASSERT(toc->env, data->size == psize);

	__wt_put_serial(toc, page, page_ovfl, data, pdata);

	return (toc->serial_ret);
}

/*
 * __wt_put_serial_func --
 *	Server function to write bytes onto a page.
 */
static int
__wt_put_serial_func(WT_TOC *toc)
{
	WT_PAGE *page, *page_ovfl;
	DBT *from;
	void *to;

	__wt_put_unpack(toc, page, page_ovfl, from, to);

	memcpy(to, from->data, from->size);

	if (page_ovfl != NULL)
		WT_RET(__wt_bt_page_out(toc, page_ovfl, WT_MODIFIED));
	WT_RET(
	    __wt_bt_page_out(toc, page, page_ovfl == NULL ? WT_MODIFIED : 0));

	return (0);
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
	int cmp, isleaf, next_isleaf, put_page, ret;

	db = toc->db;
	idb = db->idb;

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_LEAF ? 1 : 0;

	/* Search the tree. */
	for (put_page = 0;; put_page = 1) {
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
			if (WT_INDX_NEED_PROCESS(idb, ip))
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
		addr = WT_INDX_OFFP_ADDR(ip);
		next_isleaf =
		    WT_ITEM_TYPE(ip->ditem) == WT_ITEM_OFFP_LEAF ? 1 : 0;

		/* We're done with the page. */
		if (put_page)
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
err:	if (put_page)
		(void)__wt_bt_page_out(toc, page, 0);
	return (ret);
}
