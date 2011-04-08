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
	SESSION *session;
	WT_CELL *cell;
	WT_CURSOR *cursor;
	WT_UPDATE *upd;
	void *huffman;

	btree = cbt->btree;
	cursor = &cbt->iface;
	huffman = btree->huffman_value;
	session = (SESSION *)cbt->iface.session;
	upd = NULL;

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
			switch (cbt->ref->page->type) {
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_RLE:
			case WT_PAGE_COL_VAR:
				cbt->cip = cbt->ref->page->u.col_leaf.d;
				cbt->recno = cbt->ref->page->u.col_leaf.recno;
				break;
			case WT_PAGE_ROW_LEAF:
				cbt->rip = cbt->ref->page->u.row_leaf.d;
				break;
			default:
				continue;
			}

			cbt->nitems = cbt->ref->page->indx_count;
		}

		for (; cbt->nitems > 0;
		    ++cbt->cip, ++cbt->rip, ++cbt->recno, cbt->nitems--) {
			/* Check for deletion. */
			switch (cbt->ref->page->type) {
			case WT_PAGE_COL_FIX:
			case WT_PAGE_COL_VAR:
				upd = WT_COL_UPDATE(cbt->ref->page, cbt->cip);
				break;
			case WT_PAGE_ROW_LEAF:
				upd = WT_ROW_UPDATE(cbt->ref->page, cbt->rip);
				break;
			}

			if (upd != NULL && WT_UPDATE_DELETED_ISSET(upd))
				continue;

			/*
			 * The key and value variables reference the items we'll
			 * print.  Set the key.
			 */
			if (!F_ISSET(btree, WT_COLUMN)) {
				if (__wt_key_process(cbt->rip))
					WT_RET(__wt_key_build(session,
					    cbt->ref->page, cbt->rip,
					    &cursor->key));

				cursor->key.data = cbt->rip->key;
				cursor->key.size = cbt->rip->size;
			}

			/*
			 * If the item was ever modified, dump the data from
			 * the WT_UPDATE entry.
			 */
			if (upd != NULL) {
				cursor->value.data = WT_UPDATE_DATA(upd);
				cursor->value.size = upd->size;
				break;
			}

			/* Check for empty data. */
			if (F_ISSET(btree, WT_COLUMN)) {
				cell = WT_COL_PTR(cbt->ref->page, cbt->cip);
				switch (WT_CELL_TYPE(cell)) {
				case WT_CELL_DATA:
					if (huffman == NULL) {
						cursor->value.data =
						    WT_CELL_BYTE(cell);
						cursor->value.size =
						    WT_CELL_LEN(cell);
						break;
					}
					/* FALLTHROUGH */
				case WT_CELL_DATA_OVFL:
					WT_RET(__wt_cell_process(session,
					    cell, &cursor->value));
					break;
				case WT_CELL_DEL:
					continue;
				WT_ILLEGAL_FORMAT(session);
				}
				break;
			}

			if (WT_ROW_EMPTY_ISSET(cbt->rip)) {
				cursor->value.data = "";
				cursor->value.size = 0;
				break;
			}

			/* Set cell to reference the value we'll dump. */
			cell = WT_ROW_PTR(cbt->ref->page, cbt->rip);
			switch (WT_CELL_TYPE(cell)) {
			case WT_CELL_DATA:
				if (huffman == NULL) {
					cursor->value.data = WT_CELL_BYTE(cell);
					cursor->value.size = WT_CELL_LEN(cell);
					break;
				}
				/* FALLTHROUGH */
			case WT_CELL_DATA_OVFL:
				WT_RET(__wt_cell_process(session, cell,
				    &cursor->value));
				break;
			}
			break;
		}

		if (cbt->nitems == 0)
			continue;

		/* We have a key/value pair, return it and move on. */
		if (cbt->nrepeats > 0)
			cbt->nrepeats--;
		else {
			cbt->nitems--;
			++cbt->rip;
			++cbt->cip;
		}
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
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	*exact = 0;
	if (F_ISSET(btree, WT_COLUMN))
		return (__wt_btree_col_get(session,
		    cursor->recno, (WT_ITEM *)&cursor->value));
	else
		return (__wt_btree_row_get(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value));
}

/*
 * __wt_btcur_insert --
 *	Insert a record into the tree.
 */
int
__wt_btcur_insert(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	if (F_ISSET(btree, WT_COLUMN))
		return (__wt_btree_col_put(session,
		    cursor->recno, (WT_ITEM *)&cursor->value));
	else
		return (__wt_btree_row_put(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value));
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	if (F_ISSET(btree, WT_COLUMN))
		return (__wt_btree_col_put(session,
		    cursor->recno, (WT_ITEM *)&cursor->value));
	else
		return (__wt_btree_row_put(session,
		    (WT_ITEM *)&cursor->key, (WT_ITEM *)&cursor->value));
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(CURSOR_BTREE *cbt)
{
	BTREE *btree;
	SESSION *session;
	WT_CURSOR *cursor;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (SESSION *)cursor->session;

	if (F_ISSET(btree, WT_COLUMN))
		return (__wt_btree_col_del(session, cursor->recno));
	else
		return (__wt_btree_row_del(session, (WT_ITEM *)&cursor->key));
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
