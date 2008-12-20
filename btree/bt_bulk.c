/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_promote(DB *, WT_PAGE **, WT_PAGE *, DBT *);
static int __wt_dup_offpage(DB *, WT_PAGE *, DBT **, DBT **,
    DBT *, WT_ITEM *, u_int32_t, int (*cb)(DB *, DBT **, DBT **));
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
	WT_ITEM *item, key_item, data_item, *dup_key, *dup_data;
	WT_ITEM_OVFL key_ovfl, data_ovfl, tmp_ovfl;
	WT_PAGE *page, *parent, *next;
	u_int32_t dup_count, dup_space, len;
	int ret, tret;

	ienv = db->ienv;

	DB_FLAG_CHK(db, "Db.bulk_load", flags, WT_APIMASK_DB_BULK_LOAD);
	WT_ASSERT(ienv, LF_ISSET(WT_SORTED_INPUT));

	bulk_state = FIRST_PAGE;
	page = parent = NULL;
	dup_space = dup_count = 0;

	/*lint -esym(530,dup_key) */
	/*lint -esym(530,dup_data)
	 *
	 * LINT complains dup_key and dup_data may be used before being set --
	 * that's not true, we set/check dup_count before accessing dup_key
	 * or dup_data.
	 */
	/*lint -esym(613,page)
	 * LINT complains page may be used before being set -- that's not
	 * correct, bulk_state is set to FIRST_PAGE until page is initialized.
	 */

	lastkey = &lastkey_std;
	WT_CLEAR(lastkey_std);
	WT_CLEAR(lastkey_ovfl);
	WT_CLEAR(key_item);
	WT_CLEAR(data_item);

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
		    WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			if ((ret = __wt_db_falloc(
			    db, WT_FRAGS_PER_PAGE(db), &next)) != 0)
				goto err;
			next->hdr->type = WT_PAGE_LEAF;
			next->hdr->nextaddr =
			    next->hdr->prntaddr = WT_ADDR_INVALID;
			if (page == NULL)
				next->hdr->prevaddr = WT_ADDR_INVALID;
			else {
				next->hdr->prevaddr = page->addr;
				page->hdr->nextaddr = next->addr;
			}

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
				dup_key = (WT_ITEM *)next->first_data;
				dup_data =
				    (WT_ITEM *)(next->first_data +
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
			}

			/*
			 * If this is the first page, there's nothing to do,
			 * the only page in the database is a leaf page.
			 *
			 * If this is the second page, create an internal page
			 * and promote a key from the first page plus the
			 * current key.
			 *
			 * If this is a subsequent page, promote the current
			 * key.
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
				item = (WT_ITEM *)page->first_data;
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
				if ((ret = __wt_db_promote(
				    db, &parent, page, &tmpkey)) != 0)
					goto err;
				bulk_state = SUBSEQUENT_PAGES;
				/* FALLTHROUGH */
			case SUBSEQUENT_PAGES:
				if ((ret = __wt_db_promote(
				    db, &parent, next, key)) != 0)
					goto err;

				if ((ret = __wt_db_fwrite(db, page)) != 0)
					goto err;
				break;
			}

			/* Switch to the next page. */
			page = next;
		}

		/* Copy the key item onto the page. */
		if (key != NULL) {
			++page->hdr->u.entries;
			key_item.len = key->size;
			memcpy(page->first_free, &key_item, sizeof(key_item));
			memcpy(page->first_free +
			    sizeof(key_item), key->data, key->size);
			page->space_avail -= WT_ITEM_SPACE_REQ(key->size);

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
		data_item.len = data->size;
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
		    data_item.type != WT_ITEM_DUP &&
		    data_item.type != WT_ITEM_DUP_OVFL) {
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
			if ((ret = __wt_dup_offpage(db, page, &key,
			    &data, lastkey, dup_data, dup_count, cb)) != 0)
				goto err;

			/* Restart our dup counters. */
			dup_count = dup_space = 0;

			goto skip_read;
		}
	}

	/* A ret of 1 just means we've reached the end of the input. */
	if (ret == 1) {
		ret = 0;

		/* Write any partially-filled page and parent. */
		if (page != NULL && (tret = __wt_db_fwrite(db, page)) != 0)
			ret = tret;
		if (parent != NULL &&
		    (tret = __wt_db_fwrite(db, parent)) != 0 && ret == 0)
			ret = tret;
	}

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
__wt_dup_offpage(DB *db, WT_PAGE *leaf_page,
    DBT **keyp, DBT **datap, DBT *lastkey, WT_ITEM *dup_data,
    u_int32_t dup_count, int (*cb)(DB *, DBT **, DBT **))
{
	enum { SECOND_PAGE, SUBSEQUENT_PAGES } bulk_state;
	DBT *key, *data, tmpkey;
	WT_ITEM data_item, *item;
	WT_ITEM_OFFP offpage_item;
	WT_ITEM_OVFL data_local, tmp_ovfl;
	WT_PAGE *next, *parent, *page;
	u_int32_t len;
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
	bulk_state = SECOND_PAGE;
	parent = NULL;
	ret = 0;

	/* Allocate and initialize a new page. */
	if ((ret = __wt_db_falloc(db, WT_FRAGS_PER_PAGE(db), &page)) != 0)
		return (ret);
	page->hdr->type = WT_PAGE_DUP_LEAF;
	page->hdr->u.entries = dup_count;
	page->hdr->prntaddr = leaf_page->addr;
	page->hdr->nextaddr = page->hdr->prevaddr = WT_ADDR_INVALID;

	/* Copy the duplicate data set into place, and set page information. */
	len = (u_int32_t)(leaf_page->first_free - (u_int8_t *)dup_data);
	memcpy(page->first_free, dup_data, (size_t)len);
	page->first_free = WT_PAGE_BYTE(page) + len;
	page->space_avail = db->pagesize -
	    (u_int32_t)((page->first_free - (u_int8_t *)page->hdr));

	/*
	 * Clean up the caller's page.  Replace the duplicate set with a single
	 * WT_ITEM_OFFP structure, that is, we've moved dup_count - 1 entries.
	 */
	/*lint -esym(613,leaf_page)
	 * LINT complains leafpage may be used before being set -- that's not
	 * correct, we aren't called with an NULL page.
	 */
	WT_CLEAR(data_item);
	data_item.len = sizeof(WT_ITEM_OFFP);
	data_item.type = WT_ITEM_OFFPAGE;
	offpage_item.addr = page->addr;
	offpage_item.records = dup_count;
	p = (u_int8_t *)dup_data;
	memcpy(p, &data_item, sizeof(data_item));
	memcpy(p + sizeof(data_item), &offpage_item, sizeof(offpage_item));
	leaf_page->hdr->u.entries -= (dup_count - 1);
	leaf_page->first_free =
	    (u_int8_t *)dup_data + WT_ITEM_SPACE_REQ(dup_data->len);
	leaf_page->space_avail = db->pagesize -
	    (u_int32_t)(leaf_page->first_free - (u_int8_t *)leaf_page->hdr);

	/* Read in new duplicate records until the key changes. */
	while ((ret = cb(db, &key, &data)) == 0) {
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
		 * If there's insufficient space available, allocate a new
		 * page.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			if ((ret = __wt_db_falloc(
			    db, WT_FRAGS_PER_PAGE(db), &next)) != 0)
				goto err;
			next->hdr->type = WT_PAGE_DUP_LEAF;
			next->hdr->nextaddr =
			    next->hdr->prntaddr = WT_ADDR_INVALID;
			next->hdr->prevaddr = page->addr;
			page->hdr->nextaddr = next->addr;

			/*
			 * If this is the second page, create an internal page
			 * and promote a key from the first page plus the
			 * current key.
			 *
			 * If this is a subsequent page, promote the current
			 * key.
			 *
			 * If this is the second or subsequent page, write the
			 * previous page.
			 */
			switch (bulk_state) {
			case SECOND_PAGE:
				/*
				 * We have to get a copy of the first data item
				 * on the page.   It might be an overflow item,
				 * of course.
				 */
				WT_CLEAR(tmpkey);
				item = (WT_ITEM *)WT_PAGE_BYTE(page);
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
				if ((ret = __wt_db_promote(
				    db, &parent, page, &tmpkey)) != 0)
					goto err;
				bulk_state = SUBSEQUENT_PAGES;
				/* FALLTHROUGH */
			case SUBSEQUENT_PAGES:
				if ((ret = __wt_db_promote(
				    db, &parent, next, data)) != 0)
					goto err;

				if ((ret = __wt_db_fwrite(db, page)) != 0)
					goto err;
				break;
			}

			/* Switch to the next page. */
			page = next;
		}

		/* Copy the data item onto the page. */
		++dup_count;
		++page->hdr->u.entries;
		data_item.len = data->size;
		memcpy(page->first_free, &data_item, sizeof(data_item));
		memcpy(page->first_free +
		    sizeof(data_item), data->data, data->size);
		page->space_avail -= WT_ITEM_SPACE_REQ(data->size);
		page->first_free += WT_ITEM_SPACE_REQ(data->size);
	}

	/*
	 * Write last page and parent -- ret values of 1 and 0 are both "OK",
	 * the ret value of 1 means we reached the end of the bulk input.
	 */
	if ((ret == 0 || ret == 1) && (tret = __wt_db_fwrite(db, page)) != 0)
		ret = tret;
	if (parent != NULL &&
	    (ret == 0 || ret == 1) && (tret = __wt_db_fwrite(db, parent)) != 0)
		ret = tret;

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
__wt_db_promote(DB *db, WT_PAGE **parentp, WT_PAGE *page, DBT *key)
{
	WT_ITEM item;
	WT_ITEM_OFFP offp;
	WT_ITEM_OVFL key_ovfl;
	WT_PAGE *palloc, *parent;
	int ret;

	parent = *parentp;

	WT_CLEAR(item);
	WT_CLEAR(key_ovfl);

	/*
	 * If we don't already have a parent page, or if a new item pair won't
	 * fit on the current page, allocate a new one.
	 */
	if (parent == NULL) {
split:		if ((ret =
		    __wt_db_falloc(db, WT_FRAGS_PER_PAGE(db), &palloc)) != 0)
			return (ret);
		palloc->hdr->type = WT_PAGE_INT;
		palloc->hdr->prevaddr =
		    parent == NULL ? WT_ADDR_INVALID : parent->addr;
		palloc->hdr->nextaddr = palloc->hdr->prntaddr = WT_ADDR_INVALID;

		/* Write any previous parent page. */
		if (parent != NULL) {
			parent->hdr->nextaddr = palloc->addr;
			if ((ret = __wt_db_fwrite(db, parent)) != 0)
				return (ret);
		}

		/* Swap pages. */
		parent = palloc;
	}

	/*
	 * See if both chunks will fit.  If they don't, we have to split.
	 *
	 * Lint thinks that key might be NULL -- it better not be.
	 */
	WT_ASSERT(db->ienv, key != NULL);
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
	memcpy(parent->first_free, &item, sizeof(item));
	memcpy(parent->first_free + sizeof(item), key->data, key->size);
	parent->first_free += WT_ITEM_SPACE_REQ(key->size);
	parent->space_avail -= WT_ITEM_SPACE_REQ(key->size);

	/* Create the internal page reference. */
	item.type = WT_ITEM_OFFPAGE;
	item.len = sizeof(WT_ITEM_OFFP);
	offp.addr = page->addr;
	offp.records = 0;

	/* Store the internal page reference. */
	++parent->hdr->u.entries;
	memcpy(parent->first_free, &item, sizeof(item));
	memcpy(parent->first_free + sizeof(item), &offp, sizeof(offp));
	parent->first_free += WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP));
	parent->space_avail -= WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP));

	/*
	 * Update our caller -- may already be set, but re-setting isn't
	 * a problem.
	 */
	page->hdr->prntaddr = parent->addr;

	/* Return the last parent page. */
	*parentp = parent;

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
