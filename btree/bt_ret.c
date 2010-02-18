/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

typedef enum { WT_DATA, WT_KEY, WT_KEY_NO_HUFFMAN } COPY_OP;
static int __wt_bt_dbt_copyout(
    WT_TOC *, DBT *, DBT *, u_int8_t *, u_int32_t, COPY_OP);

/*
 * __wt_bt_dbt_return --
 *	Retrun a WT_PAGE/WT_{ROW,COL}_INDX pair to the application.
 */
int
__wt_bt_dbt_return(WT_TOC *toc,
    DBT *key, DBT *data, WT_PAGE *page, void *ip, int key_return)
{
	DB *db;
	DBT local_key, local_data;
	IDB *idb;
	WT_COL_INDX *cip;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_PAGE *ovfl_page;
	WT_ROW_INDX *rip;
	WT_SDBT *sdbt;
	int (*callback)(DB *, DBT *, DBT *), ret;

	db = toc->db;
	idb = db->idb;
	cip = ip;
	ovfl = NULL;
	rip = ip;
	callback = data->callback;

	/*
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
	 * Handle the key item -- the key may be unchanged, in which case we
	 * don't touch it, it's already correct.
	 *
	 * Key return implies a reference to a WT_ROW_INDX index -- we don't
	 * return record number keys (yet -- that will probably change when I
	 * add cursor support).
	 */
	if (key_return) {
		if (callback != NULL &&
		    rip->data != NULL && !F_ISSET(rip, WT_HUFFMAN)) {
			WT_CLEAR(local_key);
			key = &local_key;
			key->data = rip->data;
			key->size = rip->size;
		} else
			WT_RET(__wt_bt_dbt_copyout(toc, key, &toc->key,
			    rip->data, rip->size, WT_ROW_INDX_PROCESS(rip) ?
			    WT_KEY : WT_KEY_NO_HUFFMAN));
	}

	/*
	 * Handle the data item.
	 *
	 * If it's been updated, it's easy, take the last data item, it's just
	 * a byte string.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		if (rip->repl != NULL) {
			sdbt = rip->repl->data + (rip->repl->repl_next - 1);
			goto repl;
		}
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		if (cip->repl != NULL) {
			sdbt = cip->repl->data + (cip->repl->repl_next - 1);
repl:			if (sdbt->data == WT_DATA_DELETED)
				return (WT_NOTFOUND);
			data->data = sdbt->data;
			data->size = sdbt->size;
			return (callback == NULL ? 0 : callback(db, key, data));
		}
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	/*
	 * Copy the data item out.   In the case of a variable-length column-
	 * or row-store leaf page, we have to get it from the page.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_COL_VAR:
		item = cip->page_data;
		if (callback != NULL &&
		    WT_ITEM_TYPE(item) == WT_ITEM_DATA &&
		    idb->huffman_data == NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = WT_ITEM_BYTE(item);
			data->size = WT_ITEM_LEN(item);
			break;
		}
		if (WT_ITEM_TYPE(item) != WT_ITEM_DATA) {
			ovfl = WT_ITEM_BYTE_OVFL(cip->page_data);
			goto overflow;
		}

		WT_RET(__wt_bt_dbt_copyout(toc, data, &toc->data,
		    WT_ITEM_BYTE(item), WT_ITEM_LEN(item), WT_DATA));
		break;
	case WT_PAGE_ROW_LEAF:
		item = rip->page_data;
		if (callback != NULL &&
		    WT_ITEM_TYPE(item) == WT_ITEM_DATA &&
		    idb->huffman_data == NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = WT_ITEM_BYTE(item);
			data->size = WT_ITEM_LEN(item);
			break;
		}

		if (WT_ITEM_TYPE(item) != WT_ITEM_DATA) {
			ovfl = WT_ITEM_BYTE_OVFL(rip->page_data);
			goto overflow;
		}

		WT_RET(__wt_bt_dbt_copyout(toc, data, &toc->data,
		    WT_ITEM_BYTE(item), WT_ITEM_LEN(item), WT_DATA));
		break;
	case WT_PAGE_DUP_LEAF:
		item = rip->page_data;
		if (callback != NULL &&
		    WT_ITEM_TYPE(item) == WT_ITEM_DUP &&
		    idb->huffman_data == NULL) {
			WT_CLEAR(local_data);
			data = &local_data;
			data->data = rip->data;
			data->size = rip->size;
			break;
		}

		if (WT_ITEM_TYPE(item) != WT_ITEM_DUP) {
			ovfl = WT_ITEM_BYTE_OVFL(rip->page_data);
			goto overflow;
		}

		WT_RET(__wt_bt_dbt_copyout(toc, data,
		    &toc->data, rip->data, rip->size, WT_DATA));
		break;
	WT_ILLEGAL_FORMAT(db);
	}

	if (0) {
overflow:	WT_RET(__wt_bt_ovfl_in(toc, ovfl->addr, ovfl->len, &ovfl_page));
		ret = __wt_bt_dbt_copyout(toc, data,
		    &toc->data, WT_PAGE_BYTE(ovfl_page), ovfl->len, WT_DATA);
		WT_TRET(__wt_bt_page_out(toc, ovfl_page, 0));
		if (ret != 0)
			return (ret);
	}

	return (callback == NULL ? 0 : callback(db, key, data));
}

/*
 * __wt_bt_dbt_copyout --
 *	Do the actual allocation and copy for a returned DBT.
 */
static int
__wt_bt_dbt_copyout(WT_TOC *toc,
    DBT *dbt, DBT *local_dbt, u_int8_t *p, u_int32_t size, COPY_OP op)
{
	ENV *env;
	IDB *idb;
	void *huffman;

	env = toc->env;
	idb = toc->db->idb;
	huffman = NULL;

	/* Uncompress any Huffman-encoded data. */
	switch (op) {
	case WT_DATA:
		huffman = idb->huffman_data;
		break;
	case WT_KEY:
		huffman = idb->huffman_key;
		break;
	case WT_KEY_NO_HUFFMAN:
		break;
	}

	if (huffman == NULL) {
		/* Don't grow the return buffer a byte at a time. */
		if (local_dbt->data_len < size)
			WT_RET(__wt_realloc(env,
			    &local_dbt->data_len, size + 40, &local_dbt->data));
		local_dbt->size = size;
		memcpy(local_dbt->data, p, size);
	} else
		 WT_RET(__wt_huffman_decode(huffman, p, size,
		     &local_dbt->data, &local_dbt->data_len, &local_dbt->size));

	dbt->data = local_dbt->data;
	dbt->size = local_dbt->size;
	return (0);
}
