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
	WT_OVFL *ovfl;
	WT_PAGE *page, *ovfl_page;
	WT_ROW *rip;
	WT_REPL *repl;
	void *orig;
	u_int32_t size;
	int (*callback)(DB *, DBT *, DBT *), ret;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ovfl = NULL;
	callback = data->callback;
	ret = 0;

	page = toc->srch_page;
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
			WT_RET(__wt_bt_key_process(toc, NULL, rip, &toc->key));

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
	switch (page->hdr->type) {
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		if (repl != NULL)
			goto repl;
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		if (repl != NULL) {
repl:			if (WT_REPL_DELETED_ISSET(repl->data))
				return (WT_NOTFOUND);
			data->data = repl->data;
			data->size = repl->size;
			return (callback == NULL ? 0 : callback(db, key, data));
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	switch (page->hdr->type) {
	case WT_PAGE_COL_FIX:
		if (callback != NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = F_ISSET(idb, WT_REPEAT_COMP) ?
			    WT_FIX_REPEAT_DATA(cip->data) : cip->data;
			data->size = db->fixed_len;
			return (callback(db, key, data));
		}
		orig = F_ISSET(idb, WT_REPEAT_COMP) ?
		    WT_FIX_REPEAT_DATA(cip->data) : cip->data;
		size = db->fixed_len;
		break;
	case WT_PAGE_COL_VAR:
		item = cip->data;
		goto item_set;
	case WT_PAGE_ROW_LEAF:
		item = rip->data;
item_set:	if (callback != NULL &&
		    WT_ITEM_TYPE(item) == WT_ITEM_DATA &&
		    idb->huffman_data == NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = WT_ITEM_BYTE(item);
			data->size = WT_ITEM_LEN(item);
			return (callback(db, key, data));
		}

		if (WT_ITEM_TYPE(item) == WT_ITEM_DATA) {
			orig = WT_ITEM_BYTE(item);
			size = WT_ITEM_LEN(item);
		} else
			ovfl = WT_ITEM_BYTE_OVFL(item);
		break;
	case WT_PAGE_DUP_LEAF:
		item = rip->data;
		if (callback != NULL &&
		    WT_ITEM_TYPE(item) == WT_ITEM_DUP &&
		    idb->huffman_data == NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = rip->key;
			data->size = rip->size;
			return (callback(db, key, data));
		}

		if (WT_ITEM_TYPE(item) == WT_ITEM_DUP) {
			orig = rip->key;
			size = rip->size;
		} else
			ovfl = WT_ITEM_BYTE_OVFL(item);
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (ovfl != NULL) {
		WT_RET(__wt_bt_ovfl_in(toc, ovfl, &ovfl_page));
		orig = WT_PAGE_BYTE(ovfl_page);
		size = ovfl->size;
	}

	if (idb->huffman_data == NULL) {
		if (toc->data.mem_size < size)
			WT_ERR(__wt_realloc(
			    env, &toc->data.mem_size, size, &toc->data.data));
		memcpy(toc->data.data, orig, size);
		toc->data.size = size;
	} else
		 WT_ERR(__wt_huffman_decode(idb->huffman_data, orig, size,
		     &toc->data.data, &toc->data.mem_size, &toc->data.size));

	data->data = toc->data.data;
	data->size = toc->data.size;

err:	if (ovfl != NULL)
		__wt_bt_page_out(toc, &ovfl_page, 0);

	return (ret != 0 ? ret :
	    (callback == NULL ? 0 : callback(db, key, data)));
}
