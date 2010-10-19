/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * WT_STACK --
 *	We maintain a stack of parent pages as we build the tree, encapsulated
 *	in this structure.
 */
typedef struct {
	WT_PAGE **page;				/* page stack */
	u_int size;				/* stack size */
} WT_STACK;

/*
 * Bulk-load builds physical pages, and we want to verify them as soon as they
 * are created (when running in diagnostic mode).
 */
#ifdef HAVE_DIAGNOSTIC
#define	WT_BULK_PAGE_OUT(toc, pagep, flags) do {			\
	WT_ASSERT(							\
	    (toc)->env, __wt_bt_verify_page(toc, *(pagep), NULL) == 0);	\
	__wt_bt_page_out(toc, pagep, flags);				\
} while (0)
#else
#define	WT_BULK_PAGE_OUT(toc, pagep, flags)				\
	__wt_bt_page_out(toc, pagep, flags)
#endif

static int  __wt_bt_bulk_fix(WT_TOC *,
	void (*)(const char *, u_int64_t), int (*)(DB *, DBT **, DBT **));
static int __wt_bt_bulk_ovfl_write(WT_TOC *, DBT *, u_int32_t *);
static int  __wt_bt_bulk_var(WT_TOC *, u_int32_t,
	void (*)(const char *, u_int64_t), int (*)(DB *, DBT **, DBT **));
static inline int __wt_bt_bulk_write(WT_TOC *, WT_PAGE *);
static int  __wt_bt_dbt_copy(ENV *, DBT *, DBT *);
static int  __wt_bt_dup_offpage(WT_TOC *, WT_PAGE *, DBT **, DBT **,
	DBT *, WT_ITEM *, u_int32_t, int (*cb)(DB *, DBT **, DBT **));
static int  __wt_bt_promote(
	WT_TOC *, WT_PAGE *, u_int64_t, WT_STACK *, u_int, u_int32_t *);
static int  __wt_bt_promote_col_indx(WT_TOC *, WT_PAGE *, void *);
static int  __wt_bt_promote_row_indx(WT_TOC *, WT_PAGE *, WT_ITEM *, WT_ITEM *);
static inline void __wt_bt_promote_col_rec(WT_PAGE *, u_int64_t);
static inline void __wt_bt_promote_row_rec(WT_PAGE *, u_int64_t);

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(WT_TOC *toc, u_int32_t flags,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	IDB *idb;

	db = toc->db;
	idb = db->idb;

	if (F_ISSET(idb, WT_COLUMN))
		WT_DB_FCHK(db, "DB.bulk_load", flags, 0);

	/*
	 * There are two styles of bulk-load: variable length pages or
	 * fixed-length pages.
	 */
	if (F_ISSET(idb, WT_COLUMN) && db->fixed_len != 0)
		WT_RET(__wt_bt_bulk_fix(toc, f, cb));
	else
		WT_RET(__wt_bt_bulk_var(toc, flags, f, cb));

	/* Get a permanent root page reference. */
	return (__wt_bt_root_pin(toc, 1));
}

/*
 * __wt_bt_bulk_fix
 *	Db.bulk_load method for column-store, fixed-length database pages.
 */
static int
__wt_bt_bulk_fix(WT_TOC *toc,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, *tmp;
	ENV *env;
	IDB *idb;
	WT_PAGE _page, *page;
	WT_PAGE_HDR *hdr;
	WT_STACK stack;
	u_int64_t insert_cnt;
	u_int32_t len;
	u_int16_t *last_repeat;
	u_int8_t *last_data;
	int ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	insert_cnt = 0;
	WT_CLEAR(stack);

	/* Figure out how large is the chunk we're storing on the page. */
	len = db->fixed_len +
	    (F_ISSET(idb, WT_REPEAT_COMP) ? sizeof(u_int16_t) : 0);

	/*
	 * We don't run the leaf pages through the cache -- that means passing
	 * a lot of messages we don't want to bother with.  We're the only user
	 * of the database, which means we can grab file space whenever we want.
	 *
	 * Make sure the TOC's scratch buffer is big enough to hold a leaf page,
	 * and clear the memory so it's never random bytes.
	 */
	tmp = &toc->tmp1;
	if (tmp->mem_size < db->leafmin)
		WT_RET(__wt_realloc(
		    env, &tmp->mem_size, db->leafmin, &tmp->data));
	memset(tmp->data, 0, db->leafmin);

	/* Initialize the WT_PAGE and WT_PAGE_HDR information. */
	WT_CLEAR(_page);
	page = &_page;
	page->hdr = hdr = tmp->data;
	WT_ERR(__wt_cache_alloc(toc, &page->addr, db->leafmin));
	page->size = db->leafmin;
	__wt_bt_set_ff_and_sa_from_offset(page, WT_PAGE_BYTE(page));
	hdr->type = WT_PAGE_COL_FIX;
	hdr->level = WT_LLEAF;

	/* Update the descriptor record. */
	idb->root_addr = page->addr;
	idb->root_size = page->size;
	WT_ERR(__wt_bt_desc_write(toc));

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key != NULL) {
			__wt_api_db_errx(db,
			    "column database keys are implied and "
			    "so should not be returned by the bulk "
			    "load input routine");
			ret = WT_ERROR;
			goto err;
		}
		if (data->size != db->fixed_len)
			WT_ERR(__wt_database_wrong_fixed_size(toc, data->size));

		/*
		 * We use the high bit of the data field as a "deleted" value,
		 * make sure the user's data doesn't set it.
		 */
		if (WT_FIX_DELETE_ISSET(data->data)) {
			__wt_api_db_errx(db,
			    "the first bit may not be stored in fixed-length "
			    "column-store database items");
			ret = WT_ERROR;
			goto err;
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(toc->name, insert_cnt);
		WT_STAT_INCR(idb->stats, ITEMS_INSERTED);

		/*
		 * Bulk load is a long-running operation, update the generation
		 * number so we don't tie memory down.
		 */
		WT_TOC_GEN_SET(toc);

		/*
		 * If doing repeat compression, check to see if this record
		 * matches the last data inserted.   If there's a match try
		 * and increment that item's repeat count instead of entering
		 * new data.
		 */
		if (F_ISSET(idb, WT_REPEAT_COMP) && hdr->u.entries != 0)
			if (*last_repeat < UINT16_MAX &&
			    memcmp(last_data, data->data, data->size) == 0) {
				++*last_repeat;
				++hdr->u.entries;
				WT_STAT_INCR(idb->stats, REPEAT_COUNT);
				continue;
			}

		/*
		 * We now have the data item to store on the page.  If there
		 * is insufficient space on the current page, allocate a new
		 * one.
		 */
		if (len > page->space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			page->records = hdr->u.entries;
			WT_ERR(__wt_bt_promote(
			    toc, page, page->records, &stack, 0, NULL));
			WT_ERR(__wt_bt_bulk_write(toc, page));
			hdr->u.entries = 0;
			WT_ERR(__wt_cache_alloc(toc, &page->addr, db->leafmin));
			__wt_bt_set_ff_and_sa_from_offset(
			    page, WT_PAGE_BYTE(page));
		}

		++hdr->u.entries;

		/*
		 * Copy the data item onto the page -- if we're doing repeat
		 * compression, track the location of the item for comparison.
		 */
		if (F_ISSET(idb, WT_REPEAT_COMP)) {
			last_repeat = (u_int16_t *)page->first_free;
			*last_repeat = 1;
			page->first_free += sizeof(u_int16_t);
			page->space_avail -= sizeof(u_int16_t);
			last_data = page->first_free;
		}
		memcpy(page->first_free, data->data, data->size);
		page->first_free += data->size;
		page->space_avail -= data->size;
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (hdr->u.entries != 0) {
		page->records = hdr->u.entries;
		ret = __wt_bt_promote(
		    toc, page, page->records, &stack, 0, NULL);
		WT_ERR(__wt_bt_bulk_write(toc, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	if (stack.page != NULL) {
		u_int i;
		for (i = 0; stack.page[i] != NULL; ++i)
			WT_BULK_PAGE_OUT(toc, &stack.page[i], WT_MODIFIED);
		__wt_free(env, stack.page, stack.size * sizeof(WT_PAGE *));
	}

	return (ret);
}

/*
 * __wt_bt_bulk_var --
 *	Db.bulk_load method for row or column-store variable-length database
 *	pages.
 */
static int
__wt_bt_bulk_var(WT_TOC *toc, u_int32_t flags,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, key_copy, data_copy;
	DBT *lastkey, lastkey_std, lastkey_ovfl;
	ENV *env;
	IDB *idb;
	WT_ITEM key_item, data_item, *dup_key, *dup_data;
	WT_OVFL key_ovfl, data_ovfl;
	WT_PAGE *page, *next;
	WT_STACK stack;
	u_int64_t insert_cnt;
	u_int32_t dup_count, dup_space, len;
	u_int type;
	int ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ret = 0;

	WT_CLEAR(stack);
	dup_space = dup_count = 0;
	insert_cnt = 0;
	type = F_ISSET(idb, WT_COLUMN) ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF;

	lastkey = &lastkey_std;
	WT_CLEAR(data_copy);
	WT_CLEAR(key_copy);
	WT_CLEAR(key_item);
	WT_CLEAR(lastkey_ovfl);
	WT_CLEAR(lastkey_std);

	/*
	 * Allocate our first page and set our handle to reference it, then
	 * update the database descriptor record.
	 */
	WT_ERR(__wt_bt_page_alloc(toc, type, WT_LLEAF, db->leafmin, &page));
	idb->root_addr = page->addr;
	idb->root_size = db->leafmin;
	WT_ERR(__wt_bt_desc_write(toc));

	while ((ret = cb(db, &key, &data)) == 0) {
		if (F_ISSET(idb, WT_COLUMN) ) {
			if (key != NULL) {
				__wt_api_db_errx(db,
				    "column database keys are implied and "
				    "so should not be returned by the bulk "
				    "load input routine");
				ret = WT_ERROR;
				goto err;
			}
		} else {
			if (key == NULL && !LF_ISSET(WT_DUPLICATES)) {
				__wt_api_db_errx(db,
				    "keys must be specified unless duplicates "
				    "are configured");
				ret = WT_ERROR;
				goto err;
			}
			if (key != NULL && key->size == 0) {
				__wt_api_db_errx(db,
				    "zero-length keys are not supported");
				ret = WT_ERROR;
				goto err;
			}
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f(toc->name, insert_cnt);
		WT_STAT_INCR(idb->stats, ITEMS_INSERTED);

		/*
		 * Bulk load is a long-running operation, update the generation
		 * number so we don't tie memory down.
		 */
		WT_TOC_GEN_SET(toc);

		/*
		 * We don't have a key to store on the page if we're building a
		 * column-store, and we don't store the key on the page in the
		 * case of a row-store duplicate data item.  The check from here
		 * on is if "key == NULL" for both cases, that is, there's no
		 * key to store.
		 */

		/*
		 * Copy the caller's DBTs, we don't want to modify them.  But,
		 * copy them carefully, all we want is a pointer and a length.
		 */
		if (key != NULL) {
			key_copy.data = key->data;
			key_copy.size = key->size;
			key = &key_copy;
		}
		data_copy.data = data->data;
		data_copy.size = data->size;
		data = &data_copy;

skip_read:	/*
		 * We pushed a set of duplicates off-page, and that routine
		 * returned an ending key/data pair to us.
		 */

		/*
		 * Process the key/data pairs and build the items we're going
		 * to store on the page.
		 */
		if (key != NULL)
			WT_ERR(__wt_bt_build_key_item(
			    toc, key, &key_item, &key_ovfl, 1));
		WT_ERR(__wt_bt_build_data_item(
		    toc, data, &data_item, &data_ovfl, 1));

		/*
		 * Check for duplicate data; we don't store the key on the page
		 * in the case of a duplicate.
		 *
		 * !!!
		 * Do a fast check of the old and new sizes -- note checking
		 * lastkey->size is safe -- it's initialized to 0, and we do
		 * not allow zero-length keys.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    (key == NULL ||
		    (lastkey->size == key->size &&
		    db->btree_compare(db, lastkey, key) == 0))) {
			/*
			 * The first duplicate in the set is already on the
			 * page, but with an item type set to WT_ITEM_DATA or
			 * WT_ITEM_DATA_OVFL.  Correct the type and dup_count.
			 */
			if (++dup_count == 1) {
				dup_count = 2;
				WT_ITEM_TYPE_SET(dup_data,
				    WT_ITEM_TYPE(dup_data) == WT_ITEM_DATA ?
				    WT_ITEM_DUP : WT_ITEM_DUP_OVFL);
			}

			/* Reset the type of the current item to a duplicate. */
			WT_ITEM_TYPE_SET(&data_item,
			    WT_ITEM_TYPE(&data_item) == WT_ITEM_DATA ?
			    WT_ITEM_DUP : WT_ITEM_DUP_OVFL);

			WT_STAT_INCR(idb->stats, DUPLICATE_ITEMS_INSERTED);

			key = NULL;
		} else
			dup_count = 0;

		/*
		 * If duplicates: we'll need a copy of the key for comparison
		 * with the next key.  If the key is an overflow object, we
		 * can't just use the on-page version, we have to save a copy.
		 */
		if (key != NULL &&
		    LF_ISSET(WT_DUPLICATES) &&
		    WT_ITEM_TYPE(&key_item) == WT_ITEM_KEY_OVFL) {
			lastkey = &lastkey_ovfl;
			WT_ERR(__wt_bt_dbt_copy(env, key, lastkey));
		}

		/*
		 * We now have the key/data items to store on the page.  If
		 * there is insufficient space on the current page, allocate
		 * a new one.
		 */
		if ((key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			WT_ERR(__wt_bt_page_alloc(
			    toc, type, WT_LLEAF, db->leafmin, &next));

			/*
			 * If in the middle of loading a set of duplicates, but
			 * the set hasn't yet reached the boundary where we'd
			 * push them offpage, we can't split them across the two
			 * pages.  Move the entire set to the new page.  This
			 * can waste up to 25% of the old page, but it would be
			 * difficult and messy to move them and then go back
			 * and fix things up if and when they moved offpage.
			 *
			 * We use a check of dup_count instead of checking the
			 * WT_DUPLICATES flag, since we have to check it anyway.
			 */
			if (dup_count != 0) {
				/*
				 * Reset the page entry and record counts -- we
				 * are moving a single key plus the duplicate
				 * set.
				 *
				 * Since dup_count was already incremented to
				 * reflect the data item we're loading now, it
				 * is the right number of elements to move, that
				 * is, move (dup_count - 1) + 1 for the key.
				 */
				page->hdr->u.entries -= dup_count;
				page->records -= dup_count - 1;
				next->hdr->u.entries += dup_count;
				next->records += dup_count - 1;

				/*
				 * Move the duplicate set and adjust the page
				 * information for "next" -- we don't have to
				 * fix up "page", we're never going to use it
				 * again.
				 */
				len = (u_int32_t)
				    (page->first_free - (u_int8_t *)dup_key);
				memcpy(next->first_free, dup_key, len);
				next->first_free += len;
				next->space_avail -= len;

				/*
				 * We'll never have to move this dup set to
				 * another primary page -- if the dup set
				 * continues to grow, it will be moved
				 * off-page.  We still need to know where
				 * the dup set starts, though, for the
				 * possible move off-page: it's the second
				 * entry on the page, where the first entry
				 * is the dup set's key.
				 */
				dup_key = (WT_ITEM *)WT_PAGE_BYTE(next);
				dup_data =
				    (WT_ITEM *)((u_int8_t *)dup_key +
				    WT_ITEM_SPACE_REQ(WT_ITEM_LEN(dup_key)));

				/*
				 * The "lastkey" value just moved to a new page.
				 * If it's an overflow item, we have a copy; if
				 * it's not, then we need to reset it.
				 */
				if (lastkey == &lastkey_std) {
					lastkey_std.data =
					    WT_ITEM_BYTE(dup_key);
					lastkey_std.size = WT_ITEM_LEN(dup_key);
				}
			}

			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bt_promote(
			    toc, page, page->records, &stack, 0, NULL));
			WT_BULK_PAGE_OUT(toc, &page, WT_DISCARD | WT_MODIFIED);
			page = next;
		}

		++page->records;

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->hdr->u.entries;

			memcpy(page->first_free, &key_item, sizeof(key_item));
			memcpy(page->first_free +
			    sizeof(key_item), key->data, key->size);
			page->space_avail -= WT_ITEM_SPACE_REQ(key->size);

			/*
			 * If duplicates: we'll need a copy of the key for
			 * comparison with the next key.  Not an overflow
			 * object, so we can just use the on-page memory.
			 *
			 * We also save the location for the key of any current
			 * duplicate set in case we have to move the set to a
			 * different page (the case where a duplicate set isn't
			 * large enough to move offpage, but doesn't entirely
			 * fit on this page).
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				if (WT_ITEM_TYPE(
				    &key_item) != WT_ITEM_KEY_OVFL) {
					lastkey = &lastkey_std;
					lastkey_std.data =
					    WT_ITEM_BYTE(page->first_free);
					lastkey_std.size = key->size;
				}
				dup_key = (WT_ITEM *)page->first_free;
			}
			page->first_free += WT_ITEM_SPACE_REQ(key->size);
		}

		/* Copy the data item onto the page. */
		++page->hdr->u.entries;
		memcpy(page->first_free, &data_item, sizeof(data_item));
		memcpy(page->first_free +
		    sizeof(data_item), data->data, data->size);
		page->space_avail -= WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: if this isn't a duplicate data item, save
		 * the item location, since it's potentially the first of a
		 * duplicate data set, and we need to know where duplicate
		 * data sets start.  Additionally, reset the counter and
		 * space calculation.
		 */
		if (LF_ISSET(WT_DUPLICATES) && dup_count == 0) {
			dup_space = data->size;
			dup_data = (WT_ITEM *)page->first_free;
		}
		page->first_free += WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: check to see if the duplicate set crosses
		 * the (roughly) 25% of the page space boundary.  If it does,
		 * move it offpage.
		 */
		if (LF_ISSET(WT_DUPLICATES) && dup_count != 0) {
			dup_space += data->size;

			if (dup_space < db->leafmin / db->btree_dup_offpage)
				continue;

			/*
			 * Move the duplicate set offpage and read in the
			 * rest of the duplicate set.
			 */
			WT_ERR(__wt_bt_dup_offpage(toc, page, &key,
			    &data, lastkey, dup_data, dup_count, cb));

			/*
			 * Reset local counters -- on-page information was
			 * reset by __wt_bt_dup_offpage.
			 */
			dup_count = dup_space = 0;

			goto skip_read;
		}
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret == 1) {
		ret = 0;

		/* Promote a key from any partially-filled page and write it. */
		if (page != NULL) {
			ret = __wt_bt_promote(
			    toc, page, page->records, &stack, 0, NULL);
			WT_BULK_PAGE_OUT(toc, &page, WT_DISCARD | WT_MODIFIED);
		}
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	if (page != NULL)
		WT_BULK_PAGE_OUT(toc, &page, WT_DISCARD | WT_MODIFIED);
	if (stack.page != NULL) {
		u_int i;
		for (i = 0; stack.page[i] != NULL; ++i)
			WT_BULK_PAGE_OUT(toc, &stack.page[i], WT_MODIFIED);
		__wt_free(env, stack.page, stack.size * sizeof(WT_PAGE *));
	}

	__wt_free(env, lastkey_ovfl.data, lastkey_ovfl.mem_size);

	return (ret);
}

/*
 * __wt_bt_dup_offpage --
 *	Move the last set of duplicates on the page to a page of their own,
 *	then load the rest of the duplicate set.
 */
static int
__wt_bt_dup_offpage(WT_TOC *toc, WT_PAGE *leaf_page,
    DBT **keyp, DBT **datap, DBT *lastkey, WT_ITEM *dup_data,
    u_int32_t dup_count, int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data;
	ENV *env;
	IDB *idb;
	WT_ITEM data_item;
	WT_OFF off;
	WT_OVFL data_local;
	WT_PAGE *page;
	WT_STACK stack;
	u_int32_t len, root_addr;
	u_int8_t *p;
	int ret, tret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	/*
	 * This routine is the same as the bulk load routine, except it loads
	 * only data items into off-page duplicate trees.  It's passed a lot
	 * of state from the bulk load routine, and updates that state as a
	 * side-effect.
	 *
	 * In summary, the bulk load routine stops loading a primary btree leaf
	 * page, calls us to load a set of duplicate data items into a separate
	 * btree, and then continues on with its primary leaf page when we
	 * return.  The arguments are complex enough that it's worth describing
	 * them:
	 *
	 * leaf_page --
	 *	The caller's PAGE, which we have to fix up.
	 * keyp/datap --
	 *	The key and data pairs the application is filling in -- we
	 *	get them passed to us because we get additional key/data
	 *	pairs returned to us, and the last one we get is likely to
	 *	be consumed by our caller.
	 * lastkey --
	 *	The last key pushed onto the caller's page -- we use this to
	 *	compare against future keys we read.
	 * dup_data --
	 *	On-page reference to the first duplicate data item in the set.
	 * dup_count --
	 *	Count of duplicates in the set.
	 * cb --
	 *	User's callback function.
	 */

	WT_CLEAR(data_item);
	WT_CLEAR(stack);
	ret = 0;

	/*
	 * Allocate and initialize a new page, and copy the duplicate set into
	 * place.
	 */
	WT_RET(__wt_bt_page_alloc(
	    toc, WT_PAGE_DUP_LEAF, WT_LLEAF, db->leafmin, &page));
	page->hdr->u.entries = dup_count;
	page->records = dup_count;
	len = (u_int32_t)(leaf_page->first_free - (u_int8_t *)dup_data);
	memcpy(page->first_free, dup_data, (size_t)len);
	__wt_bt_set_ff_and_sa_from_offset(page, WT_PAGE_BYTE(page) + len);

	/*
	 * Unless we have enough duplicates to split this page, it will be the
	 * "root" of the offpage duplicates.
	 */
	root_addr = page->addr;

	/*
	 * Reset the caller's page entry count.   Once we know the final root
	 * page and record count we'll replace the duplicate set with a single
	 * WT_OFF structure, that is, we've replaced dup_count entries
	 * with a single entry.
	 */
	leaf_page->hdr->u.entries -= (dup_count - 1);

	/* Read in new duplicate records until the key changes. */
	while ((ret = cb(db, &key, &data)) == 0) {
		if (key->size == 0) {
			__wt_api_db_errx(
			    db, "zero-length keys are not supported");
			return (WT_ERROR);
		}
		WT_STAT_INCR(idb->stats, ITEMS_INSERTED);
		WT_STAT_INCR(idb->stats, DUPLICATE_ITEMS_INSERTED);

		/* Loading duplicates, so a key change means we're done. */
		if (lastkey->size != key->size ||
		    db->btree_compare_dup(db, lastkey, key) != 0) {
			*keyp = key;
			*datap = data;
			break;
		}

		/* Create overflow objects if the data won't fit. */
		if (data->size > db->leafitemsize) {
			data_local.size = data->size;
			WT_RET(__wt_bt_ovfl_write(toc, data, &data_local.addr));
			data->data = &data_local;
			data->size = sizeof(data_local);
			WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DUP_OVFL);
			WT_STAT_INCR(idb->stats, OVERFLOW_DATA);
		} else
			WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DUP);

		/*
		 * If there's insufficient space available, allocate a new
		 * page.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 *
			 * If we promoted a key, we might have split, and so
			 * there may be a new offpage duplicates root page.
			 */
			WT_RET(__wt_bt_promote(toc,
			    page, page->records, &stack, 0, &root_addr));
			WT_BULK_PAGE_OUT(toc, &page, WT_DISCARD | WT_MODIFIED);
			WT_RET(__wt_bt_page_alloc(toc,
			    WT_PAGE_DUP_LEAF, WT_LLEAF, db->leafmin, &page));
		}

		++dup_count;			/* Total duplicate count */
		++page->records;		/* On-page key/data count */
		++page->hdr->u.entries;		/* On-page entry count */
		++leaf_page->records;		/* Parent page key/data count */

		/* Copy the data item onto the page. */
		WT_ITEM_LEN_SET(&data_item, data->size);
		memcpy(page->first_free, &data_item, sizeof(data_item));
		memcpy(page->first_free +
		    sizeof(data_item), data->data, data->size);
		page->space_avail -= WT_ITEM_SPACE_REQ(data->size);
		page->first_free += WT_ITEM_SPACE_REQ(data->size);
	}

	/*
	 * Ret values of 1 and 0 are both "OK", the ret value of 1 means we
	 * reached the end of the bulk input.
	 *
	 * Promote a key from any partially-filled page and write it.
	 */
	if ((tret = __wt_bt_promote(toc, page, page->records,
	    &stack, 0, &root_addr)) != 0 && (ret == 0 || ret == 1))
		ret = tret;
	WT_BULK_PAGE_OUT(toc, &page, WT_DISCARD | WT_MODIFIED);

	/*
	 * Replace the caller's duplicate set with a WT_OFF structure, and
	 * reset the caller's page information.
	 */
	WT_ITEM_LEN_SET(&data_item, sizeof(WT_OFF));
	WT_ITEM_TYPE_SET(&data_item, WT_ITEM_OFF);
	WT_RECORDS(&off) = dup_count;
	off.addr = root_addr;
	off.size = db->intlmin;

	p = (u_int8_t *)dup_data;
	memcpy(p, &data_item, sizeof(data_item));
	memcpy(p + sizeof(data_item), &off, sizeof(WT_OFF));
	__wt_bt_set_ff_and_sa_from_offset(leaf_page,
	    (u_int8_t *)dup_data + WT_ITEM_SPACE_REQ(sizeof(WT_OFF)));

	if (stack.page != NULL) {
		u_int i;
		for (i = 0; stack.page[i] != NULL; ++i)
			WT_BULK_PAGE_OUT(toc, &stack.page[i], WT_MODIFIED);
		__wt_free(env, stack.page, stack.size * sizeof(WT_PAGE *));
	}

	return (ret);
}

/*
 * __wt_bt_promote --
 *	Promote the first entry on a page to its parent.
 */
static int
__wt_bt_promote(WT_TOC *toc, WT_PAGE *page, u_int64_t incr,
    WT_STACK *stack, u_int level, u_int32_t *dup_root_addrp)
{
	DB *db;
	DBT *key, key_build;
	ENV *env;
	WT_ITEM *key_item, item, *parent_key;
	WT_OFF off;
	WT_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	u_int type;
	int need_promotion, ret;
	void *parent_data;

	db = toc->db;
	env = toc->env;

	WT_CLEAR(item);
	next = parent = NULL;

	/*
	 * If it's a row-store, get a copy of the first item on the page -- it
	 * might be an overflow item, in which case we need to make a copy for
	 * the database.  Most versions of Berkeley DB tried to reference count
	 * overflow items if they were promoted to internal pages.  That turned
	 * out to be hard to get right, so I'm not doing it again.
	 *
	 * If it's a column-store page, we don't promote a key at all.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		key = &key_build;
		WT_CLEAR(key_build);

		key_item = (WT_ITEM *)WT_PAGE_BYTE(page);
		switch (WT_ITEM_TYPE(key_item)) {
		case WT_ITEM_DUP:
		case WT_ITEM_KEY:
			key->data = WT_ITEM_BYTE(key_item);
			key->size = WT_ITEM_LEN(key_item);
			WT_ITEM_TYPE_SET(&item, WT_ITEM_KEY);
			WT_ITEM_LEN_SET(&item, key->size);
			break;
		case WT_ITEM_DUP_OVFL:
		case WT_ITEM_KEY_OVFL:
			/*
			 * Assume overflow keys remain overflow keys when they
			 * are promoted; not necessarily true if internal nodes
			 * are larger than leaf nodes), but that's unlikely.
			 */
			WT_CLEAR(tmp_ovfl);
			WT_RET(__wt_bt_ovfl_copy(toc,
			    WT_ITEM_BYTE_OVFL(key_item), &tmp_ovfl));
			key->data = &tmp_ovfl;
			key->size = sizeof(tmp_ovfl);
			WT_ITEM_TYPE_SET(&item, WT_ITEM_KEY_OVFL);
			WT_ITEM_LEN_SET(&item, sizeof(tmp_ovfl));
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		key = NULL;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * There are two paths into this code based on whether the page already
	 * has a parent.
	 *
	 * If we have a page with no parent page, create the parent page.  In
	 * this path, there's not much to do -- allocate a parent page, copy
	 * reference information from the page to the parent, and we're done.
	 * This is a modified root-split: we're putting a single key on an
	 * internal page, which is illegal, but we know another page on this
	 * page's level was created, and it will be promoted to the parent at
	 * some point.  This is case #1.
	 *
	 * The second path into this code is if we have a page and its parent,
	 * but the page's reference information doesn't fit on the parent and
	 * we have to split the parent.  This path has two different cases,
	 * based on whether the page's parent itself has a parent.
	 *
	 * Here's a diagram of case #2, where the parent also has a parent:
	 *
	 * P2 -> P1 -> L	(case #2)
	 *
	 * The promoted key from leaf L won't fit onto P1, and so we split P1:
	 *
	 * P2 -> P1
	 *    -> P3 -> L
	 *
	 * In case #2, allocate P3 and copy reference information from the leaf
	 * page to it, then recursively call the promote code to promote the
	 * first entry from P3 to P2.
	 *
	 * Here's a diagram of case #3, where the parent does not have a parent,
	 * in other words, a root split:
	 *
	 * P1 -> L		(case #3)
	 *
	 * The promoted key from leaf L won't fit onto P1, and so we split P1:
	 *
	 * P1 ->
	 * P2 -> L
	 *
	 * In case #3, we allocate P2, copy reference information from the page
	 * to it, and then recursively call the promote code twice: first to
	 * promote the first entry from P1 to a new page, and again to promote
	 * the first entry from P2 to a new page, creating a new root level of
	 * the tree:
	 *
	 * P3 -> P1
	 *    -> P2 -> L
	 */
#ifdef HAVE_DIAGNOSTIC
#define	WT_STACK_ALLOC_INCR	2
#else
#define	WT_STACK_ALLOC_INCR	20
#endif
	/*
	 * To simplify the rest of the code, check to see if there's room for
	 * another entry in our stack structure.  Allocate the stack in groups
	 * of 20, which is probably big enough for any tree we'll ever see in
	 * the field, we'll never test the realloc code unless we work at it.
	 */
	if (stack->size == 0 || level == stack->size - 1) {
		u_int32_t bytes_allocated = stack->size * sizeof(WT_PAGE *);
		WT_RET(__wt_realloc(env, &bytes_allocated,
		    (stack->size + WT_STACK_ALLOC_INCR) * sizeof(WT_PAGE *),
		    &stack->page));
		stack->size += WT_STACK_ALLOC_INCR;
		/*
		 * Note, the stack structure may be entirely uninitialized here,
		 * that is, everything set to 0 bytes.  That's OK: the level of
		 * the stack starts out at 0, that is, the 0th element of the
		 * stack is the 1st level of internal/parent pages in the tree.
		 */
	}

	/*
	 * If we don't have a parent page, it's case #1 -- allocate the parent
	 * page immediately.
	 */
	if ((parent = stack->page[level]) != NULL)
		need_promotion = 0;
	else {
split:		switch (page->hdr->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_VAR:
			type = WT_PAGE_COL_INT;
			break;
		case WT_PAGE_DUP_INT:
		case WT_PAGE_DUP_LEAF:
			type = WT_PAGE_DUP_INT;
			break;
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			type = WT_PAGE_ROW_INT;
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		WT_ERR(__wt_bt_page_alloc(
		    toc, type, page->hdr->level + 1, db->intlmin, &next));

		/*
		 * Case #1 -- there's no parent, it's a root split.  If in a
		 * primary tree, update the description page, in an off-page
		 * duplicates tree, return the root of the off-page tree.
		 */
		if (parent == NULL) {
			switch (type) {
			case WT_PAGE_COL_INT:
			case WT_PAGE_ROW_INT:
				WT_ERR(__wt_bt_desc_write_root(
				    toc, next->addr, db->intlmin));
				break;
			case WT_PAGE_DUP_INT:
				*dup_root_addrp = next->addr;
				break;
			default:
				break;
			}
			need_promotion = 0;
		}
		/*
		 * Case #2 and #3.
		 */
		else {
			/*
			 * Case #3 -- it's a root split, so we have to promote
			 * a key from both of the parent pages: promote the key
			 * from the existing parent page.
			 */
			if (stack->page[level + 1] == NULL)
				WT_ERR(__wt_bt_promote(toc, parent,
				    incr, stack, level + 1, dup_root_addrp));

			/* Discard the old parent page, we have a new one. */
			WT_BULK_PAGE_OUT(toc, &parent, WT_MODIFIED);
			need_promotion = 1;
		}

		/* There's a new parent page, reset the stack. */
		stack->page[level] = parent = next;
		next = NULL;
	}

	/*
	 * See if the promoted data will fit (if they don't, we have to split).
	 * We don't need to check for overflow keys: if the key was an overflow,
	 * we already created a smaller, on-page version of it.
	 *
	 * If there's room, copy the promoted data onto the parent's page.
	 */
	switch (parent->hdr->type) {
	case WT_PAGE_COL_INT:
		if (parent->space_avail < sizeof(WT_OFF))
			goto split;

		parent_key = NULL;

		/* Create the WT_OFF reference. */
		WT_RECORDS(&off) = page->records;
		off.addr = page->addr;
		off.size =
		    page->hdr->level == WT_LLEAF ? db->leafmin : db->intlmin;

		/* Store the data item. */
		++parent->hdr->u.entries;
		parent_data = parent->first_free;
		memcpy(parent->first_free, &off, sizeof(off));
		parent->first_free += sizeof(WT_OFF);
		parent->space_avail -= sizeof(WT_OFF);

		/* Append new parent index to the in-memory page structures. */
		WT_ERR(__wt_bt_promote_col_indx(toc, parent, parent_data));
		break;
	case WT_PAGE_DUP_INT:
	case WT_PAGE_ROW_INT:
		if (parent->space_avail <
		    WT_ITEM_SPACE_REQ(sizeof(WT_OFF)) +
		    WT_ITEM_SPACE_REQ(key->size))
			goto split;

		/* Store the key. */
		++parent->hdr->u.entries;
		parent_key = (WT_ITEM *)parent->first_free;
		memcpy(parent->first_free, &item, sizeof(item));
		memcpy(parent->first_free + sizeof(item), key->data, key->size);
		parent->first_free += WT_ITEM_SPACE_REQ(key->size);
		parent->space_avail -= WT_ITEM_SPACE_REQ(key->size);

		/* Create the WT_ITEM(WT_OFF) reference. */
		WT_ITEM_LEN_SET(&item, sizeof(WT_OFF));
		WT_ITEM_TYPE_SET(&item, WT_ITEM_OFF);
		WT_RECORDS(&off) = page->records;
		off.addr = page->addr;
		off.size =
		    page->hdr->level == WT_LLEAF ? db->leafmin : db->intlmin;

		/* Store the data item. */
		++parent->hdr->u.entries;
		parent_data = parent->first_free;
		memcpy(parent->first_free, &item, sizeof(item));
		memcpy(parent->first_free + sizeof(item), &off, sizeof(off));
		parent->first_free += WT_ITEM_SPACE_REQ(sizeof(WT_OFF));
		parent->space_avail -= WT_ITEM_SPACE_REQ(sizeof(WT_OFF));

		/* Append new parent index to the in-memory page structures. */
		WT_ERR(__wt_bt_promote_row_indx(
		    toc, parent, parent_key, parent_data));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	parent->records += page->records;

	/*
	 * The promotion for case #2 and the second part of case #3 -- promote
	 * the key from the newly allocated internal page to its parent.
	 */
	if (need_promotion)
		WT_RET(__wt_bt_promote(
		    toc, parent, incr, stack, level + 1, dup_root_addrp));
	else {
		/*
		 * We've finished promoting the new page's key into the tree.
		 * What remains is to push the new record counts all the way
		 * to the root.  We've already corrected our current "parent"
		 * page, so proceed from there to the root.
		 */
		u_int i;
		for (i = level + 1; (parent = stack->page[i]) != NULL; ++i) {
			switch (parent->hdr->type) {
			case WT_PAGE_COL_INT:
				__wt_bt_promote_col_rec(parent, incr);
				break;
			case WT_PAGE_ROW_INT:
				__wt_bt_promote_row_rec(parent, incr);
				break;
			WT_ILLEGAL_FORMAT(db);
			}
			parent->records += incr;
		}
	}

err:	if (next != NULL)
		WT_BULK_PAGE_OUT(toc, &next, WT_MODIFIED);

	return (ret);
}

/*
 * __wt_bt_promote_col_indx --
 *	Append a new WT_OFF to an internal page's in-memory information.
 */
static int
__wt_bt_promote_col_indx(WT_TOC *toc, WT_PAGE *page, void *data)
{
	ENV *env;
	WT_COL *cip;
	u_int32_t allocated;

	env = toc->env;

	/*
	 * Make sure there's enough room in the in-memory index.  We don't grow
	 * the page's index anywhere else, so we don't have any "size" value
	 * separate from the number of entries the index array holds.  In this
	 * one case, to avoid re-allocating the array on every promotion, we
	 * allocate in chunks of 100, which we can detect as the count hits a
	 * new boundary.
	 */
	if (page->indx_count % 100 == 0) {
		allocated = page->indx_count * sizeof(WT_COL);
		WT_RET(__wt_realloc(env, &allocated,
		    (page->indx_count + 100) * sizeof(WT_COL),
		    &page->u.icol));
	}

	/* Add in the new index entry. */
	cip = page->u.icol + page->indx_count;
	++page->indx_count;

	/* Fill in the on-page data. */
	cip->data = data;

	return (0);
}

/*
 * __wt_bt_promote_row_indx --
 *	Append a new WT_ITEM_KEY/WT_OFF pair to an internal page's in-memory
 *	information.
 */
static int
__wt_bt_promote_row_indx(
    WT_TOC *toc, WT_PAGE *page, WT_ITEM *key, WT_ITEM *data)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_ROW *rip;
	u_int32_t allocated;

	env = toc->env;
	db = toc->db;
	idb = db->idb;

	/*
	 * Make sure there's enough room in the in-memory index.  We don't grow
	 * the page's index anywhere else, so we don't have any "size" value
	 * separate from the number of entries the index array holds.  In this
	 * one case, to avoid re-allocating the array on every promotion, we
	 * allocate in chunks of 100, which we can detect as the count hits a
	 * new boundary.
	 */
	if (page->indx_count % 100 == 0) {
		allocated = page->indx_count * sizeof(WT_ROW);
		WT_RET(__wt_realloc(env, &allocated,
		    (page->indx_count + 100) * sizeof(WT_ROW),
		    &page->u.irow));
	}

	/* Add in the new index entry. */
	rip = page->u.irow + page->indx_count;
	++page->indx_count;

	/*
	 * If there's a key, fill it in.  On-page uncompressed keys are directly
	 * referenced, but compressed or overflow keys reference the on-page
	 * item, with a size of 0 to indicate they need further processing.
	 */
	if (key != NULL)
		switch (WT_ITEM_TYPE(key)) {
		case WT_ITEM_KEY:
			if (idb->huffman_key == NULL) {
				WT_KEY_SET(rip,
				    WT_ITEM_BYTE(key), WT_ITEM_LEN(key));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			WT_KEY_SET_PROCESS(rip, key);
			break;
		WT_ILLEGAL_FORMAT(db);
		}

	/* Fill in the on-page data. */
	rip->data = data;

	return (0);
}

/*
 * __wt_bt_promote_col_rec --
 *	Promote the record count to a column-store parent.
 */
static inline void
__wt_bt_promote_col_rec(WT_PAGE *parent, u_int64_t incr)
{
	WT_COL *cip;

	/*
	 * Because of the bulk load pattern, we're always adding records to
	 * the subtree referenced by the last entry in each parent page.
	 */
	cip = parent->u.icol + (parent->indx_count - 1);
	WT_COL_OFF_RECORDS(cip) += incr;
}

/*
 * __wt_bt_promote_row_rec --
 *	Promote the record count to a row-store parent.
 */
static inline void
__wt_bt_promote_row_rec(WT_PAGE *parent, u_int64_t incr)
{
	WT_ROW *rip;

	/*
	 * Because of the bulk load pattern, we're always adding records to
	 * the subtree referenced by the last entry in each parent page.
	 */
	rip = parent->u.irow + (parent->indx_count - 1);
	WT_ROW_OFF_RECORDS(rip) += incr;
}

/*
 * __wt_bt_build_key_item --
 *	Process an inserted key item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
int
__wt_bt_build_key_item(
    WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl, int bulk_load)
{
	DB *db;
	IDB *idb;
	WT_STATS *stats;

	db = toc->db;
	idb = db->idb;
	stats = idb->stats;

	/*
	 * We're called with a DBT that references a data/size pair.  We can
	 * re-point that DBT's data and size fields to other memory, but we
	 * cannot allocate memory in that DBT -- all we can do is re-point it.
	 */

	/* Optionally compress the data using the Huffman engine. */
	if (idb->huffman_key != NULL) {
		WT_RET(__wt_huffman_encode(
		    idb->huffman_key, dbt->data, dbt->size,
		    &toc->key.data, &toc->key.mem_size, &toc->key.size));
		if (toc->key.size > dbt->size)
			WT_STAT_INCRV(stats,
			    HUFFMAN_KEY, toc->key.size - dbt->size);
		dbt->data = toc->key.data;
		dbt->size = toc->key.size;
	}

	/* Create an overflow object if the data won't fit. */
	if (dbt->size > db->leafitemsize) {
		WT_CLEAR(*ovfl);
		ovfl->size = dbt->size;
		if (bulk_load)
			WT_RET(__wt_bt_bulk_ovfl_write(toc, dbt, &ovfl->addr));
		else
			WT_RET(__wt_bt_ovfl_write(toc, dbt, &ovfl->addr));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_TYPE_SET(item, WT_ITEM_KEY_OVFL);
		WT_STAT_INCR(stats, OVERFLOW_KEY);
	} else
		WT_ITEM_TYPE_SET(item, WT_ITEM_KEY);

	WT_ITEM_LEN_SET(item, dbt->size);
	return (0);
}

/*
 * __wt_bt_build_data_item --
 *	Process an inserted data item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
int
__wt_bt_build_data_item(
    WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl, int bulk_load)
{
	DB *db;
	IDB *idb;
	WT_STATS *stats;

	db = toc->db;
	idb = db->idb;
	stats = idb->stats;

	/*
	 * We're called with a DBT that references a data/size pair.  We can
	 * re-point that DBT's data and size fields to other memory, but we
	 * cannot allocate memory in that DBT -- all we can do is re-point it.
	 */
	WT_CLEAR(*item);
	WT_ITEM_TYPE_SET(item, WT_ITEM_DATA);

	/*
	 * Handle zero-length items quickly -- this is a common value, it's
	 * a deleted column-store variable length item.
	 */
	if (dbt->size == 0) {
		WT_ITEM_LEN_SET(item, 0);
		return (0);
	}

	/* Optionally compress the data using the Huffman engine. */
	if (idb->huffman_data != NULL) {
		WT_RET(__wt_huffman_encode(
		    idb->huffman_data, dbt->data, dbt->size,
		    &toc->data.data, &toc->data.mem_size, &toc->data.size));
		if (toc->data.size > dbt->size)
			WT_STAT_INCRV(stats,
			    HUFFMAN_DATA, toc->data.size - dbt->size);
		dbt->data = toc->data.data;
		dbt->size = toc->data.size;
	}

	/* Create an overflow object if the data won't fit. */
	if (dbt->size > db->leafitemsize) {
		WT_CLEAR(*ovfl);
		ovfl->size = dbt->size;
		if (bulk_load)
			WT_RET(__wt_bt_bulk_ovfl_write(toc, dbt, &ovfl->addr));
		else
			WT_RET(__wt_bt_ovfl_write(toc, dbt, &ovfl->addr));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_TYPE_SET(item, WT_ITEM_DATA_OVFL);
		WT_STAT_INCR(stats, OVERFLOW_DATA);
	}

	WT_ITEM_LEN_SET(item, dbt->size);
	return (0);
}

/*
 * __wt_bt_bulk_ovfl_write --
 *	Store bulk-loaded overflow item in the database, returning the starting
 *	addr.
 */
static int
__wt_bt_bulk_ovfl_write(WT_TOC *toc, DBT *dbt, u_int32_t *addrp)
{
	DB *db;
	DBT *tmp;
	ENV *env;
	WT_PAGE page;
	WT_PAGE_HDR *hdr;
	u_int32_t remain, size;
	u_int8_t *p;

	db = toc->db;
	tmp = &toc->tmp2;
	env = toc->env;

	/* Make sure the TOC's scratch buffer is big enough. */
	size = WT_ALIGN(sizeof(WT_PAGE_HDR) + dbt->size, db->allocsize);
	if (tmp->mem_size < size)
		WT_RET(__wt_realloc(env, &tmp->mem_size, size, &tmp->data));
	p = tmp->data;
	remain = size;

	/*
	 * Allocate a chunk of file space -- we don't go through the cache
	 * to do that, it's going to take additional time because we'd have
	 * to pass messages around, and during a bulk load there shouldn't
	 * be any other threads of control accessing this file.
	 */
	WT_RET(
	    __wt_cache_alloc(toc, addrp, WT_HDR_BYTES_TO_ALLOC(db, dbt->size)));

	/* Initialize the page header and copy the overflow item in. */
	hdr = tmp->data;
	hdr->type = WT_PAGE_OVFL;
	hdr->level = WT_LLEAF;
	hdr->u.datalen = dbt->size;
	p += sizeof(WT_PAGE_HDR);
	remain -= sizeof(WT_PAGE_HDR);

	/* Copy the record into place. */
	memcpy(p, dbt->data, dbt->size);
	p += dbt->size;
	remain -= dbt->size;

	/* Clear any remaining bytes. */
	if (remain > 0)
		memset(p, 0, remain);

	/* Build a page structure. */
	WT_CLEAR(page);
	page.hdr = tmp->data;
	page.addr = *addrp;
	page.size = size;

	return (__wt_bt_bulk_write(toc, &page));
}

/*
 * __wt_bt_bulk_write --
 *	Write a bulk-loaded page into the database.
 */
static inline int
__wt_bt_bulk_write(WT_TOC *toc, WT_PAGE *page)
{
	DB *db;
	ENV *env;

	db = toc->db;
	env = toc->env;

	WT_ASSERT(env, __wt_bt_verify_page(toc, page, NULL) == 0);

	return (__wt_page_write(db, page));
}

/*
 * __wt_bt_dbt_copy --
 *	Get a copy of DBT referenced object.
 */
static int
__wt_bt_dbt_copy(ENV *env, DBT *orig, DBT *copy)
{
	if (copy->data == NULL || copy->mem_size < orig->size)
		WT_RET(__wt_realloc(
		    env, &copy->mem_size, orig->size, &copy->data));
	memcpy(copy->data, orig->data, copy->size = orig->size);

	return (0);
}
