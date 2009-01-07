/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_dbt_copy(ENV *, DBT *, DBT *);
static int __wt_bt_dup_offpage(DB *, WT_PAGE *, DBT **, DBT **,
    DBT *, WT_ITEM *, u_int32_t, int (*cb)(DB *, DBT **, DBT **));
static int __wt_bt_promote(DB *, WT_PAGE *, u_int32_t *);

/*
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(DB *db, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data, *lastkey, lastkey_std, lastkey_ovfl;
	ENV *env;
	WT_ITEM key_item, data_item, *dup_key, *dup_data;
	WT_ITEM_OVFL key_ovfl, data_ovfl;
	WT_PAGE *page, *next;
	u_int32_t dup_count, dup_space, len;
	int ret, tret;

	env = db->env;

	DB_FLAG_CHK(db, "Db.bulk_load", flags, WT_APIMASK_DB_BULK_LOAD);
	WT_ASSERT(env, LF_ISSET(WT_SORTED_INPUT));

	page = NULL;
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
	 * correct, state is set to FIRST_PAGE until page is initialized.
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
		WT_STAT_INCR(db,
		    BULK_PAIRS_READ, "bulk key/data pairs inserted");

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

			WT_STAT_INCR(db, BULK_DUP_DATA_READ,
			    "bulk duplicate data pairs read");

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
				    __wt_bt_dbt_copy(env, key, lastkey)) != 0)
					goto err;
			}

			key_ovfl.len = key->size;
			if ((ret =
			    __wt_bt_ovfl_write(db, key, &key_ovfl.addr)) != 0)
				goto err;
			key->data = &key_ovfl;
			key->size = sizeof(key_ovfl);

			key_item.type = WT_ITEM_KEY_OVFL;
			WT_STAT_INCR(db, BULK_OVERFLOW_KEY,
			    "bulk overflow key items read");
		} else
			key_item.type = WT_ITEM_KEY;

		if (data->size > db->maxitemsize) {
			data_ovfl.len = data->size;
			if ((ret = __wt_bt_ovfl_write(
			    db, data, &data_ovfl.addr)) != 0)
				goto err;
			data->data = &data_ovfl;
			data->size = sizeof(data_ovfl);

			data_item.type =
			    key == NULL ? WT_ITEM_DUP_OVFL : WT_ITEM_DATA_OVFL;

			WT_STAT_INCR(db, BULK_OVERFLOW_DATA,
			    "bulk overflow data items read");
		} else
			data_item.type =
			    key == NULL ? WT_ITEM_DUP : WT_ITEM_DATA;

		/*
		 * We now know what the key/data items will look like on a
		 * page.  If we haven't yet allocated a page, or there is
		 * insufficient space on the current page, allocate a new page.
		 */
		if (page == NULL ||
		    (key == NULL ? 0 : WT_ITEM_SPACE_REQ(key->size)) +
		    WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			if ((ret = __wt_cache_db_alloc(
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
				dup_key = (WT_ITEM *)WT_PAGE_BYTE(next);
				dup_data =
				    (WT_ITEM *)(dup_key +
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
			 * If we've finished with a page, promote its first key
			 * to its parent and write it.   The promotion function
			 * sets the page's parent address, which is the same for
			 * the newly allocated page.
			 */
			if (page != NULL) {
				if ((ret =
				    __wt_bt_promote(db, page, NULL)) != 0)
					goto err;
				next->hdr->prntaddr = page->hdr->prntaddr;
				if ((ret = __wt_cache_db_out(
				    db, page, WT_MODIFIED)) != 0)
					goto err;
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
			if ((ret = __wt_bt_dup_offpage(db, page, &key,
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

		/* Promote a key from any partially-filled page and write it. */
		if (page != NULL) {
			ret = __wt_bt_promote(db, page, NULL);
			if ((tret = __wt_cache_db_out(
			    db, page, WT_MODIFIED)) != 0 && ret == 0)
				ret = tret;
		}
	}

	if (0) {
err:		ret = WT_ERROR;
	}
	if (lastkey_ovfl.data != NULL)
		__wt_free(env, lastkey_ovfl.data);

	return (ret == 1 ? 0 : ret);
}

/*
 * __wt_bt_dup_offpage --
 *	Move the last set of duplicates on the page to a page of their own,
 *	then load the rest of the duplicate set.
 */
static int
__wt_bt_dup_offpage(DB *db, WT_PAGE *leaf_page,
    DBT **keyp, DBT **datap, DBT *lastkey, WT_ITEM *dup_data,
    u_int32_t dup_count, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data;
	WT_ITEM data_item;
	WT_ITEM_OFFP offpage_item;
	WT_ITEM_OVFL data_local;
	WT_PAGE *next, *page;
	u_int32_t root_addr, len;
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
	WT_CLEAR(data_item);
	ret = 0;

	/* Allocate and initialize a new page. */
	if ((ret = __wt_cache_db_alloc(db, WT_FRAGS_PER_PAGE(db), &page)) != 0)
		return (ret);
	page->hdr->type = WT_PAGE_DUP_LEAF;
	page->hdr->u.entries = dup_count;
	page->hdr->prntaddr =
	    page->hdr->prevaddr = page->hdr->nextaddr =  WT_ADDR_INVALID;

	/*
	 * Unless we have enough duplicates to split this page, it will be the
	 * "root" of the offpage duplicates.
	 */
	root_addr = page->addr;

	/* Copy the duplicate data set into place, and set page information. */
	len = (u_int32_t)(leaf_page->first_free - (u_int8_t *)dup_data);
	memcpy(page->first_free, dup_data, (size_t)len);
	page->first_free = WT_PAGE_BYTE(page) + len;
	page->space_avail = db->pagesize -
	    (u_int32_t)((page->first_free - (u_int8_t *)page->hdr));

	/*
	 * Reset the caller's page entry count.   Once we know the final root
	 * page and record count we'll replace the duplicate set with a single
	 * WT_ITEM_OFFP structure, that is, we've moved dup_count - 1 entries.
	 */
	/*lint -esym(613,leaf_page)
	 * LINT complains leafpage may be used before being set -- that's not
	 * correct, we aren't called with a NULL page.
	 */
	leaf_page->hdr->u.entries -= (dup_count - 1);

	/* Read in new duplicate records until the key changes. */
	while ((ret = cb(db, &key, &data)) == 0) {
		if (key->size == 0) {
			__wt_db_errx(db, "zero-length keys are not supported");
			goto err;
		}
		WT_STAT_INCR(db, BULK_PAIRS_READ, NULL);
		WT_STAT_INCR(db, BULK_DUP_DATA_READ, NULL);

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
			if ((ret = __wt_bt_ovfl_write(
			    db, data, &data_local.addr)) != 0)
				goto err;
			data->data = &data_local;
			data->size = sizeof(data_local);
			data_item.type = WT_ITEM_DUP_OVFL;
			WT_STAT_INCR(db, BULK_OVERFLOW_DATA, NULL);
		} else
			data_item.type = WT_ITEM_DUP;

		/*
		 * If there's insufficient space available, allocate a new
		 * page.
		 */
		if (WT_ITEM_SPACE_REQ(data->size) > page->space_avail) {
			if ((ret = __wt_cache_db_alloc(
			    db, WT_FRAGS_PER_PAGE(db), &next)) != 0)
				goto err;
			next->hdr->type = WT_PAGE_DUP_LEAF;
			next->hdr->nextaddr =
			    next->hdr->prntaddr = WT_ADDR_INVALID;
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
			if ((ret = __wt_bt_promote(db, page, &root_addr)) != 0)
				goto err;
			next->hdr->prntaddr = page->hdr->prntaddr;
			if ((ret =
			    __wt_cache_db_out(db, page, WT_MODIFIED)) != 0)
				goto err;

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
	 * Ret values of 1 and 0 are both "OK", the ret value of 1 means we
	 * reached the end of the bulk input.
	 *
	 * Promote a key from any partially-filled page and write it.
	 */
	if ((tret = __wt_bt_promote(
	    db, page, &root_addr)) != 0 && (ret == 0 || ret == 1))
		ret = tret;
	if ((tret = __wt_cache_db_out(
	    db, page, WT_MODIFIED)) != 0 && (ret == 0 || ret == 1))
		ret = tret;

	/*
	 * Replace the caller's duplicate set with a WT_ITEM_OFFP structure,
	 * and reset the caller's page information.
	 */
	data_item.len = sizeof(WT_ITEM_OFFP);
	data_item.type = WT_ITEM_OFFPAGE;
	offpage_item.addr = root_addr;
	offpage_item.records = dup_count;
	p = (u_int8_t *)dup_data;
	memcpy(p, &data_item, sizeof(data_item));
	memcpy(p + sizeof(data_item), &offpage_item, sizeof(offpage_item));
	leaf_page->first_free =
	    (u_int8_t *)dup_data + WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP));
	leaf_page->space_avail = db->pagesize -
	    (u_int32_t)(leaf_page->first_free - (u_int8_t *)leaf_page->hdr);

	if (0) {
err:		ret = WT_ERROR;
	}
	return (ret);
}

/*
 * __wt_bt_promote --
 *	Promote the first item on a page to a parent.
 */
static int
__wt_bt_promote(DB *db, WT_PAGE *page, u_int32_t *root_addrp)
{
	DBT key;
	WT_ITEM *key_item, item;
	WT_ITEM_OFFP offp;
	WT_ITEM_OVFL tmp_ovfl;
	WT_PAGE *next, *parent;
	u_int32_t parent_addr, root_addr;
	int need_promotion, ret, root_split, tret;

	WT_CLEAR(key);
	WT_CLEAR(item);
	next = parent = NULL;

	/*
	 * Get a copy of the first key on the page -- it might be an overflow
	 * item, in which case we need to make a copy for the database.  Most
	 * versions of Berkeley DB tried to reference count overflow items if
	 * they were promoted to internal pages.  That turned out to be hard
	 * to get right, so I'm not doing it again.
	 */
	key_item = (WT_ITEM *)WT_PAGE_BYTE(page);
	switch (key_item->type) {
	case WT_ITEM_DUP:
	case WT_ITEM_KEY:
		key.data = WT_ITEM_BYTE(key_item);
		item.type = WT_ITEM_KEY;
		item.len = key.size = key_item->len;
		break;
	case WT_ITEM_DUP_OVFL:
	case WT_ITEM_KEY_OVFL:
		WT_CLEAR(tmp_ovfl);
		if ((ret = __wt_bt_ovfl_copy(db,
		    (WT_ITEM_OVFL *)WT_ITEM_BYTE(key_item),
		    &tmp_ovfl)) != 0)
			return (ret);
		key.data = &tmp_ovfl;
		item.type = WT_ITEM_KEY_OVFL;
		item.len = key.size = sizeof(tmp_ovfl);
		break;
	default:
		return (__wt_database_format(db));
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
	 * page and it's parent, and the page's key wouldn't fit on the parent
	 * and we have to split the parent.  This path has two different cases,
	 * based on if a parent of the parent exists.   Here's a diagram of case
	 * #2, where the parent also has a parent:
	 *
	 * p -> P1 -> P2	(case #2)
	 *
	 * The promoted key from p didn't fit onto P1, and so we split P1:
	 *
	 *	P1
	 *	^
	 *	|  -> P2
	 *	v
	 * p -> P3
	 *
	 * In case #2, we allocate P3, copy the first key from the page to it,
	 * then recursively call the promote code to promote the first key from
	 * P3 to P2.
	 *
	 * Here's a diagram of case #3, where the parent does not have a parent,
	 * in other words, a root split:
	 *
	 * p -> P1		(case #3)
	 *
	 * The promoted key from p didn't fit onto P1, and so we split P1:
	 *
	 *	P1
	 *	^
	 *	|
	 *	v
	 * p -> P2
	 *
	 * In case #3, we allocate P2, copy the first key from the page to it,
	 * but then recursively call the promote code to promote the first key
	 * from P1 to a new page, and again to promote the first key from P2 to
	 * a new page.
	 */
	parent_addr = page->hdr->prntaddr;
	if (parent_addr == WT_ADDR_INVALID) {
split:		if ((ret =
		    __wt_cache_db_alloc(db, WT_FRAGS_PER_PAGE(db), &next)) != 0)
			goto err;
		next->hdr->type =
		    page->hdr->type == WT_PAGE_INT ||
		    page->hdr->type == WT_PAGE_LEAF ?
		    WT_PAGE_INT : WT_PAGE_DUP_INT;
		next->hdr->prevaddr =
		    next->hdr->nextaddr = next->hdr->prntaddr = WT_ADDR_INVALID;

		/*
		 * Case #1 -- it's a modified root split, so if we're in the
		 * primary tree we have to update the WT_PAGE_DESC area.
		 */
		if (parent_addr == WT_ADDR_INVALID) {
			root_split =  1;
			root_addr = next->addr;
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
			 * the key from the existing parent page, then copy the
			 * address into our newly allocated parent page for this
			 * level.
			 */
			if (parent->hdr->prntaddr == WT_ADDR_INVALID) {
				root_split = 1;
				if ((ret = __wt_bt_promote(
				    db, parent, root_addrp)) != 0)
					goto err;
				root_addr = parent->hdr->prntaddr;
			} else
				root_split = 0;

			next->hdr->prntaddr = parent->hdr->prntaddr;

			/* Discard the old parent page, we have a new one. */
			if ((ret =
			    __wt_cache_db_out(db, parent, WT_MODIFIED)) != 0)
				goto err;

			need_promotion = 1;
		}

		/*
		 * In case #1 and case #3, we're doing a root split.  If it's
		 * the primary tree, we have to update the WT_PAGE_DESC area
		 * of the database.  For an offpage duplicates set, pass back
		 * the root page so our caller can use it.
		 */
		if (root_split)
			if (next->hdr->type == WT_PAGE_INT) {
				if ((ret =
				    __wt_bt_desc_set_root(db, root_addr)) != 0)
					goto err;
			} else
				*root_addrp = root_addr;

		/* There's a new parent page, update the page's parent ref. */
		page->hdr->prntaddr = next->addr;
		parent = next;
		next = NULL;
	} else {
		if ((ret = __wt_cache_db_in(db, parent_addr,
		    WT_FRAGS_PER_PAGE(db), &parent, 0)) != 0)
			goto err;

		need_promotion = 0;
	}

	/*
	 * See if both chunks will fit (if they don't, we have to split).
	 * We don't check for overflow keys: if the key was an overflow,
	 * we already created a smaller, on-page version of it.
	 */
	if (parent->space_avail <
	    WT_ITEM_SPACE_REQ(sizeof(WT_ITEM_OFFP)) +
	    WT_ITEM_SPACE_REQ(key.size))
		goto split;

	/* Store the key. */
	++parent->hdr->u.entries;
	memcpy(parent->first_free, &item, sizeof(item));
	memcpy(parent->first_free + sizeof(item), key.data, key.size);
	parent->first_free += WT_ITEM_SPACE_REQ(key.size);
	parent->space_avail -= WT_ITEM_SPACE_REQ(key.size);

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
	 * The promotion for case #2 and the second part of case #3 --
	 * promote the key from the newly allocated internal page to its
	 * parent.
	 */
	if (need_promotion)
		ret = __wt_bt_promote(db, parent, root_addrp);

err:	/* Discard the parent page. */
	if (parent != NULL && (tret =
	    __wt_cache_db_out(db, parent, WT_MODIFIED)) != 0 && ret == 0)
		ret = tret;
	if (next != NULL && (tret =
	    __wt_cache_db_out(db, next, WT_MODIFIED)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_bt_dbt_copy --
 *	Get a local copy of an overflow key.
 */
static int
__wt_bt_dbt_copy(ENV *env, DBT *orig, DBT *copy)
{
	int ret;

	if (copy->data == NULL || copy->alloc_size < orig->size) {
		if ((ret = __wt_realloc(env, orig->size, &copy->data)) != 0)
			return (ret);
		copy->alloc_size = orig->size;
	}
	memcpy(copy->data, orig->data, copy->size = orig->size);

	return (0);
}
