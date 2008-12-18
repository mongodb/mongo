/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_promote(DB *, WT_PAGE *, WT_PAGE_HDR *, u_int32_t, DBT *);
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
	enum { FIRST_PAGE, SECOND_PAGE, SUBSEQUENT_PAGES } bulk_state;
	DBT *key, *data, *lastkey, lastkey_std, lastkey_ovfl, tmpkey;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM *item, key_item, data_item, *dup_key, *dup_data;
	WT_ITEM_OVFL key_ovfl, data_ovfl, tmp_ovfl;
	WT_PAGE parent;
	WT_PAGE_HDR *hdr, *next_hdr;
	u_int32_t addr, dup_count, dup_space, len, next_addr, space_avail;
	u_int8_t *p;
	int ret, tret;

	ienv = db->ienv;
	bt = db->idb->btree;

	bulk_state = FIRST_PAGE;
	addr = WT_ADDR_INVALID;
	dup_space = dup_count = 0;

	lastkey = &lastkey_std;
	WT_CLEAR(lastkey_std);
	WT_CLEAR(lastkey_ovfl);
	WT_CLEAR(key_item);
	WT_CLEAR(data_item);
	WT_CLEAR(parent);

	DB_FLAG_CHK(db, "Db.bulk_load", flags, WT_APIMASK_DB_BULK_LOAD);
	WT_ASSERT(ienv, LF_ISSET(WT_SORTED_INPUT));

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key->size == 0) {
			__wt_db_errx(db, "zero-length keys are not supported");
			goto err;
		}
		/*
		 * We pushed a set of duplicates off-page, and that routine
		 * returned a ending key/data pair to us.
		 */
skip_read:
		/*
		 * Check for duplicate data; we don't store the key on the page
		 * in the case of a duplicate.   The check through the rest of
		 * this code is if "key" is NULL, it means we aren't going to
		 * store anything for this key.
		 *
		 * !!!
		 * Do a fast check of the old and new sizes -- note checking
		 * lastkey->size is safe -- it's initialized to 0, and we do
		 * not allow zero-length keys.
		 */
		if (LF_ISSET(WT_DUPLICATES) &&
		    lastkey->size == key->size &&
		    db->btree_compare(db, lastkey, key) == 0) {
			/*
			 * The first duplicate in the set is already on the
			 * page, but with an item type set to WT_ITEM_DATA or
			 * WT_ITEM_DATA_OVFL.  Correct the type.
			 */
			if (dup_count == 1)
				dup_data->type =
				    dup_data->type == WT_ITEM_DATA ?
			            WT_ITEM_DUP : WT_ITEM_DUP_OVFL;

			key = NULL;
		}

		/* Create overflow objects if the key or data won't fit. */
		if (key != NULL && key->size > db->maxitemsize) {
			/*
			 * If duplicates: we'll need a copy of the key for
			 * comparison with the next key.  It's an overflow
			 * object, so we can't just use the on-page memory.
			 * Save a copy.
			 */
			if (LF_ISSET(WT_DUPLICATES)) {
				lastkey = &lastkey_ovfl;
				if ((ret =
				    __wt_ovfl_copy(ienv, key, lastkey)) != 0)
					goto err;
			}

			key_ovfl.len = key->size;
			if ((ret =
			    __wt_db_ovfl_write(db, key, &key_ovfl.addr)) != 0)
				goto err;
			key->data = &key_ovfl;
			key->size = sizeof(key_ovfl);

			key_item.type = WT_ITEM_KEY_OVFL;
		} else
			key_item.type = WT_ITEM_KEY;

		if (data->size > db->maxitemsize) {
			data_ovfl.len = data->size;
			if ((ret = __wt_db_ovfl_write(
			    db, data, &data_ovfl.addr)) != 0)
				goto err;
			data->data = &data_ovfl;
			data->size = sizeof(data_ovfl);

			data_item.type =
			    key == NULL ? WT_ITEM_DUP_OVFL : WT_ITEM_DATA_OVFL;
		} else
			data_item.type =
			    key == NULL ? WT_ITEM_DUP : WT_ITEM_DATA;

		/* 
		 * We now know what the key/data items will look like on a
		 * page.  If we haven't yet allocated a page, or there is
		 * insufficient space on the current page, allocate a new page.
		 */
		if (bulk_state == FIRST_PAGE ||
		    (key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail) {
			if ((ret = __wt_db_falloc(bt,
			    WT_FRAGS_PER_PAGE(db), &next_hdr, &next_addr)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_LEAF;
			next_hdr->prevaddr = addr;
			next_hdr->nextaddr =
			    next_hdr->prntaddr = WT_ADDR_INVALID;

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
				hdr->u.entries -= (dup_count + 1);
				next_hdr->u.entries += (dup_count + 1);

				/* Move the duplicate set. */
				len = p - (u_int8_t *)dup_key;
				memcpy(WT_PAGE_BYTE(next_hdr), dup_key, len);

				/*
				 * Reset the next available page byte, and the
				 * space available.
				 */
				p = WT_PAGE_BYTE(next_hdr) + len;
				space_avail = WT_DATA_SPACE(db->pagesize) - len;

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
				dup_key = (WT_ITEM *)WT_PAGE_BYTE(next_hdr);
				dup_data = (WT_ITEM *)
				    ((u_int8_t *)dup_key +
				    WT_ITEM_SPACE_REQ(dup_key->len));

				/*
				 * The "lastkey" value just moved to a new page.
				 * If it's an overflow item, we have a copy; if
				 * it's not, then we need to reset it.
				 */
				if (lastkey == &lastkey_std) {
					lastkey_std.data =
					    WT_ITEM_BYTE(dup_key);
					lastkey_std.size = dup_key->len;
				}
			} else {
				p = WT_PAGE_BYTE(next_hdr);
				space_avail = WT_DATA_SPACE(db->pagesize);
			}

			/*
			 * If this is the first page, there's nothing to do.
			 * If this is the second page, create an internal page
			 * and promote a key from the first page plus the
			 * current key.  If this is a subsequent page, promote
			 * the current key.
			 *
			 * If this is the second or subsequent page, write the
			 * previous page.
			 */
			switch (bulk_state) {
			case FIRST_PAGE:
				bulk_state = SECOND_PAGE;
				break;
			case SECOND_PAGE:
				/*
				 * We have to get a copy of the first key on
				 * the page.   It might be an overflow item,
				 * of course.
				 */
				WT_CLEAR(tmpkey);
				item = (WT_ITEM *)WT_PAGE_BYTE(hdr);
				switch (item->type) {
				case WT_ITEM_KEY:
					tmpkey.data = WT_ITEM_BYTE(item);
					tmpkey.size = item->len;
					break;
				case WT_ITEM_KEY_OVFL:
					if ((ret = __wt_db_ovfl_copy(db, 
					    (WT_ITEM_OVFL *)WT_ITEM_BYTE(item),
					    &tmp_ovfl)) != 0)
						goto err;
					tmpkey.data = &tmp_ovfl;
					tmpkey.size = sizeof(tmp_ovfl);
					break;
				default:
					(void)__wt_database_format(db);
					goto err;
				}
				if ((ret = __wt_db_promote(db, &parent,
				    hdr, addr, &tmpkey)) != 0)
					goto err;
				bulk_state = SUBSEQUENT_PAGES;
				/* FALLTHROUGH */
			case SUBSEQUENT_PAGES:
				if ((ret = __wt_db_promote(db, &parent,
				    next_hdr, next_addr, lastkey)) != 0)
					goto err;

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

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret == 1)
		ret = 0;

	/* Write any partially-filled page and parent. */
	if (hdr != NULL && (tret = __wt_db_fwrite(
	    bt, addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0)
		ret = tret;
	if (parent.hdr != NULL && (tret = __wt_db_fwrite(
	    bt, parent.addr, WT_FRAGS_PER_PAGE(db), parent.hdr)) != 0)
		ret = tret;

	if (0) {
err:		ret = WT_ERROR;
	}
	if (lastkey_ovfl.data != NULL)
		__wt_free(ienv, lastkey_ovfl.data);

	return (ret == 1 ? 0 : ret);
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
	enum { SECOND_PAGE, SUBSEQUENT_PAGES } bulk_state;
	DBT *key, *data, tmpkey;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM data_item, *item;
	WT_ITEM_OFFP offpage_item;
	WT_ITEM_OVFL data_local, tmp_ovfl;
	WT_PAGE parent;
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
	 * keyp/datap --
	 *	The key and data pairs the application is filling in -- we
	 *	get them passed to us because we get additional key/data
	 *	pairs returned to us, and the last one we get is likely to
	 *	be consumed by our caller.
	 * lastkey --
	 *	The last key pushed onto the caller's page -- we use this to
	 *	compare against future keys we read.
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

	bulk_state = SECOND_PAGE;
	ret = 0;

	WT_CLEAR(data_item);
	WT_CLEAR(parent);

	/* Allocate and initialize a new page. */
	if ((ret =
	    __wt_db_falloc(bt, WT_FRAGS_PER_PAGE(db), &hdr, &root_addr)) != 0)
		return (ret);
	hdr->type = WT_PAGE_DUP_LEAF;
	hdr->u.entries = dup_count;

	/* Copy the duplicate data set into place. */
	memcpy(WT_PAGE_BYTE(hdr),
	    dup_first, last_byte_after - (u_int8_t *)dup_first);

	/* Promote the key to our parent. */
	if ((ret = __wt_db_promote(db, &parent, hdr, root_addr, lastkey)) != 0)
		goto err;

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

			/*
			 * Because we start by allocating a new page and moving
			 * the duplicate set onto it, we only have two states
			 * here -- when we allocate the second page, and when
			 * we allocate subsequent pages.  If this is the second
			 * page, create an internal page and promote a data
			 * item from the first page plus the current data item.
			 * If this is a subsequent page, promote the current
			 * data item.
			 */
			switch (bulk_state) {
			case SECOND_PAGE:
				/*
				 * We have to get a copy of the first data item
				 * on the page.   It might be an overflow item,
				 * of course.
				 */
				WT_CLEAR(tmpkey);
				item = (WT_ITEM *)WT_PAGE_BYTE(hdr);
				switch (item->type) {
				case WT_ITEM_DUP:
					tmpkey.data = WT_ITEM_BYTE(item);
					tmpkey.size = item->len;
					break;
				case WT_ITEM_DUP_OVFL:
					if ((ret = __wt_db_ovfl_copy(db, 
					    (WT_ITEM_OVFL *)WT_ITEM_BYTE(item),
					    &tmp_ovfl)) != 0)
						goto err;
					tmpkey.data = &tmp_ovfl;
					tmpkey.size = sizeof(tmp_ovfl);
					break;
				default:
					(void)__wt_database_format(db);
					goto err;
				}
				if ((ret = __wt_db_promote(db, &parent,
				    hdr, addr, &tmpkey)) != 0)
					goto err;
				bulk_state = SUBSEQUENT_PAGES;
				/* FALLTHROUGH */
			case SUBSEQUENT_PAGES:
				if ((ret = __wt_db_promote(db, &parent,
				    next_hdr, next_addr, data)) != 0)
					goto err;
				break;
			}

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

	/*
	 * Write last page and parent -- ret values of 1 and 0 are both "OK",
	 * the ret value of 1 just means we reached the end of the bulk input.
	 */
	if ((tret = __wt_db_fwrite(
	    bt, addr, WT_FRAGS_PER_PAGE(db), hdr)) != 0 && ret == 1 || ret == 0)
		ret = tret;
	if ((tret = __wt_db_fwrite(bt, parent.addr,
	    WT_FRAGS_PER_PAGE(db), parent.hdr)) != 0 && ret == 1 || ret == 0)
		ret = tret;

	/* Write the caller's on-page object. */
	data_item.len = sizeof(WT_ITEM_OFFP);
	data_item.type = WT_ITEM_OFFPAGE;
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
 * __wt_db_promote --
 *	Promote the first item on a page to the parent.
 */
static int
__wt_db_promote(DB *db,
    WT_PAGE *parent, WT_PAGE_HDR *hdr, u_int32_t addr, DBT *key)
{
	WT_BTREE *bt;
	WT_ITEM item;
	WT_ITEM_OFFP offp;
	WT_ITEM_OVFL key_ovfl;
	WT_PAGE palloc;
	int ret;

	bt = db->idb->btree;

	WT_CLEAR(item);
	WT_CLEAR(key_ovfl);

	/*
	 * If we don't already have a parent page, or if a new item pair won't
	 * fit on the current page, allocate a new one.
	 */
	if (parent->hdr == NULL) {
split:		WT_CLEAR(palloc);
		if ((ret = __wt_db_falloc(bt,
		    WT_FRAGS_PER_PAGE(db), &palloc.hdr, &palloc.addr)) != 0)
			return (WT_ERROR);
		palloc.hdr->type = WT_PAGE_INT;
		palloc.hdr->prevaddr = parent->addr;
		palloc.hdr->nextaddr = palloc.hdr->prntaddr = WT_ADDR_INVALID;
		palloc.space_avail = WT_DATA_SPACE(db->pagesize);
		palloc.p = WT_PAGE_BYTE(palloc.hdr);

		/* Write any previous parent page. */
		if (parent->hdr != NULL) {
			parent->hdr->nextaddr = palloc.addr;
			if ((ret = __wt_db_fwrite(bt, parent->addr,
			    WT_FRAGS_PER_PAGE(db), parent->hdr)) != 0)
				return (WT_ERROR);
		}

		/* Swap pages. */
		*parent = palloc;
	}

	/* See if both chunks will fit.  If they don't, we have to split. */
	if (parent->space_avail <
	    WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP)) +
	    WT_ITEM_SPACE_REQ(
		key->size > db->maxitemsize ? sizeof(key_ovfl) : key->size))
		goto split;

	/*
	 * Create the internal page DBT item, using an overflow object if the
	 * DBT won't fit.
	 */
	if (key->size > db->maxitemsize) {
		key_ovfl.len = key->size;
		if ((ret = __wt_db_ovfl_write(db, key, &key_ovfl.addr)) != 0)
			return (WT_ERROR);
		key->data = &key_ovfl;
		key->size = sizeof(key_ovfl);
		item.type = WT_ITEM_KEY_OVFL;
	} else
		item.type = WT_ITEM_KEY;
	item.len = key->size;

	/* Store the key. */
	++parent->hdr->u.entries;
	memcpy(parent->p, &item, sizeof(item));
	memcpy(parent->p + sizeof(item), key->data, key->size);
	parent->p += WT_ITEM_SPACE_REQ(key->size);
	parent->space_avail -= WT_ITEM_SPACE_REQ(key->size);

	/* Create the internal page reference. */
	item.type = WT_ITEM_OFFPAGE;
	item.len = sizeof(WT_ITEM_OFFP);
	offp.addr = addr;
	offp.records = 0;

	/* Store the internal page reference. */
	++parent->hdr->u.entries;
	memcpy(parent->p, &item, sizeof(item));
	memcpy(parent->p + sizeof(item), &offp, sizeof(offp));
	parent->p += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP));
	parent->space_avail -= WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP));

	/*
	 * Update our caller -- may already be set, but re-setting isn't
	 * a problem.
	 */
	hdr->prntaddr = parent->addr;

	return (0);
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
