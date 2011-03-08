/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

/*
 * __wt_value_return --
 *	Return a WT_PAGE/WT_{ROW,COL}_INDX pair to the application.
 */
int
__wt_value_return(
    SESSION *session, WT_ITEM *key, WT_ITEM *value, int key_return)
{
	BTREE *btree;
	WT_ITEM local_key, local_value;
	WT_COL *cip;
	WT_CELL *cell;
	WT_PAGE *page;
	WT_PAGE_DISK *dsk;
	WT_ROW *rip;
	WT_UPDATE *upd;
	const void *value_ret;
	uint32_t size_ret;
	int (*callback)(BTREE *, WT_ITEM *, WT_ITEM *), ret;

	btree = session->btree;
	callback = NULL; /* TODO: was value->callback */
	ret = 0;

	page = session->srch_page;
	dsk = page->dsk;
	cip = session->srch_ip;
	rip = session->srch_ip;
	upd = session->srch_upd;

	/*
	 * Handle the key item -- the key may be unchanged, in which case we
	 * don't touch it, it's already correct.
	 *
	 * If the key/value items are being passed to a callback routine and
	 * there's nothing special about them (they aren't uninstantiated
	 * overflow or compressed items), then give the callback a pointer to
	 * the on-page data.  (We use a local WT_ITEM in this case, so we don't
	 * touch potentially allocated application WT_ITEM memory.)  Else, copy
	 * the items into the application's WT_DATAITEMs.
	 *
	 * If the key/value item are uninstantiated overflow and/or compressed
	 * items, they require processing before being copied into the
	 * WT_DATAITEMs.  Don't allocate WT_INDX memory for key/value items
	 * here.  (We never allocate WT_INDX memory for data items.   We do
	 * allocate WT_INDX memory for keys, but if we are looking at a key
	 * only to return it, it's not that likely to be accessed again (think
	 * of a cursor moving through the tree).  Use memory in the
	 * application's WT_ITEM instead, it is discarded when the SESSION is
	 * discarded.
	 *
	 * Key return implies a reference to a WT_ROW index (we don't return
	 * record number keys yet, that will probably change when I add cursor
	 * support).
	 */
	if (key_return) {
		if (__wt_key_process(rip)) {
			WT_RET(__wt_cell_process(
			    session, rip->key, &session->key));

			*key = session->key.item;
		} else if (callback == NULL) {
			WT_RET(
			    __wt_buf_grow(session, &session->key, rip->size));
			memcpy(session->key.mem, rip->key, rip->size);

			*key = session->key.item;
		} else {
			WT_CLEAR(local_key);
			key = &local_key;
			key->data = rip->key;
			key->size = rip->size;
		}
	}

	/*
	 * Handle the value.
	 *
	 * If the item was ever updated, it's easy, take the last update,
	 * it's just a byte string.
	 */
	if (upd != NULL) {
		if (WT_UPDATE_DELETED_ISSET(upd))
			return (WT_NOTFOUND);
		value->data = WT_UPDATE_DATA(upd);
		value->size = upd->size;
		return (callback == NULL ? 0 : callback(btree, key, value));
	}

	/* Otherwise, take the item from the original page. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
		value_ret = WT_COL_PTR(dsk, cip);
		size_ret = btree->fixed_len;
		break;
	case WT_PAGE_COL_RLE:
		value_ret = WT_RLE_REPEAT_DATA(WT_COL_PTR(dsk, cip));
		size_ret = btree->fixed_len;
		break;
	case WT_PAGE_COL_VAR:
		cell = WT_COL_PTR(dsk, cip);
		goto cell_set;
	case WT_PAGE_ROW_LEAF:
		cell = rip->value;
cell_set:	switch (WT_CELL_TYPE(cell)) {
		case WT_CELL_DATA:
			if (btree->huffman_data == NULL) {
				value_ret = WT_CELL_BYTE(cell);
				size_ret = WT_CELL_LEN(cell);
			}
			/* FALLTHROUGH */
		case WT_CELL_DATA_OVFL:
			WT_RET(__wt_cell_process(
			    session, cell, &session->value));
			value_ret = session->value.item.data;
			size_ret = session->value.item.size;
			break;
		WT_ILLEGAL_FORMAT(btree);
		}
		break;
	WT_ILLEGAL_FORMAT(btree);
	}

	/*
	 * When we get here, value_ret and size_ret are set to the byte string
	 * and the length we're going to return.   That byte string has been
	 * decoded, we called __wt_cell_process above in all cases where the
	 * item could be encoded.
	 */
	if (callback == NULL) {
		/*
		 * We're copying the key/value pair out to the caller.  If we
		 * haven't yet copied the value_ret/size_ret pair into the
		 * return WT_ITEM (potentially done by __wt_cell_process), do
		 * so now.
		 */
		if (value_ret != session->value.item.data) {
			WT_RET(__wt_buf_grow(
			    session, &session->value, size_ret));
			memcpy(session->value.mem, value_ret, size_ret);
		}

		*value = session->value.item;
	} else {
		/*
		 * If we're given a callback function, use the data_ret/size_ret
		 * fields as set.
		 */
		WT_CLEAR(local_value);
		value = &local_value;
		value->data = value_ret;
		value->size = size_ret;
		ret = callback(btree, key, value);
	}

	return (ret);
}
