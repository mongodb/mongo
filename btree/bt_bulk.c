/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 */

#include "wt_internal.h"

static int  __wt_bt_bulk_fix(DB *,
    void (*)(const char *, u_int64_t), int (*)(DB *, DBT **, DBT **));
static int  __wt_bt_bulk_var(DB *, u_int32_t,
    void (*)(const char *, u_int64_t), int (*)(DB *, DBT **, DBT **));
static int  __wt_bt_dbt_copy(ENV *, DBT *, DBT *);
static int  __wt_bt_dup_offpage(WT_TOC *, WT_PAGE *, DBT **, DBT **,
    DBT *, WT_ITEM *, u_int32_t, int (*cb)(DB *, DBT **, DBT **));
static int  __wt_bt_promote(WT_TOC *, WT_PAGE *, u_int64_t, u_int32_t *);
static int  __wt_bt_promote_col_indx(WT_TOC *, WT_PAGE *, void *);
static int  __wt_bt_promote_row_indx(WT_TOC *, WT_PAGE *, WT_ITEM *, void *);
static void __wt_bt_promote_col_rec(WT_PAGE *, u_int64_t);
static void __wt_bt_promote_row_rec(WT_PAGE *, u_int64_t);

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(DB *db, u_int32_t flags,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	IDB *idb;

	idb = db->idb;

	if (F_ISSET(idb, WT_COLUMN))
		WT_DB_FCHK(db, "DB.bulk_load", flags, 0);

	/*
	 * There are two styles of bulk-load: variable length pages or
	 * fixed-length pages.
	 */
	if (F_ISSET(idb, WT_COLUMN) && db->fixed_len != 0)
		return (__wt_bt_bulk_fix(db, f, cb));

	return (__wt_bt_bulk_var(db, flags, f, cb));
}

/*
 * __wt_bt_bulk_fix
 *	Db.bulk_load method for column-store, fixed-length database pages.
 */
static int
__wt_bt_bulk_fix(DB *db,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data;
	ENV *env;
	IDB *idb;
	WT_PAGE *page, *next;
	WT_TOC *toc;
	u_int len;
	u_int64_t insert_cnt;
	u_int16_t *last_repeat;
	u_int8_t *last_data;
	int ret;

	env = db->env;
	idb = db->idb;
	insert_cnt = 0;

	/* Figure out how large is the chunk we're storing on the page. */
	len = db->fixed_len +
	    (F_ISSET(idb, WT_REPEAT_COMP) ? sizeof(u_int16_t) : 0);

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.bulk_load");

	/*
	 * Allocate our first page -- we do this before we look at any keys
	 * because the first key/data items may be overflow items, in which
	 * case we would allocate page 0 as an overflow page, which is, for
	 * lack of a better phrase, "bad".
	 */
	WT_ERR(__wt_bt_page_alloc(toc, 1, &page));
	page->hdr->type = WT_PAGE_COL_FIX;

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key != NULL) {
			__wt_db_errx(db,
			    "column database keys are implied and "
			    "so should not be returned by the bulk "
			    "load input routine");
			ret = WT_ERROR;
			goto err;
		}
		if (data->size != db->fixed_len) {
			__wt_db_errx(db,
			    "length of %lu does not match the fixed-length "
			    "configuration for this database of %lu",
			    (u_long)data->size, (u_long)db->fixed_len);
			ret = WT_ERROR;
			goto err;
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f("Db.bulk_load", insert_cnt);
		WT_STAT_INCR(idb->stats, BULK_PAIRS_READ);

		/*
		 * If doing repeat compression, check to see if this record
		 * matches the last data inserted.   If there's a match try
		 * and increment that item's repeat count instead of entering
		 * new data.
		 */
		if (F_ISSET(idb, WT_REPEAT_COMP) && page->hdr->u.entries != 0) {
			if (*last_repeat < UINT16_MAX &&
			    memcmp(last_data, data->data, data->size) == 0) {
				++*last_repeat;
				++page->records;
				++page->hdr->u.entries;
				WT_STAT_INCR(idb->stats, BULK_REPEAT_COUNT);
				continue;
			}
		}

		/*
		 * We now have the data item to store on the page.  If there
		 * is insufficient space on the current page, allocate a new
		 * one.
		 */
		if (len > page->space_avail) {
			WT_ERR(__wt_bt_page_alloc(toc, 1, &next));
			next->hdr->type = WT_PAGE_COL_FIX;
			next->hdr->prevaddr = page->addr;
			page->hdr->nextaddr = next->addr;

			/*
			 * If we've finished with a page, promote its first key
			 * to its parent and write it.   The promotion function
			 * sets the page's parent address, which is the same for
			 * the newly allocated page.
			 */
			WT_ERR(__wt_bt_promote(toc, page, page->records, NULL));
			next->hdr->prntaddr = page->hdr->prntaddr;
			WT_ERR(__wt_bt_page_out(toc, page, WT_MODIFIED));

			/* Switch to the next page. */
			page = next;
		}

		++page->records;
		++page->hdr->u.entries;

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
	if (ret == 1) {
		ret = 0;

		/* Promote a key from any partially-filled page and write it. */
		if (page != NULL) {
			ret = __wt_bt_promote(toc, page, page->records, NULL);
			WT_ERR(__wt_bt_page_out(toc, page, WT_MODIFIED));
			page = NULL;
		}
	}

	/* Get a permanent root page reference. */
	WT_TRET(__wt_bt_root_page(toc));

	/* Wrap up reporting. */
	if (f != NULL)
		f("Db.bulk_load", insert_cnt);

	if (0) {
err:		if (page != NULL)
			(void)__wt_bt_page_out(toc, page, 0);
	}

	WT_TRET(toc->close(toc, 0));

	return (ret == 1 ? 0 : ret);
}

/*
 * __wt_bt_bulk_var --
 *	Db.bulk_load method for row or column-store variable-length database
 *	pages.
 */
static int
__wt_bt_bulk_var(DB *db, u_int32_t flags,
    void (*f)(const char *, u_int64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data, key_copy, data_copy, key_comp, data_comp;
	DBT *lastkey, lastkey_std, lastkey_ovfl;
	ENV *env;
	IDB *idb;
	WT_ITEM key_item, data_item, *dup_key, *dup_data;
	WT_OVFL key_ovfl, data_ovfl;
	WT_PAGE *page, *next;
	WT_TOC *toc;
	u_int64_t insert_cnt;
	u_int32_t dup_count, dup_space, len;
	int ret;

	env = db->env;
	idb = db->idb;
	ret = 0;

	dup_space = dup_count = 0;
	insert_cnt = 0;

	lastkey = &lastkey_std;
	WT_CLEAR(data_comp);
	WT_CLEAR(data_copy);
	WT_CLEAR(data_item);
	WT_CLEAR(key_comp);
	WT_CLEAR(key_copy);
	WT_CLEAR(key_item);
	WT_CLEAR(lastkey_ovfl);
	WT_CLEAR(lastkey_std);

	WT_RET(env->toc(env, 0, &toc));
	WT_TOC_DB_INIT(toc, db, "Db.bulk_load");

	/*
	 * Allocate our first page -- we do this before we look at any keys
	 * because the first key/data items may be overflow items, in which
	 * case we would allocate page 0 as an overflow page, which is, for
	 * lack of a better phrase, "bad".
	 */
	WT_ERR(__wt_bt_page_alloc(toc, 1, &page));
	page->hdr->type =
	    F_ISSET(idb, WT_COLUMN) ? WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF;

	while ((ret = cb(db, &key, &data)) == 0) {
		if (F_ISSET(idb, WT_COLUMN) ) {
			if (key != NULL) {
				__wt_db_errx(db,
				    "column database keys are implied and "
				    "so should not be returned by the bulk "
				    "load input routine");
				ret = WT_ERROR;
				goto err;
			}
		} else {
			if (key == NULL && !LF_ISSET(WT_DUPLICATES)) {
				__wt_db_errx(db,
				    "keys must be set unless duplicadtes are "
				    "configured");
				ret = WT_ERROR;
				goto err;
			}
			if (key->size == 0) {
				__wt_db_errx(db,
				    "zero-length keys are not supported");
				ret = WT_ERROR;
				goto err;
			}
		}

		/* Report on progress every 100 inserts. */
		if (f != NULL && ++insert_cnt % 100 == 0)
			f("Db.bulk_load", insert_cnt);
		WT_STAT_INCR(idb->stats, BULK_PAIRS_READ);

		/*
		 * Copy the caller's DBTs, we don't want to modify them.  But,
		 * copy them carefully, all we want is a pointer and a length.
		 * Then, optionally compress using a Huffman engine.
		 *
		 * We don't have a key to store on the page if we're building a
		 * column-store, and we don't store the key on the page in the
		 * case of a row-store duplicate data item.  The check from here
		 * on is if "key == NULL" for both cases, that is, there's no
		 * key to store.
		 */
		if (key != NULL) {
			key_copy.data = key->data;
			key_copy.size = key->size;
			key = &key_copy;

			if (idb->huffman_key != NULL) {
				WT_RET(__wt_huffman_encode(idb->huffman_key,
				    key->data, key->size, &key_comp.data,
				    &key_comp.data_len, &key_comp.size));
				if (key->size > key_comp.size)
					WT_STAT_INCRV(
					    idb->stats, BULK_HUFFMAN_KEY,
					    key->size - key_comp.size);
				key = &key_comp;
			}
		}

		data_copy.data = data->data;
		data_copy.size = data->size;
		data = &data_copy;
		if (idb->huffman_data != NULL) {
			WT_RET(__wt_huffman_encode(idb->huffman_data,
			    data->data, data->size, &data_comp.data,
			    &data_comp.data_len, &data_comp.size));
			if (data->size > data_comp.size)
				WT_STAT_INCRV(idb->stats, BULK_HUFFMAN_DATA,
				    data->size - data_comp.size);
			data = &data_comp;
		}

skip_read:	/*
		 * We pushed a set of duplicates off-page, and that routine
		 * returned an ending key/data pair to us.
		 */

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
			 * WT_ITEM_DATA_OVFL.  Correct the type.
			 */
			if (dup_count == 1)
				WT_ITEM_TYPE_SET(dup_data,
				    WT_ITEM_TYPE(dup_data) == WT_ITEM_DATA ?
				    WT_ITEM_DUP : WT_ITEM_DUP_OVFL);

			WT_STAT_INCR(idb->stats, BULK_DUP_DATA_READ);

			key = NULL;
		}

		/* Create overflow objects if the key or data won't fit. */
		if (key != NULL && key->size > db->leafitemsize) {
			/*
			 * If duplicates: we'll need a copy of the key for
			 * comparison with the next key.  It's an overflow
			 * object, so we can't just use the on-page memory.
			 * Save a copy.
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				lastkey = &lastkey_ovfl;
				WT_ERR(__wt_bt_dbt_copy(env, key, lastkey));
			}

			key_ovfl.len = key->size;
			WT_ERR(__wt_bt_ovfl_write(toc, key, &key_ovfl.addr));
			key->data = &key_ovfl;
			key->size = sizeof(key_ovfl);

			WT_ITEM_TYPE_SET(&key_item, WT_ITEM_KEY_OVFL);
			WT_STAT_INCR(idb->stats, BULK_OVERFLOW_KEY);
		} else
			WT_ITEM_TYPE_SET(&key_item, WT_ITEM_KEY);

		if (data->size > db->leafitemsize) {
			data_ovfl.len = data->size;
			WT_ERR(__wt_bt_ovfl_write(toc, data, &data_ovfl.addr));
			data_copy.data = &data_ovfl;
			data_copy.size = sizeof(data_ovfl);
			data = &data_copy;

			WT_ITEM_TYPE_SET(&data_item,
			    key == NULL && !F_ISSET(idb, WT_COLUMN) ?
			    WT_ITEM_DUP_OVFL : WT_ITEM_DATA_OVFL);

			WT_STAT_INCR(idb->stats, BULK_OVERFLOW_DATA);
		} else
			WT_ITEM_TYPE_SET(&data_item,
			    key == NULL && !F_ISSET(idb, WT_COLUMN) ?
			    WT_ITEM_DUP : WT_ITEM_DATA);

		/*
		 * We now have the key/data items to store on the page.  If
		 * there is insufficient space on the current page, allocate
		 * a new one.
		 */
		if ((key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			WT_ERR(__wt_bt_page_alloc(toc, 1, &next));
			next->hdr->type = F_ISSET(idb, WT_COLUMN) ?
			    WT_PAGE_COL_VAR : WT_PAGE_ROW_LEAF;
			next->hdr->prevaddr = page->addr;
			page->hdr->nextaddr = next->addr;

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
			if (dup_count > 1) {
				/*
				 * Set and re-set the page entry count -- we're
				 * moving a single key PLUS the duplicate set.
				 */
				page->hdr->u.entries -= (dup_count + 1);
				next->hdr->u.entries += (dup_count + 1);

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
			 * If we've finished with a page, promote its first key
			 * to its parent and write it.   The promotion function
			 * sets the page's parent address, which is the same for
			 * the newly allocated page.
			 */
			WT_ERR(__wt_bt_promote(toc, page, page->records, NULL));
			next->hdr->prntaddr = page->hdr->prntaddr;
			WT_ERR(__wt_bt_page_out(toc, page, WT_MODIFIED));

			/* Switch to the next page. */
			page = next;
		}

		++page->records;

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->hdr->u.entries;

			WT_ITEM_LEN_SET(&key_item, key->size);
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
		WT_ITEM_LEN_SET(&data_item, data->size);
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
		if (LF_ISSET(WT_DUPLICATES) &&
		    WT_ITEM_TYPE(&data_item) != WT_ITEM_DUP &&
		    WT_ITEM_TYPE(&data_item) != WT_ITEM_DUP_OVFL) {
			dup_count = 1;
			dup_space = data->size;
			dup_data = (WT_ITEM *)page->first_free;
		}
		page->first_free += WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: check to see if the duplicate set crosses
		 * the (roughly) 25% of the page space boundary.  If it does,
		 * move it offpage.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    WT_ITEM_TYPE(&data_item) == WT_ITEM_DUP ||
		    WT_ITEM_TYPE(&data_item) == WT_ITEM_DUP_OVFL) {
			++dup_count;
			dup_space += data->size;

			if (dup_space < db->leafsize / db->btree_dup_offpage)
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
			ret = __wt_bt_promote(toc, page, page->records, NULL);
			WT_ERR(__wt_bt_page_out(toc, page, WT_MODIFIED));
			page = NULL;
		}
	}

	/* Get a permanent root page reference. */
	WT_TRET(__wt_bt_root_page(toc));

	/* Wrap up reporting. */
	if (f != NULL)
		f("Db.bulk_load", insert_cnt);

	if (0) {
err:		if (page != NULL)
			(void)__wt_bt_page_out(toc, page, 0);
	}

	__wt_free(env, data_comp.data, data_comp.data_len);
	__wt_free(env, key_comp.data, key_comp.data_len);
	__wt_free(env, lastkey_ovfl.data, lastkey_ovfl.data_len);

	WT_TRET(toc->close(toc, 0));

	return (ret == 1 ? 0 : ret);
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
	IDB *idb;
	WT_ITEM data_item;
	WT_OFF offpage_item;
	WT_OVFL data_local;
	WT_PAGE *next, *page;
	u_int32_t len, root_addr;
	u_int8_t *p;
	int ret, tret;

	db = toc->db;
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
	ret = 0;

	/*
	 * Allocate and initialize a new page, and copy the duplicate set into
	 * place.
	 */
	WT_RET(__wt_bt_page_alloc(toc, 1, &page));
	page->hdr->type = WT_PAGE_DUP_LEAF;
	page->hdr->u.entries = dup_count;
	page->records = dup_count;
	len = (u_int32_t)(leaf_page->first_free - (u_int8_t *)dup_data);
	memcpy(page->first_free, dup_data, (size_t)len);
	__wt_bt_set_ff_and_sa_from_addr(page, WT_PAGE_BYTE(page) + len);

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
			__wt_db_errx(db, "zero-length keys are not supported");
			return (WT_ERROR);
		}
		WT_STAT_INCR(idb->stats, BULK_PAIRS_READ);
		WT_STAT_INCR(idb->stats, BULK_DUP_DATA_READ);

		/* Loading duplicates, so a key change means we're done. */
		if (lastkey->size != key->size ||
		    db->btree_compare_dup(db, lastkey, key) != 0) {
			*keyp = key;
			*datap = data;
			break;
		}

		/* Create overflow objects if the data won't fit. */
		if (data->size > db->leafitemsize) {
			data_local.len = data->size;
			WT_RET(
			    __wt_bt_ovfl_write(toc, data, &data_local.addr));
			data->data = &data_local;
			data->size = sizeof(data_local);
			WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DUP_OVFL);
			WT_STAT_INCR(idb->stats, BULK_OVERFLOW_DATA);
		} else
			WT_ITEM_TYPE_SET(&data_item, WT_ITEM_DUP);

		/*
		 * If there's insufficient space available, allocate a new
		 * page.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			WT_RET(__wt_bt_page_alloc(toc, 1, &next));
			next->hdr->type = WT_PAGE_DUP_LEAF;
			next->hdr->prevaddr = page->addr;
			page->hdr->nextaddr = next->addr;

			/*
			 * If we have finished with a page, promote its first
			 * key to its parent.  The promotion function sets the
			 * page's parent information, which is the same for
			 * the newly allocated page.
			 *
			 * If we promoted a key, we might have split, and so
			 * there may be a new offpage duplicates root page.
			 */
			WT_RET(__wt_bt_promote(
			    toc, page, page->records, &root_addr));
			next->hdr->prntaddr = page->hdr->prntaddr;
			WT_RET(__wt_bt_page_out(toc, page, WT_MODIFIED));

			/* Switch to the next page. */
			page = next;
		}

		++dup_count;			/* Total duplicate count. */
		++page->records;		/* On-page key/data count. */
		++page->hdr->u.entries;		/* On-page entry count. */

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
	if ((tret = __wt_bt_promote(toc,
	    page, page->records, &root_addr)) != 0 && (ret == 0 || ret == 1))
		ret = tret;
	if ((tret = __wt_bt_page_out(
	    toc, page, WT_MODIFIED)) != 0 && (ret == 0 || ret == 1))
		ret = tret;

	/*
	 * Replace the caller's duplicate set with a WT_OFF structure,
	 * and reset the caller's page information.
	 */
	WT_ITEM_LEN_SET(&data_item, sizeof(WT_OFF));
	WT_ITEM_TYPE_SET(&data_item,
	    root_addr == page->addr ? WT_ITEM_OFF_LEAF : WT_ITEM_OFF_INT);
	WT_RECORDS(&offpage_item) = dup_count,
	offpage_item.addr = root_addr;
	p = (u_int8_t *)dup_data;
	memcpy(p, &data_item, sizeof(data_item));
	memcpy(p + sizeof(data_item), &offpage_item, sizeof(offpage_item));
	__wt_bt_set_ff_and_sa_from_addr(leaf_page,
	    (u_int8_t *)dup_data + WT_ITEM_SPACE_REQ(sizeof(WT_OFF)));

	return (ret);
}

/*
 * __wt_bt_promote --
 *	Promote the first WT_ITEM on a variable length page to a parent.
 */
static int
__wt_bt_promote(
    WT_TOC *toc, WT_PAGE *page, u_int64_t increment, u_int32_t *root_addrp)
{
	DB *db;
	DBT *key, key_build;
	WT_ITEM *key_item, item, *parent_key;
	WT_OFF off;
	WT_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	u_int32_t parent_addr, tmp_root_addr;
	int need_promotion, ret, root_split;
	void *parent_data;

	db = toc->db;

	WT_CLEAR(item);

	if (root_addrp == NULL)
		root_addrp = &tmp_root_addr;
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
	 * There are two paths into this code based on whether the page has a
	 * parent (that is, if parent_addr is set).
	 *
	 * If parent_addr is NOT set, then we have a page with no parent page
	 * and we're creating the parent page.  In this path, there's not much
	 * to do -- allocate the parent page, copy the first key from the page
	 * to it, set the parent address on the original page and continue out.
	 * This is kind of a modified root-split: we're putting a single key
	 * onto an internal page, which is illegal, but we do it knowing that
	 * another page on this page's level has been created, and a key from
	 * it will be promoted at some point.  Call this case #1.
	 *
	 * The second path into this code is if parent_addr is set.  We have a
	 * page and its parent, the page's key wouldn't fit on the parent so
	 * we have to split the parent.  This path has two different cases,
	 * based on whether a parent of the parent exists.
	 *
	 * Here's a diagram of case #2, where the parent also has a parent:
	 *
	 * L -> P1 -> P2	(case #2)
	 *
	 * The promoted key from leaf L didn't fit onto P1, and so we split P1:
	 *
	 *	P1
	 *	^
	 *	|  -> P2
	 *	v
	 * L -> P3
	 *
	 * In case #2, we allocate P3, copy the first key from the leaf page to
	 * it, then recursively call the promote code to promote the first key
	 * from P3 to P2.
	 *
	 * Here's a diagram of case #3, where the parent does not have a parent,
	 * in other words, a root split:
	 *
	 * L -> P1		(case #3)
	 *
	 * The promoted key from leaf L didn't fit onto P1, and so we split P1:
	 *
	 *	P1
	 *	^
	 *	|
	 *	v
	 * L -> P2
	 *
	 * In case #3, we allocate P2, copy the first key from the page to it,
	 * but then recursively call the promote code to promote the first key
	 * from P1 to a new page, and again to promote the first key from P2 to
	 * a new page, creating a new root level of the tree:
	 *
	 *	P1
	 *	^
	 *	| -> P3
	 *	v
	 * L -> P2
	 */
	parent_addr = page->hdr->prntaddr;
	if (parent_addr == WT_ADDR_INVALID) {
split:		WT_ERR(__wt_bt_page_alloc(toc, 0, &next));
		switch (page->hdr->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_VAR:
			next->hdr->type = WT_PAGE_COL_INT;
			break;
		case WT_PAGE_DUP_INT:
		case WT_PAGE_DUP_LEAF:
			next->hdr->type = WT_PAGE_DUP_INT;
			break;
		case WT_PAGE_ROW_INT:
		case WT_PAGE_ROW_LEAF:
			next->hdr->type = WT_PAGE_ROW_INT;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		/*
		 * Case #1 -- it's a modified root split, so if we're in the
		 * primary tree we have to update the WT_PAGE_DESC area.
		 */
		if (parent_addr == WT_ADDR_INVALID) {
			root_split =  1;
			*root_addrp = next->addr;
			need_promotion = 0;
		}
		/*
		 * Case #2 and #3.
		 */
		else {
			/*
			 * Link the new parent page into the level's page
			 * chain.
			 */
			next->hdr->prevaddr = parent_addr;
			parent->hdr->nextaddr = next->addr;

			/*
			 * Case #3 -- it's a root split, so we have to promote
			 * a key from both of the parent pages.  First, promote
			 * the key from the existing parent page.
			 */
			if (parent->hdr->prntaddr == WT_ADDR_INVALID) {
				root_split = 1;
				WT_ERR(__wt_bt_promote(
				    toc, parent, increment, root_addrp));
			} else
				root_split = 0;

			next->hdr->prntaddr = parent->hdr->prntaddr;

			/* Discard the old parent page, we have a new one. */
			WT_ERR(__wt_bt_page_out(toc, parent, WT_MODIFIED));

			need_promotion = 1;
		}

		/*
		 * In case #1 and case #3, we're doing a root split:
		 *
		 * If it's the primary tree, update the WT_PAGE_DESC area
		 * of the database.
		 *
		 * Update the returned database level.
		 */
		if (root_split &&
		   (next->hdr->type == WT_PAGE_COL_INT ||
		   next->hdr->type == WT_PAGE_ROW_INT))
			WT_ERR(__wt_bt_desc_write(toc, *root_addrp));

		/* There's a new parent page, update the page's parent ref. */
		page->hdr->prntaddr = next->addr;
		parent = next;
		next = NULL;
	} else {
		WT_ERR(__wt_bt_page_in(toc, parent_addr, 0, 1, &parent));

		need_promotion = 0;
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
		off.addr = page->addr;
		WT_RECORDS(&off) = page->records;

		/* Store the data item. */
		++parent->hdr->u.entries;
		parent_data = parent->first_free;
		memcpy(parent->first_free, &off, sizeof(off));
		parent->first_free += sizeof(WT_OFF);
		parent->space_avail -= sizeof(WT_OFF);

		/*
		 * Set the WT_OFFPAGE_REF_LEAF flag in the on-disk page, as
		 * necessary.
		 */
		if (parent->hdr->u.entries == 1 &&
		    (page->hdr->type == WT_PAGE_COL_FIX ||
		    page->hdr->type == WT_PAGE_COL_VAR))
			F_SET(parent->hdr, WT_OFFPAGE_REF_LEAF);

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
		WT_ITEM_TYPE_SET(&item,
		    page->hdr->type == WT_PAGE_DUP_INT ||
		    page->hdr->type == WT_PAGE_ROW_INT ?
		    WT_ITEM_OFF_INT : WT_ITEM_OFF_LEAF);
		WT_RECORDS(&off) = page->records;
		off.addr = page->addr;

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
		ret = __wt_bt_promote(toc, parent, increment, root_addrp);
	else	/*
		 * We've finished promoting the new page's key into the tree.
		 * What remains is to push the new record counts all the way
		 * to the root.  We've already corrected our current "parent"
		 * page, so proceed from there to the root.
		 */
		while (
		    (parent_addr = parent->hdr->prntaddr) != WT_ADDR_INVALID) {
			WT_ERR(__wt_bt_page_out(toc, parent, WT_MODIFIED));
			parent = NULL;
			WT_ERR(
			    __wt_bt_page_in(toc, parent_addr, 0, 1, &parent));

			switch (parent->hdr->type) {
			case WT_PAGE_COL_INT:
				__wt_bt_promote_col_rec(parent, increment);
				break;
			case WT_PAGE_ROW_INT:
				__wt_bt_promote_row_rec(parent, increment);
				break;
			WT_ILLEGAL_FORMAT(db);
			}
			parent->records += increment;
		}

err:	/* Discard the parent page. */
	if (parent != NULL)
		WT_TRET(__wt_bt_page_out(toc, parent, WT_MODIFIED));
	if (next != NULL)
		WT_TRET(__wt_bt_page_out(toc, next, WT_MODIFIED));

	return (ret);
}

/*
 * __wt_bt_promote_col_indx --
 *	Append a new WT_OFF to an internal page's in-memory information.
 */
static int
__wt_bt_promote_col_indx(WT_TOC *toc, WT_PAGE *page, void *page_data)
{
	ENV *env;
	WT_COL_INDX *ip;
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
		allocated = page->indx_count * sizeof(WT_COL_INDX);
		WT_RET(__wt_realloc(env, &allocated,
		    (page->indx_count + 100) * sizeof(WT_COL_INDX),
		    &page->u.c_indx));
	}

	/* Add in the new index entry. */
	ip = page->u.c_indx + page->indx_count;
	++page->indx_count;

	/* Fill in the on-page data. */
	ip->page_data = page_data;

	return (0);
}

/*
 * __wt_bt_promote_row_indx --
 *	Append a new WT_ITEM_KEY/WT_OFF pair to an internal page's in-memory
 *	information.
 */
static int
__wt_bt_promote_row_indx(
    WT_TOC *toc, WT_PAGE *page, WT_ITEM *key, void *page_data)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_ROW_INDX *ip;
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
		allocated = page->indx_count * sizeof(WT_ROW_INDX);
		WT_RET(__wt_realloc(env, &allocated,
		    (page->indx_count + 100) * sizeof(WT_ROW_INDX),
		    &page->u.r_indx));
	}

	/* Add in the new index entry. */
	ip = page->u.r_indx + page->indx_count;
	++page->indx_count;

	/*
	 * If there's a key, fill it in.  On-page keys are directly referenced.
	 * Overflow keys, we grab the size but otherwise leave them alone.
	 */
	if (key != NULL) {
		switch (WT_ITEM_TYPE(key)) {
		case WT_ITEM_KEY:
			ip->data = WT_ITEM_BYTE(key);
			ip->size = WT_ITEM_LEN(key);
			break;
		case WT_ITEM_KEY_OVFL:
			ip->size = WT_ITEM_BYTE_OVFL(key)->len;
			break;
		WT_ILLEGAL_FORMAT(db);
		}

		if (idb->huffman_key != NULL)
			F_SET(ip, WT_HUFFMAN);
	}

	/* Fill in the on-page data. */
	ip->page_data = page_data;

	return (0);
}

/*
 * __wt_bt_promote_col_rec --
 *	Promote the record count to a column-store parent.
 */
static void
__wt_bt_promote_col_rec(WT_PAGE *parent, u_int64_t increment)
{
	WT_COL_INDX *ip;

	/*
	 * Because of the bulk load pattern, we're always adding records to
	 * the subtree referenced by the last entry in each parent page.
	 */
	ip = parent->u.c_indx + (parent->indx_count - 1);
	WT_COL_OFF_RECORDS(ip) += increment;
}

/*
 * __wt_bt_promote_row_rec --
 *	Promote the record count to a row-store parent.
 */
static void
__wt_bt_promote_row_rec(WT_PAGE *parent, u_int64_t increment)
{
	WT_ROW_INDX *ip;

	/*
	 * Because of the bulk load pattern, we're always adding records to
	 * the subtree referenced by the last entry in each parent page.
	 */
	ip = parent->u.r_indx + (parent->indx_count - 1);
	WT_ROW_OFF_RECORDS(ip) += increment;
}

/*
 * __wt_bt_dbt_copy --
 *	Get a local copy of an overflow key.
 */
static int
__wt_bt_dbt_copy(ENV *env, DBT *orig, DBT *copy)
{
	if (copy->data == NULL || copy->data_len < orig->size)
		WT_RET(__wt_realloc(
		    env, &copy->data_len, orig->size, &copy->data));
	memcpy(copy->data, orig->data, copy->size = orig->size);

	return (0);
}
