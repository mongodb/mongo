/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_dup_move(DB *, WT_ITEM *, u_int8_t *, u_int32_t);
static int __wt_ovfl_key_copy(IENV *, DBT *, DBT *);

/*
 * The bulk load loop handles multiple pages at a time when loading sets
 * of duplicates, that is, while loading a leaf page it may have to go
 * off and load a big set of duplicate data items, returning to finish
 * the leaf page.   The BP (bulk-page) structure is the information we
 * hold about a page we're loading.
 */
typedef struct __bp {
	int foo;
} BP;

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(DB *db, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data, *lastkey, lastkey_std, lastkey_ovfl;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM key_item, data_item, *dup_first;
	WT_ITEM_OVFL key_local, data_local;
	WT_PAGE_HDR *hdr, *next_hdr;
	u_int32_t addr, dup_count, dup_space, len, next_addr, space_avail;
	u_int8_t *p;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;

	addr = WT_ADDR_INVALID;
	hdr = NULL;
	dup_space = dup_count = 0;

	lastkey = &lastkey_std;
	WT_CLEAR(lastkey_std);
	WT_CLEAR(lastkey_ovfl);
	WT_CLEAR(key_item);
	WT_CLEAR(data_item);

	DB_FLAG_CHK(db, "Db.bulk_load", flags, WT_APIMASK_DB_BULK_LOAD);
	WT_ASSERT(ienv, LF_ISSET(WT_SORTED_INPUT));

	while ((ret = cb(db, &key, &data)) == 0) {
skip_read:	if (key->size == 0) {
			__wt_db_errx(db, "zero-length keys are not supported");
			goto err;
		}

		/*
		 * Check for duplicate data; we don't store the key on the page
		 * in the case of a duplicate.   The check is if "key" is NULL,
		 * it means we aren't going to store this key.
		 *
		 * !!!
		 * The check of lastkey->size is safe -- it's initialized to 0,
		 * and we don't allow keys of zero-length.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    lastkey->size == key->size &&
		    db->btree_compare(db, lastkey, key) == 0) {
			key = NULL;
			data_item.type = WT_ITEM_DUP;

			/*
			 * If this is the duplicate, then the first duplicate
			 * in the set has already been written to the page,
			 * but with type set to WT_ITEM_DATA or _DATA_OVFL.
			 * Fix that.
			 */
			if (dup_count == 1)
			    dup_first->type =
				dup_first->type == WT_ITEM_DATA ?
			        WT_ITEM_DUP : WT_ITEM_DUP_OVFL;
		} else {
			key_item.type = WT_ITEM_KEY;
			data_item.type = WT_ITEM_DATA;
		}

		/* Create overflow objects if the key or data won't fit. */
		if (key != NULL && key->size > db->maxitemsize) {
			/*
			 * If duplicates: we'll need a copy of the key for
			 * comparison with the next key.  It's an overflow
			 * object, so we can't just use the on-page memory.
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				lastkey = &lastkey_ovfl;
				if ((ret = __wt_ovfl_key_copy(
				    ienv, key, lastkey)) != 0)
					goto err;
			}

			key_local.len = key->size;
			if ((ret =
			    __wt_bt_ovfl_load(db, key, &key_local.addr)) != 0)
				goto err;
			key->data = &key_local;
			key->size = sizeof(key_local);

			key_item.type = WT_ITEM_KEY_OVFL;
		}
		if (data->size > db->maxitemsize) {
			data_local.len = data->size;
			if ((ret =
			    __wt_bt_ovfl_load(db, data, &data_local.addr)) != 0)
				goto err;
			data->data = &data_local;
			data->size = sizeof(data_local);
			data_item.type =
			    data_item.type == WT_ITEM_DATA ?
			    WT_ITEM_DATA_OVFL : WT_ITEM_DUP_OVFL;
		}

		/* 
		 * If there's insufficient space available, allocate a space
		 * from the backing file and connect it to the in-memory tree.
		 */
		if ((hdr == NULL ||
		    (key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail)) {
			/* Allocate a new page. */
			if ((ret = __wt_bt_falloc(bt,
			    WT_FRAGS_PER_PAGE(db), &next_hdr, &next_addr)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_LEAF;
			next_hdr->prevaddr = addr;
			next_hdr->nextaddr =
			    next_hdr->paraddr = WT_ADDR_INVALID;

			/* Write any filled page. */
			if (hdr != NULL) {
				hdr->nextaddr = next_addr;
				if ((ret = __wt_bt_fwrite(bt,
				    addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0)
					goto err;
			}

			space_avail = WT_DATA_SPACE(db->pagesize);
			hdr = next_hdr;
			addr = next_addr;
			p = WT_PAGE_DATA(hdr);
		}

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++hdr->u.entries;
			key_item.len = key->size;
			space_avail -= WT_ITEM_SPACE_REQ(key->size);
			memcpy(p, &key_item, sizeof(key_item));
			memcpy(p + sizeof(key_item), key->data, key->size);

			/*
			 * If duplicates: we'll need a copy of the key for
			 * comparison with the next key.  Not an overflow
			 * object, so we can just use the on-page memory.
			 */
			if (LF_ISSET(WT_DUPLICATES) &&
			    key_item.type != WT_ITEM_KEY_OVFL) {
				lastkey = &lastkey_std;
				lastkey_std.data = WT_ITEM_BYTE(p);
				lastkey_std.size = key->size;
			}
			p += WT_ITEM_SPACE_REQ(key->size);
		}

		/* Copy the data item onto the page. */
		++hdr->u.entries;
		space_avail -= WT_ITEM_SPACE_REQ(data->size);
		data_item.len = data->size;
		memcpy(p, &data_item, sizeof(data_item));
		memcpy(p + sizeof(data_item), data->data, data->size);

		/*
		 * If duplicates: if this isn't a duplicate data item, save
		 * the item location, since it's potentially the first of a
		 * duplicate data set, and we need to know where duplicate
		 * data sets start.  Additionally, reset the counter and
		 * space calculation.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    data_item.type != WT_ITEM_DUP &&
		    data_item.type != WT_ITEM_DUP_OVFL) {
			dup_count = 1;
			dup_space = data->size;
			dup_first = (WT_ITEM *)p;
		}

		p += WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: check to see if the duplicate set crosses
		 * the (roughly) 25% of the page space boundary.  If it does,
		 * move it off-page.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    data_item.type == WT_ITEM_DUP ||
		    data_item.type == WT_ITEM_DUP_OVFL) {
			++dup_count;
			dup_space += data->size;

			if (dup_space < db->pagesize / 4)
				continue;

			if ((ret =
			    __wt_dup_move(db, dup_first, p, dup_count)) != 0)
				goto err;

			dup_count = dup_space = 0;
			goto skip_read;
		}
	}

	/* Write any partially-filled page. */
	if (ret == 1 && hdr != NULL)
		ret = __wt_bt_fwrite(bt, addr, WT_FRAGS_PER_PAGE(db), hdr);

	if (0) {
err:		ret = WT_ERROR;
	}
	if (lastkey_ovfl.data != NULL)
		__wt_free(ienv, lastkey_ovfl.data);

	return (ret);
}

/*
 * __wt_dup_move --
 *	Move the last set of duplicates on the page to a page of their own.
 */
static int
__wt_dup_move(DB *db,
    WT_ITEM *dup_first, u_int8_t *last_byte_after, u_int32_t dup_count)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	u_int32_t addr;
	int ret;

	bt = db->idb->btree;

	/* Allocate and initialize a new page. */
	if ((ret = __wt_bt_falloc(bt, WT_FRAGS_PER_PAGE(db), &hdr, &addr)) != 0)
		return (ret);
	hdr->type = WT_PAGE_DUP_ROOT;
	hdr->u.entries = dup_count;

	/* Copy the duplicate data set into place. */
	memcpy(WT_PAGE_DATA(hdr),
	    dup_first, last_byte_after - (u_int8_t *)dup_first);

	return (0);
}

/*
 * __wt_ovfl_key_copy --
 *	Get a local copy of an overflow key.
 */
static int
__wt_ovfl_key_copy(IENV *ienv, DBT *key, DBT *copy)
{
	int ret;

	if (copy->data == NULL || copy->alloc_size < key->size) {
		if ((ret = __wt_realloc(ienv, key->size, &copy->data)) != 0)
			return (ret);
		copy->alloc_size = key->size;
	}
	memcpy(copy->data, key->data, copy->size = key->size);

	return (0);
}
