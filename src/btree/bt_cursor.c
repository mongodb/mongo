/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

/*
 * __wt_btcur_first --
 *	Move to the first record in the tree.
 */
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

/*
 * __wt_btcur_next --
 *	Move to the next record in the tree.
 */
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

	btree = cbt->btree;
	cursor = &cbt->iface;
	huffman = btree->huffman_data;
	session = (SESSION *)cbt->iface.session;

	if (cbt->walk.tree == NULL)
		return (__wt_btcur_first(cbt));

	for (;;) {
		while (cbt->nitems == 0) {
			WT_RET(__wt_walk_next(session, &cbt->walk,
			    0, &cbt->ref));
			if (cbt->ref == NULL) {
				F_CLR(cursor, WT_CURSTD_POSITIONED);
				return (WT_NOTFOUND);
			}
			if (cbt->ref->page->type != WT_PAGE_ROW_LEAF)
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
			 * The key and value variables reference the items we'll
			 * print.  Set the key.
			 */
			if (__wt_key_process(cbt->rip)) {
				WT_RET(__wt_key_build(session,
				    cbt->ref->page, cbt->rip, cbt->key_tmp));
				key = &cbt->key_tmp->item;
			} else
				key = (WT_ITEM *)cbt->rip;

			cursor->key.item = *key;

			/*
			 * If the item was ever modified, dump the data from
			 * the WT_UPDATE entry.
			 */
			if (upd != NULL) {
				cursor->value.item.data = WT_UPDATE_DATA(upd);
				cursor->value.item.size = upd->size;
				break;
			}

			/* Check for empty data. */
			if (WT_ROW_EMPTY_ISSET(cbt->rip)) {
				cursor->value.item.data = "";
				cursor->value.item.size = 0;
				break;
			}

			/* Set cell to reference the value we'll dump. */
			cell = WT_ROW_PTR(cbt->ref->page, cbt->rip);
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_DATA:
				if (huffman == NULL) {
					cursor->value.item.data =
					    WT_CELL_BYTE(cell);
					cursor->value.item.size =
					    WT_CELL_LEN(cell);
					break;
				}
				/* FALLTHROUGH */
			case WT_CELL_DATA_OVFL:
				WT_RET(__wt_cell_process(session,
				    cell, cbt->value_tmp));
				cursor->value.item = cbt->value_tmp->item;
				break;
			}
			break;
		}

		if (cbt->nitems == 0)
			continue;

		/* We have a key/value pair, return it and move on. */
		++cbt->rip;
		cbt->nitems--;
		return (0);
	}
	/* NOTREACHED */
}

/*
 * __wt_btcur_prev --
 *	Move to the previous record in the tree.
 */
int
__wt_btcur_prev(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(CURSOR_BTREE *cbt, int *exact)
{
	WT_UNUSED(cbt);
	WT_UNUSED(exact);

	return (ENOTSUP);
}

/*
 * __wt_btcur_insert --
 *	Insert a record into the tree.
 */
int
__wt_btcur_insert(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(CURSOR_BTREE *cbt)
{
	WT_UNUSED(cbt);

	return (ENOTSUP);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(CURSOR_BTREE *cbt, const char *config)
{
	SESSION *session;

	WT_UNUSED(config);

	session = (SESSION *)cbt->iface.session;
	__wt_walk_end(session, &cbt->walk);
	return (0);
}
