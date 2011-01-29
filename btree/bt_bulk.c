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
	WT_PAGE	*page;				/* page header */
	uint8_t	*first_free;			/* page's first free byte */
	uint32_t space_avail;			/* page's space available */

	DBT	*tmp;				/* page-in-a-buffer */
	void	*data;				/* last on-page WT_COL/WT_ROW */
} WT_STACK_ELEM;
typedef struct {
	WT_STACK_ELEM *elem;			/* stack */
	u_int size;				/* stack size */
} WT_STACK;

static int __wt_bulk_dbt_copy(ENV *, DBT *, DBT *);
static int __wt_bulk_dup_offpage(WT_TOC *, DBT **, DBT **, DBT *, WT_ITEM *, uint32_t, uint32_t, WT_OFF *, int (*)(DB *, DBT **, DBT **));
static int __wt_bulk_fix(WT_TOC *, void (*)(const char *, uint64_t), int (*)(DB *, DBT **, DBT **));
static int __wt_bulk_ovfl_copy(WT_TOC *, WT_OVFL *, WT_OVFL *);
static int __wt_bulk_ovfl_write(WT_TOC *, DBT *, WT_OVFL *);
static int __wt_bulk_promote(WT_TOC *, WT_PAGE *, uint64_t, WT_STACK *, u_int, uint32_t *);
static int __wt_bulk_scratch_page(WT_TOC *, uint32_t, uint32_t, uint32_t, WT_PAGE **, DBT **);
static int __wt_bulk_stack_put(WT_TOC *, WT_STACK *);
static int __wt_bulk_var(WT_TOC *, uint32_t, void (*)(const char *, uint64_t), int (*)(DB *, DBT **, DBT **));
static int __wt_item_build_key(WT_TOC *, DBT *, WT_ITEM *, WT_OVFL *);

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(WT_TOC *toc, uint32_t flags,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	IDB *idb;
	uint32_t addr;

	db = toc->db;
	idb = db->idb;

	/*
	 * XXX
	 * Write out the description record -- this goes away when we figure
	 * out how the table schema is going to work, but for now, we use the
	 * first sector, and this file extend makes sure we don't allocate it
	 * as a table page.
	 */
	WT_RET(__wt_file_alloc(toc, &addr, 512));

	if (F_ISSET(idb, WT_COLUMN))
		WT_DB_FCHK(db, "DB.bulk_load", flags, 0);

	/*
	 * There are two styles of bulk-load: variable length pages or
	 * fixed-length pages.
	 */
	if (F_ISSET(idb, WT_COLUMN) && db->fixed_len != 0)
		WT_RET(__wt_bulk_fix(toc, f, cb));
	else
		WT_RET(__wt_bulk_var(toc, flags, f, cb));

	/* Get a permanent root page reference. */
	return (__wt_root_pin(toc));
}

/*
 * __wt_bulk_fix
 *	Db.bulk_load method for column-store, fixed-length database pages.
 */
static int
__wt_bulk_fix(WT_TOC *toc,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, *tmp;
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t len, space_avail;
	uint16_t *last_repeat;
	uint8_t *first_free, *last_data;
	int rcc, ret;

	db = toc->db;
	tmp = NULL;
	idb = db->idb;
	insert_cnt = 0;
	WT_CLEAR(stack);

	rcc = F_ISSET(idb, WT_REPEAT_COMP) ? 1 : 0;

	/* Figure out how large is the chunk we're storing on the page. */
	len = db->fixed_len;
	if (rcc)
		len += sizeof(uint16_t);

	/* Get a scratch buffer and make it look like our work page. */
	WT_ERR(__wt_bulk_scratch_page(toc, db->leafmin,
	    rcc ? WT_PAGE_COL_RCC : WT_PAGE_COL_FIX, WT_LLEAF, &page, &tmp));
	hdr = page->hdr;
	hdr->start_recno = 1;
	__wt_set_ff_and_sa_from_offset(
	    page, WT_PAGE_BYTE(page), &first_free, &space_avail);

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key != NULL) {
			__wt_api_db_errx(db,
			    "column database keys are implied and so should "
			    "not be set by the bulk load input routine");
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
		if (rcc && hdr->u.entries != 0)
			if (*last_repeat < UINT16_MAX &&
			    memcmp(last_data, data->data, data->size) == 0) {
				++*last_repeat;
				++page->records;
				WT_STAT_INCR(idb->stats, REPEAT_COUNT);
				continue;
			}

		/*
		 * We now have the data item to store on the page.  If there
		 * is insufficient space on the current page, allocate a new
		 * one.
		 */
		if (len > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 */
			WT_ERR(__wt_bulk_promote(
			    toc, page, page->records, &stack, 0, NULL));
			WT_ERR(__wt_page_write(toc, page));
			hdr->u.entries = 0;
			page->records = 0;
			hdr->start_recno = insert_cnt;
			WT_ERR(
			    __wt_file_alloc(toc, &page->addr, db->leafmin));
			__wt_set_ff_and_sa_from_offset(page,
			    WT_PAGE_BYTE(page), &first_free, &space_avail);
		}

		++hdr->u.entries;
		++page->records;

		/*
		 * Copy the data item onto the page -- if we're doing repeat
		 * compression, track the location of the item for comparison.
		 */
		if (rcc) {
			last_repeat = (uint16_t *)first_free;
			*last_repeat = 1;
			first_free += sizeof(uint16_t);
			space_avail -= sizeof(uint16_t);
			last_data = first_free;
		}
		memcpy(first_free, data->data, data->size);
		first_free += data->size;
		space_avail -= data->size;
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (hdr->u.entries != 0) {
		ret = __wt_bulk_promote(
		    toc, page, page->records, &stack, 0, NULL);
		WT_ERR(__wt_page_write(toc, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(toc, &stack));
	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_var --
 *	Db.bulk_load method for row or column-store variable-length database
 *	pages.
 */
static int
__wt_bulk_var(WT_TOC *toc, uint32_t flags,
    void (*f)(const char *, uint64_t), int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, key_copy, data_copy;
	DBT *lastkey, *lastkey_copy, lastkey_std;
	DBT *tmp1, *tmp2;
	ENV *env;
	IDB *idb;
	WT_ITEM key_item, data_item, *dup_key, *dup_data;
	WT_OFF off;
	WT_OVFL key_ovfl, data_ovfl;
	WT_PAGE *page, *next;
	WT_STACK stack;
	uint64_t insert_cnt;
	uint32_t dup_count, dup_space, len, next_space_avail, space_avail;
	uint8_t *first_free, *next_first_free, *p, type;
	int ret;

	db = toc->db;
	tmp1 = tmp2 = NULL;
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
	WT_CLEAR(lastkey_std);
	WT_ERR(__wt_scr_alloc(toc, 0, &lastkey_copy));

	/* Get a scratch buffer and make it look like our work page. */
	WT_ERR(__wt_bulk_scratch_page(
	    toc, db->leafmin, type, WT_LLEAF, &page, &tmp1));
	__wt_set_ff_and_sa_from_offset(
	    page, WT_PAGE_BYTE(page), &first_free, &space_avail);
	if (type == WT_PAGE_COL_VAR)
		page->hdr->start_recno = 1;

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

skip_read:	/*
		 * We pushed a set of duplicates off-page, and that routine
		 * returned an ending key/data pair to us.
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

		/* Build the data item we're going to store on the page. */
		WT_ERR(__wt_item_build_data(
		    toc, data, &data_item, &data_ovfl, 0));

		/*
		 * Check for duplicate keys; we don't store the key on the page
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
				WT_ITEM_SET_TYPE(dup_data,
				    WT_ITEM_TYPE(dup_data) == WT_ITEM_DATA ?
				    WT_ITEM_DATA_DUP : WT_ITEM_DATA_DUP_OVFL);
			}

			/* Reset the type of the current item to a duplicate. */
			WT_ITEM_SET_TYPE(&data_item,
			    WT_ITEM_TYPE(&data_item) == WT_ITEM_DATA ?
			    WT_ITEM_DATA_DUP : WT_ITEM_DATA_DUP_OVFL);

			WT_STAT_INCR(idb->stats, DUPLICATE_ITEMS_INSERTED);

			key = NULL;
		} else {
			/*
			 * It's a new key, but if duplicates are possible we'll
			 * need a copy of the key for comparison with the next
			 * key.  If the key is Huffman encoded or an overflow
			 * object, we can't use the on-page version, we have to
			 * save a copy.
			 */
			if (LF_ISSET(WT_DUPLICATES) &&
			    (key->size > db->leafitemsize ||
			    idb->huffman_key != NULL)) {
				WT_ERR(
				    __wt_bulk_dbt_copy(env, key, lastkey_copy));
				lastkey = lastkey_copy;
			} else
				lastkey = NULL;

			dup_count = 0;
		}

		/* Build the key item we're going to store on the page. */
		if (key != NULL)
			WT_ERR(__wt_item_build_key(
			    toc, key, &key_item, &key_ovfl));

		/*
		 * We now have the key/data items to store on the page.  If
		 * there is insufficient space on the current page, allocate
		 * a new one.
		 */
		if ((key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail) {
			WT_ERR(__wt_bulk_scratch_page(toc,
			    db->leafmin, type, WT_LLEAF, &next, &tmp2));
			__wt_set_ff_and_sa_from_offset(next,
			    WT_PAGE_BYTE(next),
			    &next_first_free, &next_space_avail);
			if (type == WT_PAGE_COL_VAR)
				next->hdr->start_recno = insert_cnt;

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
				len =
				    (uint32_t)(first_free - (uint8_t *)dup_key);
				memcpy(next_first_free, dup_key, len);
				next_first_free += len;
				next_space_avail -= len;

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
				dup_data = (WT_ITEM *)((uint8_t *)dup_key +
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
			WT_ERR(__wt_bulk_promote(
			    toc, page, page->records, &stack, 0, NULL));
			WT_ERR(__wt_page_write(toc, page));
			__wt_scr_release(&tmp1);

			/*
			 * Discard the last page, and switch to the next page.
			 *
			 * XXX
			 * The obvious speed-up here is to re-initialize page
			 * instead of discarding it and acquiring it again as
			 * as soon as the just-allocated page fills up.  I am
			 * not doing that deliberately: eventually we'll use
			 * asynchronous I/O in bulk load, which means the page
			 * won't be reusable until the I/O completes.
			 */
			page = next;
			first_free = next_first_free;
			space_avail = next_space_avail;
			next = NULL;
			next_first_free = NULL;
			next_space_avail = 0;
			tmp1 = tmp2;
			tmp2 = NULL;
		}

		++page->records;

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->hdr->u.entries;

			memcpy(first_free, &key_item, sizeof(key_item));
			memcpy(first_free +
			    sizeof(key_item), key->data, key->size);
			space_avail -= WT_ITEM_SPACE_REQ(key->size);

			/*
			 * If processing duplicates we'll need a copy of the key
			 * for comparison with the next key.  If the key was an
			 * overflow or Huffman encoded item, we already have a
			 * copy -- otherwise, use the copy we just put on the
			 * page.
			 *
			 * We also save the location for the key of any current
			 * duplicate set in case we have to move the set to a
			 * different page (the case where a duplicate set isn't
			 * large enough to move offpage, but doesn't entirely
			 * fit on this page).
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				if (lastkey == NULL) {
					lastkey = &lastkey_std;
					lastkey_std.data =
					    WT_ITEM_BYTE(first_free);
					lastkey_std.size = key->size;
				}
				dup_key = (WT_ITEM *)first_free;
			}
			first_free += WT_ITEM_SPACE_REQ(key->size);
		}

		/* Copy the data item onto the page. */
		++page->hdr->u.entries;
		memcpy(first_free, &data_item, sizeof(data_item));
		memcpy(first_free + sizeof(data_item), data->data, data->size);
		space_avail -= WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: if this isn't a duplicate data item, save
		 * the item location, since it's potentially the first of a
		 * duplicate data set, and we need to know where duplicate
		 * data sets start.  Additionally, reset the counter and
		 * space calculation.
		 */
		if (LF_ISSET(WT_DUPLICATES) && dup_count == 0) {
			dup_space = data->size;
			dup_data = (WT_ITEM *)first_free;
		}
		first_free += WT_ITEM_SPACE_REQ(data->size);

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
			 * Move the duplicate set off our page, and read in the
			 * rest of the off-page duplicate set.
			 */
			WT_ERR(__wt_bulk_dup_offpage(toc, &key, &data, lastkey,
			    dup_data,
			    (uint32_t)(first_free - (uint8_t *)dup_data),
			    dup_count, &off, cb));

			/* Reset the page entry and record counts. */
			page->hdr->u.entries -= (dup_count - 1);
			page->records -= dup_count;
			page->records += WT_RECORDS(&off);

			/*
			 * Replace the duplicate set with a WT_OFF structure,
			 * that is, we've replaced dup_count entries with a
			 * single entry.
			 */
			WT_ITEM_SET(&data_item, WT_ITEM_OFF, sizeof(WT_OFF));
			p = (uint8_t *)dup_data;
			memcpy(p, &data_item, sizeof(data_item));
			memcpy(p + sizeof(data_item), &off, sizeof(WT_OFF));
			__wt_set_ff_and_sa_from_offset(page,
			    (uint8_t *)p + WT_ITEM_SPACE_REQ(sizeof(WT_OFF)),
			    &first_free, &space_avail);

			/* Reset local counters. */
			dup_count = dup_space = 0;

			goto skip_read;
		}
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret != 1)
		goto err;
	ret = 0;

	/* Promote a key from any partially-filled page and write it. */
	if (page->hdr->u.entries != 0) {
		WT_ERR(__wt_bulk_promote(
		    toc, page, page->records, &stack, 0, NULL));
		WT_ERR(__wt_page_write(toc, page));
	}

	/* Wrap up reporting. */
	if (f != NULL)
		f(toc->name, insert_cnt);

err:	WT_TRET(__wt_bulk_stack_put(toc, &stack));
	if (lastkey_copy != NULL)
		__wt_scr_release(&lastkey_copy);
	if (tmp1 != NULL)
		__wt_scr_release(&tmp1);
	if (tmp2 != NULL)
		__wt_scr_release(&tmp2);

	return (ret);
}

/*
 * __wt_bulk_dup_offpage --
 *	Move the last set of duplicates on the page to a page of their own,
 *	then load the rest of the duplicate set.
 */
static int
__wt_bulk_dup_offpage(WT_TOC *toc, DBT **keyp, DBT **datap, DBT *lastkey,
    WT_ITEM *dup_data, uint32_t dup_len, uint32_t dup_count, WT_OFF *off,
    int (*cb)(DB *, DBT **, DBT **))
{
	DB *db;
	DBT *key, *data, *tmp;
	IDB *idb;
	WT_ITEM data_item;
	WT_OVFL data_ovfl;
	WT_PAGE *page;
	WT_STACK stack;
	uint32_t root_addr, space_avail;
	uint8_t *first_free;
	int ret, success_return;

	db = toc->db;
	idb = db->idb;
	success_return = 0;

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
	 * off --
	 *	Callers WT_OFF structure, which we have to fill in.
	 * cb --
	 *	User's callback function.
	 */

	WT_CLEAR(data_item);
	WT_CLEAR(stack);
	ret = 0;

	/* Get a scratch buffer and make it look like our work page. */
	WT_ERR(__wt_bulk_scratch_page(toc,
	    db->leafmin, WT_PAGE_DUP_LEAF, WT_LLEAF, &page, &tmp));
	__wt_set_ff_and_sa_from_offset(
	    page, WT_PAGE_BYTE(page), &first_free, &space_avail);

	/* Move the duplicates onto the newly allocated page. */
	page->records = dup_count;
	page->hdr->u.entries = dup_count;
	memcpy(first_free, dup_data, (size_t)dup_len);
	first_free += dup_len;
	space_avail -= dup_len;

	/*
	 * Unless we have enough duplicates to split this page, it will be the
	 * "root" of the offpage duplicates.
	 */
	root_addr = page->addr;

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

		/* Build the data item we're going to store on the page. */
		WT_ERR(__wt_item_build_data(
		    toc, data, &data_item, &data_ovfl, WT_IS_DUP));

		/*
		 * If there's insufficient space available, allocate a new
		 * page.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > space_avail) {
			/*
			 * We've finished with the page: promote its first key
			 * to its parent and discard it, then switch to the new
			 * page.
			 *
			 * If we promoted a key, we might have split, and so
			 * there may be a new offpage duplicates root page.
			 */
			WT_RET(__wt_bulk_promote(toc,
			    page, page->records, &stack, 0, &root_addr));
			WT_ERR(__wt_page_write(toc, page));
			page->records = 0;
			page->hdr->u.entries = 0;
			__wt_set_ff_and_sa_from_offset(page,
			    WT_PAGE_BYTE(page), &first_free, &space_avail);
		}

		++dup_count;			/* Total duplicate count */
		++page->records;		/* On-page key/data count */
		++page->hdr->u.entries;		/* On-page entry count */

		/* Copy the data item onto the page. */
		WT_ITEM_SET_LEN(&data_item, data->size);
		memcpy(first_free, &data_item, sizeof(data_item));
		memcpy(first_free + sizeof(data_item), data->data, data->size);
		space_avail -= WT_ITEM_SPACE_REQ(data->size);
		first_free += WT_ITEM_SPACE_REQ(data->size);
	}

	/*
	 * Ret values of 1 and 0 are both "OK", the ret value of 1 means we
	 * reached the end of the bulk input.   Save the successful return
	 * for our final return value.
	 */
	if (ret != 0 && ret != 1)
		goto err;
	success_return = ret;

	/* Promote a key from the partially-filled page and write it. */
	WT_ERR(
	    __wt_bulk_promote(toc, page, page->records, &stack, 0, &root_addr));
	WT_ERR(__wt_page_write(toc, page));

	/* Fill in the caller's WT_OFF structure. */
	WT_RECORDS(off) = dup_count;
	off->addr = root_addr;
	off->size = db->intlmin;

err:	WT_TRET(__wt_bulk_stack_put(toc, &stack));
	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret == 0 ? success_return : ret);
}

/*
 * __wt_bulk_promote --
 *	Promote the first entry on a page to its parent.
 */
static int
__wt_bulk_promote(WT_TOC *toc, WT_PAGE *page, uint64_t incr,
    WT_STACK *stack, u_int level, uint32_t *dup_root_addrp)
{
	DB *db;
	DBT *key, key_build, *next_tmp;
	ENV *env;
	WT_ITEM *key_item, item;
	WT_OFF off;
	WT_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	WT_PAGE_HDR *hdr;
	WT_STACK_ELEM *elem;
	uint32_t next_space_avail;
	uint8_t *next_first_free;
	u_int type;
	int need_promotion, ret;
	void *parent_data;

	db = toc->db;
	env = toc->env;
	hdr = page->hdr;
	WT_CLEAR(item);
	next_tmp = NULL;
	next = parent = NULL;
	ret = 0;

	/*
	 * If it's a row-store, get a copy of the first item on the page -- it
	 * might be an overflow item, in which case we need to make a copy for
	 * the database.  Most versions of Berkeley DB tried to reference count
	 * overflow items if they were promoted to internal pages.  That turned
	 * out to be hard to get right, so I'm not doing it again.
	 *
	 * If it's a column-store page, we don't promote a key at all.
	 */
	switch (hdr->type) {
	case WT_PAGE_DUP_INT:
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		key = &key_build;
		WT_CLEAR(key_build);

		key_item = (WT_ITEM *)WT_PAGE_BYTE(page);
		switch (WT_ITEM_TYPE(key_item)) {
		case WT_ITEM_KEY:
		case WT_ITEM_DATA_DUP:
			key->data = WT_ITEM_BYTE(key_item);
			key->size = WT_ITEM_LEN(key_item);
			switch (hdr->type) {
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				WT_ITEM_SET(&item, WT_ITEM_KEY, key->size);
				break;
			case WT_PAGE_DUP_INT:
			case WT_PAGE_DUP_LEAF:
				WT_ITEM_SET(&item, WT_ITEM_KEY_DUP, key->size);
				break;
			default:		/* Not possible */
				break;
			}
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			/*
			 * Assume overflow keys remain overflow keys when they
			 * are promoted; not necessarily true if internal nodes
			 * are larger than leaf nodes), but that's unlikely.
			 */
			WT_CLEAR(tmp_ovfl);
			WT_RET(__wt_bulk_ovfl_copy(toc,
			    WT_ITEM_BYTE_OVFL(key_item), &tmp_ovfl));
			key->data = &tmp_ovfl;
			key->size = sizeof(tmp_ovfl);
			switch (hdr->type) {
			case WT_PAGE_ROW_INT:
			case WT_PAGE_ROW_LEAF:
				WT_ITEM_SET(&item,
				    WT_ITEM_KEY_OVFL, sizeof(WT_OVFL));
				break;
			case WT_PAGE_DUP_INT:
			case WT_PAGE_DUP_LEAF:
				WT_ITEM_SET(&item,
				    WT_ITEM_KEY_DUP_OVFL, sizeof(WT_OVFL));
				break;
			default:		/* Not possible */
				break;
			}
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RCC:
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
	 * page's level will be created, and it will be promoted to the parent
	 * at some point.  This is case #1.
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
	/*
	 * To simplify the rest of the code, check to see if there's room for
	 * another entry in our stack structure.  Allocate the stack in groups
	 * of 20, which is probably big enough for any tree we'll ever see in
	 * the field, we'll never test the realloc code unless we work at it.
	 */
#ifdef HAVE_DIAGNOSTIC
#define	WT_STACK_ALLOC_INCR	2
#else
#define	WT_STACK_ALLOC_INCR	20
#endif
	if (stack->size == 0 || level == stack->size - 1) {
		uint32_t
		    bytes_allocated = stack->size * sizeof(WT_STACK_ELEM);
		WT_RET(__wt_realloc(env, &bytes_allocated,
		    (stack->size + WT_STACK_ALLOC_INCR) * sizeof(WT_STACK_ELEM),
		    &stack->elem));
		stack->size += WT_STACK_ALLOC_INCR;
		/*
		 * Note, the stack structure may be entirely uninitialized here,
		 * that is, everything set to 0 bytes.  That's OK: the level of
		 * the stack starts out at 0, that is, the 0th element of the
		 * stack is the 1st level of internal/parent pages in the tree.
		 */
	}

	elem = &stack->elem[level];
	parent = elem->page;
	if (parent == NULL) {
split:		switch (hdr->type) {
		case WT_PAGE_COL_FIX:
		case WT_PAGE_COL_INT:
		case WT_PAGE_COL_RCC:
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

		WT_ERR(__wt_bulk_scratch_page(
		    toc, db->intlmin, type, hdr->level + 1, &next, &next_tmp));
		__wt_set_ff_and_sa_from_offset(next,
		    WT_PAGE_BYTE(next), &next_first_free, &next_space_avail);

		/*
		 * Column stores set the starting record number to the starting
		 * record number of the promoted leaf -- the new leaf is always
		 * the first record in the new parent's page.  Ignore the type
		 * of the database, it's simpler ot just promote 0 up the tree
		 * in row store databases.
		 */
		next->hdr->start_recno = page->hdr->start_recno;

		/*
		 * If we don't have a parent page, it's case #1 -- allocate the
		 * parent page immediately.
		 */
		if (parent == NULL) {
			/*
			 * Case #1 -- there's no parent, it's a root split.  No
			 * additional work in the main tree.  In an off-page
			 * duplicates tree, return the new root of the off-page
			 * tree.
			 */
			if (type == WT_PAGE_DUP_INT)
				*dup_root_addrp = next->addr;
			need_promotion = 0;
		} else {
			/*
			 * Case #2 and #3.
			 *
			 * Case #3: a root split, so we have to promote a key
			 * from both of the parent pages: promote the key from
			 * the existing parent page.
			 */
			if (stack->elem[level + 1].page == NULL)
				WT_ERR(__wt_bulk_promote(toc, parent,
				    incr, stack, level + 1, dup_root_addrp));
			need_promotion = 1;

			/* Write the last parent page, we have a new one. */
			WT_ERR(__wt_page_write(toc, parent));
			__wt_scr_release(&stack->elem[level].tmp);
		}

		/* There's a new parent page, reset the stack. */
		elem = &stack->elem[level];
		elem->page = parent = next;
		elem->first_free = next_first_free;
		elem->space_avail = next_space_avail;
		elem->tmp = next_tmp;
		next = NULL;
		next_first_free = NULL;
		next_space_avail = 0;
		next_tmp = NULL;
	} else
		need_promotion = 0;

	/*
	 * See if the promoted data will fit (if they don't, we have to split).
	 * We don't need to check for overflow keys: if the key was an overflow,
	 * we already created a smaller, on-page version of it.
	 *
	 * If there's room, copy the promoted data onto the parent's page.
	 */
	switch (parent->hdr->type) {
	case WT_PAGE_COL_INT:
		if (elem->space_avail < sizeof(WT_OFF))
			goto split;

		/* Create the WT_OFF reference. */
		WT_RECORDS(&off) = page->records;
		off.addr = page->addr;
		off.size = hdr->level == WT_LLEAF ? db->leafmin : db->intlmin;

		/* Store the data item. */
		++parent->hdr->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &off, sizeof(off));
		elem->first_free += sizeof(WT_OFF);
		elem->space_avail -= sizeof(WT_OFF);

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	case WT_PAGE_ROW_INT:
	case WT_PAGE_DUP_INT:
		if (elem->space_avail <
		    WT_ITEM_SPACE_REQ(sizeof(WT_OFF)) +
		    WT_ITEM_SPACE_REQ(key->size))
			goto split;

		/* Store the key. */
		++parent->hdr->u.entries;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), key->data, key->size);
		elem->first_free += WT_ITEM_SPACE_REQ(key->size);
		elem->space_avail -= WT_ITEM_SPACE_REQ(key->size);

		/* Create the WT_ITEM(WT_OFF) reference. */
		WT_ITEM_SET(&item, WT_ITEM_OFF, sizeof(WT_OFF));
		WT_RECORDS(&off) = page->records;
		off.addr = page->addr;
		off.size = hdr->level == WT_LLEAF ? db->leafmin : db->intlmin;

		/* Store the data item. */
		++parent->hdr->u.entries;
		parent_data = elem->first_free;
		memcpy(elem->first_free, &item, sizeof(item));
		memcpy(elem->first_free + sizeof(item), &off, sizeof(off));
		elem->first_free += WT_ITEM_SPACE_REQ(sizeof(WT_OFF));
		elem->space_avail -= WT_ITEM_SPACE_REQ(sizeof(WT_OFF));

		/* Track the last entry on the page for record count updates. */
		stack->elem[level].data = parent_data;
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	parent->records += page->records;

	/*
	 * The promotion for case #2 and the second part of case #3 -- promote
	 * the key from the newly allocated internal page to its parent.
	 */
	if (need_promotion)
		WT_RET(__wt_bulk_promote(
		    toc, parent, incr, stack, level + 1, dup_root_addrp));
	else {
		/*
		 * We've finished promoting the new page's key into the tree.
		 * What remains is to push the new record counts all the way
		 * to the root.  We've already corrected our current "parent"
		 * page, so proceed from there to the root.
		 */
		for (elem =
		    &stack->elem[level + 1]; elem->page != NULL; ++elem) {
			switch (elem->page->hdr->type) {
			case WT_PAGE_COL_INT:
				WT_RECORDS((WT_OFF *)elem->data) += incr;
				break;
			case WT_PAGE_ROW_INT:
			case WT_PAGE_DUP_INT:
				WT_RECORDS(
				    (WT_OFF *)WT_ITEM_BYTE(elem->data)) += incr;
				break;
			WT_ILLEGAL_FORMAT(db);
			}
			elem->page->records += incr;
		}
	}

err:	if (next_tmp != NULL)
		__wt_scr_release(&next_tmp);

	return (ret);
}

/*
 * __wt_item_build_key --
 *	Process an inserted key item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
static int
__wt_item_build_key(WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl)
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
	 *
	 * For Huffman-encoded key/data items, we need a chunk of new space;
	 * use the WT_TOC key/data return memory: this routine is called during
	 * bulk insert and reconciliation, we aren't returning key/data pairs.
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
		WT_STAT_INCR(stats, OVERFLOW_KEY);

		WT_RET(__wt_bulk_ovfl_write(toc, dbt, ovfl));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_SET(item, WT_ITEM_KEY_OVFL, dbt->size);
	} else
		WT_ITEM_SET(item, WT_ITEM_KEY, dbt->size);
	return (0);
}

/*
 * __wt_item_build_data --
 *	Process an inserted data item and return an WT_ITEM structure and byte
 *	string to be stored on the page.
 */
int
__wt_item_build_data(
    WT_TOC *toc, DBT *dbt, WT_ITEM *item, WT_OVFL *ovfl, u_int flags)
{
	DB *db;
	IDB *idb;
	WT_STATS *stats;

	 WT_ENV_FCHK_ASSERT(toc->env,
	    "__wt_item_build_data", flags, WT_APIMASK_BT_BUILD_DATA_ITEM);

	db = toc->db;
	idb = db->idb;
	stats = idb->stats;

	/*
	 * We're called with a DBT that references a data/size pair.  We can
	 * re-point that DBT's data and size fields to other memory, but we
	 * cannot allocate memory in that DBT -- all we can do is re-point it.
	 *
	 * For Huffman-encoded key/data items, we need a chunk of new space;
	 * use the WT_TOC key/data return memory: this routine is called during
	 * bulk insert and reconciliation, we aren't returning key/data pairs.
	 */
	WT_CLEAR(*item);
	WT_ITEM_SET_TYPE(
	    item, LF_ISSET(WT_IS_DUP) ? WT_ITEM_DATA_DUP : WT_ITEM_DATA);

	/*
	 * Handle zero-length items quickly -- this is a common value, it's
	 * a deleted column-store variable length item.
	 */
	if (dbt->size == 0) {
		WT_ITEM_SET_LEN(item, 0);
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
		WT_RET(__wt_bulk_ovfl_write(toc, dbt, ovfl));

		dbt->data = ovfl;
		dbt->size = sizeof(*ovfl);
		WT_ITEM_SET_TYPE(item, LF_ISSET(WT_IS_DUP) ?
		    WT_ITEM_DATA_DUP_OVFL : WT_ITEM_DATA_OVFL);
		WT_STAT_INCR(stats, OVERFLOW_DATA);
	}

	WT_ITEM_SET_LEN(item, dbt->size);
	return (0);
}

/*
 * __wt_bulk_ovfl_copy --
 *	Copy bulk-loaded overflow items in the database, returning the WT_OVFL
 *	structure, filled in.
 */
static int
__wt_bulk_ovfl_copy(WT_TOC *toc, WT_OVFL *from, WT_OVFL *to)
{
	DB *db;
	DBT *tmp;
	WT_PAGE *page;
	uint32_t size;
	int ret;

	db = toc->db;
	tmp = NULL;

	/* Get a scratch buffer and make it look like an overflow page. */
	size = WT_ALIGN(sizeof(WT_PAGE_HDR) + from->size, db->allocsize);
	WT_ERR(__wt_bulk_scratch_page(
	    toc, size, WT_PAGE_OVFL, WT_LLEAF, &page, &tmp));
	page->hdr->u.datalen = from->size;

	/* Fill in the return information. */
	to->addr = page->addr;
	to->size = from->size;

	/*
	 * Re-set the page's address and read the page into our scratch buffer.
	 * This is sleazy, but this way we can still use the underlying cache
	 * page-read functions, without allocating two chunks of memory.
	 */
	page->addr = from->addr;
	WT_ERR(__wt_page_read(db, page));

	/*
	 * Move back to the new address, and write our copy of the page to a
	 * new location.
	 */
	page->addr = to->addr;
	ret = __wt_page_write(toc, page);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_ovfl_write --
 *	Store bulk-loaded overflow items in the database, returning the page
 *	addr.
 */
static int
__wt_bulk_ovfl_write(WT_TOC *toc, DBT *dbt, WT_OVFL *to)
{
	DB *db;
	DBT *tmp;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	uint32_t size;
	int ret;

	db = toc->db;
	tmp = NULL;

	/* Get a scratch buffer and make it look like our work page. */
	size = WT_ALIGN(sizeof(WT_PAGE_HDR) + dbt->size, db->allocsize);
	WT_ERR(__wt_bulk_scratch_page(
	    toc, size, WT_PAGE_OVFL, WT_LLEAF, &page, &tmp));

	/* Fill in the return information. */
	to->addr = page->addr;
	to->size = dbt->size;

	/* Initialize the page header and copy the record into place. */
	hdr = page->hdr;
	hdr->u.datalen = dbt->size;
	memcpy((uint8_t *)hdr + sizeof(WT_PAGE_HDR), dbt->data, dbt->size);

	ret = __wt_page_write(toc, page);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}

/*
 * __wt_bulk_scratch_page --
 *	Allocate a scratch buffer and make it look like a database page.
 */
static int
__wt_bulk_scratch_page(WT_TOC *toc, uint32_t page_size,
    uint32_t page_type, uint32_t page_level, WT_PAGE **page_ret, DBT **tmp_ret)
{
	DBT *tmp;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	uint32_t size;
	int ret;

	ret = 0;

	/*
	 * Allocate a scratch buffer and make sure it's big enough to hold a
	 * WT_PAGE structure plus the page itself, and clear the memory so
	 * it's never random bytes.
	 */
	size = page_size + sizeof(WT_PAGE);
	WT_ERR(__wt_scr_alloc(toc, size, &tmp));
	memset(tmp->data, 0, size);

	/*
	 * Set up the page and allocate a file address.
	 *
	 * We don't run the leaf pages through the cache -- that means passing
	 * a lot of messages we don't want to bother with.  We're the only user
	 * of the file, which means we can grab file space whenever we want.
	 */
	page = tmp->data;
	page->hdr = hdr =
	    (WT_PAGE_HDR *)((uint8_t *)tmp->data + sizeof(WT_PAGE));
	WT_ERR(__wt_file_alloc(toc, &page->addr, page_size));
	page->size = page_size;
	hdr->type = (uint8_t)page_type;
	hdr->level = (uint8_t)page_level;

	*page_ret = page;
	*tmp_ret = tmp;
	return (0);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_bulk_stack_put --
 *	Push out the tree's stack of pages.
 */
static int
__wt_bulk_stack_put(WT_TOC *toc, WT_STACK *stack)
{
	ENV *env;
	IDB *idb;
	WT_STACK_ELEM *elem;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	ret = 0;

	for (elem = stack->elem; elem->page != NULL; ++elem) {
		WT_TRET(__wt_page_write(toc, elem->page));

		/*
		 * If we've reached the last element in the stack, it's the
		 * root page of the tree.  Update the in-memory root address
		 * and the descriptor record.
		 */
		if ((elem + 1)->page == NULL) {
			idb->root_off.addr = elem->page->addr;
			idb->root_off.size = elem->page->size;
			WT_RECORDS(&idb->root_off) = elem->page->records;
			WT_TRET(__wt_desc_write(toc));
		}

		__wt_scr_release(&elem->tmp);
	}
	__wt_free(env, stack->elem, stack->size * sizeof(WT_STACK_ELEM));

	return (0);
}

/*
 * __wt_bulk_dbt_copy --
 *	Get a copy of DBT referenced object.
 */
static int
__wt_bulk_dbt_copy(ENV *env, DBT *orig, DBT *copy)
{
	if (copy->mem_size < orig->size)
		WT_RET(__wt_realloc(
		    env, &copy->mem_size, orig->size, &copy->data));
	memcpy(copy->data, orig->data, orig->size);
	copy->size = orig->size;

	return (0);
}
