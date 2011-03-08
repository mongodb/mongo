/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

int
__wt_btcur_first(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;

	btree = cbt->btree;
	session = (SESSION *)cbt->iface.session;

	WT_RET(__wt_walk_begin(session, &btree->root_page, &cbt->walk));
	F_SET(&cbt->iface, WT_CURSTD_POSITIONED);

	return (__wt_btcur_next(cbt));
}

int
__wt_btcur_next(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	WT_ITEM *key;
	SESSION *session;
	WT_CELL *cell;
	WT_CURSOR *cursor;
	WT_UPDATE *upd;
	void *huffman;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	huffman = btree->huffman_data;
	session = (SESSION *)cbt->iface.session;
	ret = 0;

	if (cbt->walk.tree == NULL)
		return (__wt_btcur_first(cbt));

	do {
		while (cbt->nitems == 0) {
			WT_RET(__wt_walk_next(session, &cbt->walk,
			    0, &cbt->ref));
			if (cbt->ref == NULL) {
				F_CLR(cursor, WT_CURSTD_POSITIONED);
				return (WT_NOTFOUND);
			}
			if (cbt->ref->page->dsk->type != WT_PAGE_ROW_LEAF)
				continue;

			cbt->nitems = cbt->ref->page->indx_count;
			cbt->rip = cbt->ref->page->u.row_leaf.d;
		}

		for (; cbt->nitems > 0; ++cbt->rip, cbt->nitems--) {
			/* Check for deletion. */
			upd = WT_ROW_UPDATE(cbt->ref->page, cbt->rip);
			if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
				continue;

			/*
			 * The key and value variables reference the DBTs we'll
			 * print.  Set the key.
			 */
			if (__wt_key_process(cbt->rip)) {
				WT_RET(__wt_cell_process(session,
				    cbt->rip->key, cbt->key_tmp));
				key = &cbt->key_tmp->item;
			} else
				key = (WT_ITEM *)cbt->rip;

			cursor->key.item = *key;

			/*
			 * If the item was ever upd, dump the data from the
			 * upd entry.
			 */
			if (upd != NULL) {
				cursor->value.item.data = WT_UPDATE_DATA(upd);
				cursor->value.item.size = upd->size;
				break;
			}

			/* Set data to reference the data we'll dump. */
			cell = cbt->rip->value;
			if (WT_CELL_TYPE(cell) == WT_CELL_DATA) {
				if (huffman == NULL) {
					cursor->value.item.data =
					    WT_CELL_BYTE(cell);
					cursor->value.item.size =
					    WT_CELL_LEN(cell);
					break;
				}
			} else if (WT_CELL_TYPE(cell) == WT_CELL_DATA_OVFL) {
				WT_RET(__wt_cell_process(session,
				    cell, cbt->value_tmp));
				cursor->value.item = cbt->value_tmp->item;
				break;
			} else
				continue;
		}

		if (cbt->nitems == 0)
			continue;

		/* We have a key/value pair, return it and move on. */
		++cbt->rip;
		cbt->nitems--;
		return (0);
	} while (0);

	return (ret);
}

int
__wt_btcur_prev(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

int
__wt_btcur_search_near(CURSOR_BTREE *cbt, int *exact)
{
	WT_UNUSED(cbt);
	WT_UNUSED(exact);

	return (ENOTSUP);
}

int
__wt_btcur_insert(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

int
__wt_btcur_update(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

int
__wt_btcur_del(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

int
__wt_btcur_close(CURSOR_BTREE *cbt, const char *config)
{
	SESSION *session;

	WT_UNUSED(config);

	session = (SESSION *)cbt->iface.session;
	__wt_walk_end(session, &cbt->walk);
	return (0);
}
