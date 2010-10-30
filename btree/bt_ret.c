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
 * __wt_bt_dbt_return --
 *	Retrun a WT_PAGE/WT_{ROW,COL}_INDX pair to the application.
 */
int
__wt_bt_dbt_return(WT_TOC *toc, DBT *key, DBT *data, int key_return)
{
	DB *db;
	DBT local_key, local_data;
	ENV *env;
	IDB *idb;
	WT_COL *cip;
	WT_ITEM *item;
	WT_PAGE *page, *ovfl_page;
	WT_PAGE_HDR *hdr;
	WT_ROW *rip;
	WT_REPL *repl;
	void *data_ret;
	u_int32_t size_ret;
	int (*callback)(DB *, DBT *, DBT *), ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ovfl_page = NULL;
	callback = data->callback;
	ret = 0;

	page = toc->srch_page;
	hdr = page->hdr;
	cip = toc->srch_ip;
	rip = toc->srch_ip;
	repl = toc->srch_repl;

	/*
	 * Handle the key item -- the key may be unchanged, in which case we
	 * don't touch it, it's already correct.
	 *
	 * If the key/data items are being passed to a callback routine and
	 * there's nothing special about them (they aren't uninstantiated
	 * overflow or compressed items), then give the callback a pointer to
	 * the on-page data.  (We use a local DBT in this case, so we don't
	 * touch potentially allocated application DBT memory.)  Else, copy
	 * the items into the application's DBTs.
	 *
	 * If the key/data item are uninstantiated overflow and/or compressed
	 * items, they require processing before being copied into the DBTs.
	 * Don't allocate WT_INDX memory for key/data items here.  (We never
	 * allocate WT_INDX memory for data items.   We do allocate WT_INDX
	 * memory for keys, but if we are looking at a key only to return it,
	 * it's not that likely to be accessed again (think of a cursor moving
	 * through the tree).  Use memory in the application's DBT instead, it
	 * is discarded when the WT_TOC is discarded.
	 *
	 * Key return implies a reference to a WT_ROW index (we don't return
	 * record number keys yet, that will probably change when I add cursor
	 * support).
	 */
	if (key_return) {
		if (WT_KEY_PROCESS(rip)) {
			WT_RET(__wt_bt_item_process(
			    toc, rip->key, NULL, &toc->key));

			key->data = toc->key.data;
			key->size = toc->key.size;
		} else if (callback == NULL) {
			if (toc->key.mem_size < rip->size)
				WT_RET(__wt_realloc(env,
				    &toc->key.mem_size,
				    rip->size, &toc->key.data));
			memcpy(toc->key.data, rip->key, rip->size);
			toc->key.size = rip->size;

			key->data = toc->key.data;
			key->size = toc->key.size;
		} else {
			WT_CLEAR(local_key);
			key = &local_key;
			key->data = rip->key;
			key->size = rip->size;
		}
	}

	/*
	 * Handle the data item.
	 *
	 * If the item was ever replaced, it's easy, take the last replacement
	 * data item, it's just a byte string.
	 */
	if (repl != NULL) {
		if (WT_REPL_DELETED_ISSET(repl))
			return (WT_NOTFOUND);
		data->data = WT_REPL_DATA(repl);
		data->size = repl->size;
		return (callback == NULL ? 0 : callback(db, key, data));
	}

	/* Otherwise, take the item from the original page. */
	switch (hdr->type) {
	case WT_PAGE_COL_FIX:
		data_ret = cip->data;
		size_ret = db->fixed_len;
		break;
	case WT_PAGE_COL_RCC:
		data_ret = WT_RCC_REPEAT_DATA(cip->data);
		size_ret = db->fixed_len;
		break;
	case WT_PAGE_COL_VAR:
		item = cip->data;
		goto item_set;
	case WT_PAGE_ROW_LEAF:
	case WT_PAGE_DUP_LEAF:
		item = rip->data;
item_set:	switch (WT_ITEM_TYPE(item)) {
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_DUP:
			if (idb->huffman_data == NULL) {
				data_ret = WT_ITEM_BYTE(item);
				size_ret = WT_ITEM_LEN(item);
			}
			/* FALLTHROUGH */
		case WT_ITEM_DATA_OVFL:
		case WT_ITEM_DATA_DUP_OVFL:
			/*
			 * If there's a callback function, pass the item_process
			 * function a WT_PAGE reference, that way we never copy
			 * the data, we pass a pointer into the cache page to
			 * the callback function.  If there's no callback, then
			 * don't pass a WT_PAGE reference, might as well let it
			 * do the copy for us.
			 */
			WT_ERR(__wt_bt_item_process(toc, item,
			    callback == NULL ? NULL : &ovfl_page, &toc->data));
			if (ovfl_page == NULL) {
				data_ret = toc->data.data;
				size_ret = toc->data.size;
			} else {
				data_ret = WT_PAGE_BYTE(ovfl_page);
				size_ret = ovfl_page->hdr->u.datalen;
			}
			break;
		WT_ILLEGAL_FORMAT(db);
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * When we get here, data_ret and size_ret are set to the byte string
	 * and the length we're going to return.   That byte string has been
	 * decoded, we called __wt_bt_item_process above in all cases where the
	 * item could be encoded.
	 */
	if (callback == NULL) {
		/*
		 * We're copying the key/data pair out to the caller.  If we
		 * haven't yet copied the data_ret/size_ret pair into the return
		 * DBT (potentially done by __wt_bt_item_process), do so now.
		 */
		if (data_ret != toc->data.data) {
			if (toc->data.mem_size < size_ret)
				WT_ERR(__wt_realloc(env,
				    &toc->data.mem_size,
				    size_ret, &toc->data.data));
			memcpy(toc->data.data, data_ret, size_ret);
			toc->data.size = size_ret;
		}

		data->data = toc->data.data;
		data->size = toc->data.size;
	} else {
		/*
		 * If we're given a callback function, use the data_ret/size_ret
		 * fields as set.
		 */
		WT_CLEAR(local_data);
		data = &local_data;
		data->data = data_ret;
		data->size = size_ret;
		ret = callback(db, key, data);
	}

	/*
	 * Release any overflow page the __wt_bt_item_process function returned
	 * us.
	 */
err:	if (ovfl_page != NULL)
		__wt_bt_page_out(toc, &ovfl_page, 0);

	return (ret);
}
