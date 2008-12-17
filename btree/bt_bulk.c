/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_dup_offpage(DB *, DBT **, DBT **, DBT *, WT_ITEM *,
	u_int8_t *, u_int32_t, u_int32_t, int (*)(DB *, DBT **, DBT **));
static int __wt_ovfl_copy(IENV *, DBT *, DBT *);

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
	WT_ITEM key_item, data_item, *dup_key, *dup_data;
	WT_ITEM_OVFL key_ovfl, data_ovfl;
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
			    dup_data->type =
				dup_data->type == WT_ITEM_DATA ?
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
				if ((ret = __wt_ovfl_copy(
				    ienv, key, lastkey)) != 0)
					goto err;
			}

			key_ovfl.len = key->size;
			if ((ret = __wt_db_ovfl_write(
			    db, key, &key_ovfl.addr)) != 0)
				goto err;
			key->data = &key_ovfl;
			key->size = sizeof(key_ovfl);

			key_item.type = WT_ITEM_KEY_OVFL;
		}
		if (data->size > db->maxitemsize) {
			data_ovfl.len = data->size;
			if ((ret = __wt_db_ovfl_write(
			    db, data, &data_ovfl.addr)) != 0)
				goto err;
			data->data = &data_ovfl;
			data->size = sizeof(data_ovfl);
			data_item.type =
			    data_item.type == WT_ITEM_DATA ?
			    WT_ITEM_DATA_OVFL : WT_ITEM_DUP_OVFL;
		}

		/* 
		 * If there's insufficient space available, allocate space
		 * from the backing file and connect it to the in-memory tree.
		 */
		if (hdr == NULL ||
		    (key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail) {
			/* Allocate a new page. */
			if ((ret = __wt_db_falloc(bt,
			    WT_FRAGS_PER_PAGE(db), &next_hdr, &next_addr)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_LEAF;
			next_hdr->prevaddr = addr;
			next_hdr->nextaddr =
			    next_hdr->prntaddr = WT_ADDR_INVALID;

			/*
			 * If loading a set of duplicates, but the set hasn't
			 * yet reached the boundary where we push them offpage,
			 * we can't split them across the two pages.  Move the
			 * set to the new page.  This can waste up to 25% of
			 * the old page, but it would be difficult and messy to
			 * fix.
			 */
			if (dup_count > 1) {
				/*
				 * Set the entries -- we're moving a key plus
				 * the duplicates.
				 */
				hdr->u.entries -= (dup_count + 1);
				next_hdr->u.entries = dup_count + 1;

				/* Move the bytes. */
				len = p - (u_int8_t *)dup_key;
				memcpy(WT_PAGE_BYTE(next_hdr), dup_key, len);

				/*
				 * Set the next available page byte, and the
				 * space available.
				 */
				p = WT_PAGE_BYTE(next_hdr) + len;
				space_avail = WT_DATA_SPACE(db->pagesize) - len;

				/*
				 * We won't have to move this dup set to
				 * another primary page -- if the dup set
				 * continues to grow, it will be moved
				 * off-page.  We still need to know where
				 * the dup set starts, though, for the
				 * possible move off-page: it's the second
				 * entry on the page, where the first entry
				 * is the dup set's key.
				 */
				dup_key = (WT_ITEM *)WT_PAGE_BYTE(next_hdr);
				dup_data = (WT_ITEM *)
				    ((u_int8_t *)dup_key +
				    WT_ITEM_SPACE_REQ(dup_key->len));
			} else {
				p = WT_PAGE_BYTE(next_hdr);
				space_avail = WT_DATA_SPACE(db->pagesize);
			}

			/* Write any filled page. */
			if (hdr != NULL) {
				hdr->nextaddr = next_addr;
				if ((ret = __wt_db_fwrite(bt,
				    addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0)
					goto err;
			}

			hdr = next_hdr;
			addr = next_addr;
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
			 *
			 * We also save the location for the key of any
			 * current duplicate set in case we have to move
			 * the set to a different page (the case where the 
			 * duplicate set isn't large enough to move offpag,
			 * but doesn't entirely fit on this page).
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				if (key_item.type != WT_ITEM_KEY_OVFL) {
					lastkey = &lastkey_std;
					lastkey_std.data = WT_ITEM_BYTE(p);
					lastkey_std.size = key->size;
				}
				dup_key = (WT_ITEM *)p;
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
			dup_data = (WT_ITEM *)p;
		}

		p += WT_ITEM_SPACE_REQ(data->size);

		/*
		 * If duplicates: check to see if the duplicate set crosses
		 * the (roughly) 25% of the page space boundary.  If it does,
		 * move it offpage.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    data_item.type == WT_ITEM_DUP ||
		    data_item.type == WT_ITEM_DUP_OVFL) {
			++dup_count;
			dup_space += data->size;

			if (dup_space < db->pagesize / 4)
				continue;

			/*
			 * Move the duplicate set offpage and read in the
			 * rest of the duplicate set.
			 */
			if ((ret = __wt_dup_offpage(db,
			    &key, &data, lastkey,
			    dup_data, p, dup_count, flags, cb)) != 0)
				goto err;

			/*
			 * Reset the local page information; the duplicate set
			 * has been replaced by a single WT_ITEM_OFFP structure,
			 * that is, we've moved dup_count - 1 entries.
			 */
			hdr->u.entries -= (dup_count - 1);

			/* Reset the next byte available and total space. */
			p = (u_int8_t *)dup_data;
			p += WT_ITEM_SPACE_REQ(dup_data->len);
			space_avail = db->pagesize -
			    ((u_int8_t *)p - (u_int8_t *)hdr);

			/* Restart our dup counters. */
			dup_count = dup_space = 0;

			goto skip_read;
		}
	}

	/* Write any partially-filled page. */
	if (ret == 1 && hdr != NULL)
		ret = __wt_db_fwrite(bt, addr, WT_FRAGS_PER_PAGE(db), hdr);

	if (0) {
err:		ret = WT_ERROR;
	}
	if (lastkey_ovfl.data != NULL)
		__wt_free(ienv, lastkey_ovfl.data);

	return (ret);
}

/*
 * __wt_dup_offpage --
 *	Move the last set of duplicates on the page to a page of their own,
 *	then load the rest of the duplicate set.
 */
static int
__wt_dup_offpage(DB *db,
    DBT **keyp, DBT **datap,
    DBT *lastkey, WT_ITEM *dup_first, u_int8_t *last_byte_after,
    u_int32_t dup_count, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM data_item;
	WT_ITEM_OFFP offpage_item;
	WT_ITEM_OVFL data_local;
	WT_PAGE_HDR *hdr, *next_hdr;
	u_int32_t addr, next_addr, root_addr, space_avail;
	u_int8_t *p;
	int ret, tret;

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
	 * key/data --
	 *	The key and data pairs the application is filling in -- we
	 *	get them passed to us because we get additional key/data
	 *	pairs returned to us, and the last one we get is likely to
	 *	be consumed by our caller.
	 * dup_first --
	 *	On-page reference to the first duplicate data item in the set.
	 * last_byte_after --
	 *	On-page reference to the first byte after the last duplicate
	 *	data item in the set.
	 * dup_count --
	 *	Count of duplicates in the set.
	 * flags --
	 *	User's bulk-load method flags.
	 * cb --
	 *	User's callback function.
	 */
	ienv = db->ienv;
	bt = db->idb->btree;
	ret = 0;

	WT_CLEAR(data_item);

	/* Allocate and initialize a new page. */
	if ((ret =
	    __wt_db_falloc(bt, WT_FRAGS_PER_PAGE(db), &hdr, &root_addr)) != 0)
		return (ret);
	hdr->type = WT_PAGE_DUP_ROOT;
	hdr->u.entries = dup_count;

	/* Copy the duplicate data set into place. */
	memcpy(WT_PAGE_BYTE(hdr),
	    dup_first, last_byte_after - (u_int8_t *)dup_first);

	/* Set up our local page information. */
	p = WT_PAGE_BYTE(hdr) + (last_byte_after - (u_int8_t *)dup_first);
	space_avail = db->pagesize - ((u_int8_t *)p - (u_int8_t *)hdr);

	/* Read in new duplicate records until the key changes. */
	for (addr = root_addr; (ret = cb(db, &key, &data)) == 0;) {
		if (key->size == 0) {
			__wt_db_errx(db, "zero-length keys are not supported");
			goto err;
		}

		/* Loading duplicates, so a key change means we're done. */
		if (lastkey->size != key->size ||
		    db->dup_compare(db, lastkey, key) != 0) {
			*keyp = key;
			*datap = data;

			/* Write any partially-filled page. */
			if ((tret = __wt_db_fwrite(
			    bt, addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0)
				ret = tret;
			break;
		}

		/* Create overflow objects if the data won't fit. */
		if (data->size > db->maxitemsize) {
			data_local.len = data->size;
			if ((ret = __wt_db_ovfl_write(
			    db, data, &data_local.addr)) != 0)
				goto err;
			data->data = &data_local;
			data->size = sizeof(data_local);
			data_item.type = WT_ITEM_DUP_OVFL;
		} else
			data_item.type = WT_ITEM_DUP;

		/* 
		 * If there's insufficient space available, allocate a space
		 * from the backing file and connect it to the in-memory tree.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > space_avail) {
			/* Allocate a new page. */
			if ((ret = __wt_db_falloc(bt,
			    WT_FRAGS_PER_PAGE(db), &next_hdr, &next_addr)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_DUP_LEAF;
			next_hdr->prevaddr = addr;
			next_hdr->nextaddr =
			    next_hdr->prntaddr = WT_ADDR_INVALID;

			/* Write the last filled page. */
			hdr->nextaddr = next_addr;
			if ((ret = __wt_db_fwrite(bt,
			    addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0)
				goto err;

			space_avail = WT_DATA_SPACE(db->pagesize);
			hdr = next_hdr;
			addr = next_addr;
			p = WT_PAGE_BYTE(hdr);
		}

		++dup_count;
		++hdr->u.entries;

		/* Copy the data item onto the page. */
		space_avail -= WT_ITEM_SPACE_REQ(data->size);
		data_item.len = data->size;
		memcpy(p, &data_item, sizeof(data_item));
		memcpy(p + sizeof(data_item), data->data, data->size);
		p += WT_ITEM_SPACE_REQ(data->size);
	}

	/* Write the caller's on-page object. */
	data_item.len = sizeof(WT_ITEM_OFFP);
	data_item.type = WT_ITEM_DUP_OFFPAGE;
	offpage_item.addr = root_addr;
	offpage_item.records = dup_count;
	p = (u_int8_t *)dup_first;
	memcpy(p, &data_item, sizeof(data_item));
	memcpy(p + sizeof(data_item), &offpage_item, sizeof(offpage_item));

	if (0) {
err:		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __wt_ovfl_copy --
 *	Get a local copy of an overflow key.
 */
static int
__wt_ovfl_copy(IENV *ienv, DBT *orig, DBT *copy)
{
	int ret;

	if (copy->data == NULL || copy->alloc_size < orig->size) {
		if ((ret = __wt_realloc(ienv, orig->size, &copy->data)) != 0)
			return (ret);
		copy->alloc_size = orig->size;
	}
	memcpy(copy->data, orig->data, copy->size = orig->size);

	return (0);
}
