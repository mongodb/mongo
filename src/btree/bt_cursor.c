/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_clear --
 *	Reset the cursor.
 */
void
__wt_cursor_clear(WT_CURSOR_BTREE *cbt)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cbt->iface.session;

	if (cbt->page != NULL) {
		__wt_page_release(session, cbt->page);
		cbt->page = NULL;
	}

	/* Reset the cursor iteration state. */
	F_CLR(cbt, WT_CBT_SEARCH_SET);
}

/*
 * __cursor_flags_begin --
 *	Cursor function initial flags handling.
 */
static inline void
__cursor_flags_begin(WT_CURSOR_BTREE *cbt)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/* Release any page references we're holding. */
	__wt_cursor_clear(cbt);

	/* Reset the key/value state. */
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
}

/*
 * __cursor_flags_end --
 *	Cursor function final flags handling.
 */
static inline void
__cursor_flags_end(WT_CURSOR_BTREE *cbt, int ret)
{
	WT_CURSOR *cursor;

	cursor = &cbt->iface;

	/*
	 * On success, we're returning a key/value pair, and can iterate.
	 * On error, release any page references we're holding.
	 */
	if (ret == 0)
		F_SET(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	else
		__wt_cursor_clear(cbt);
}

/*
 * __cursor_deleted --
 *	Return if the item is currently deleted.
 */
static inline int
__cursor_deleted(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK unpack;
	WT_COL *cip;
	WT_INSERT *ins;
	WT_PAGE *page;

	btree = cbt->btree;
	ins = cbt->ins;
	page = cbt->page;

	/* If we found an item on an insert list, check there. */
	if (ins != NULL)
		return (WT_UPDATE_DELETED_ISSET(ins->upd) ? 1 : 0);

	/* Otherwise, check for an update in the page's slots. */
	switch (btree->type) {
	case BTREE_COL_FIX:
		break;
	case BTREE_COL_VAR:
		cip = &page->u.col_leaf.d[cbt->slot];
		if ((cell = WT_COL_PTR(page, cip)) == NULL)
			return (WT_NOTFOUND);
		__wt_cell_unpack(cell, &unpack);
		if (unpack.type == WT_CELL_DEL)
			return (1);
		break;
	case BTREE_ROW:
		if (page->u.row_leaf.upd != NULL &&
		    page->u.row_leaf.upd[cbt->slot] != NULL &&
		    WT_UPDATE_DELETED_ISSET(page->u.row_leaf.upd[cbt->slot]))
			return (1);
		break;
	}
	return (0);
}

/*
 * __wt_btcur_search --
 *	Search for a matching record in the tree.
 */
int
__wt_btcur_search(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_BSTAT_INCR(session, file_read);

	__cursor_flags_begin(cbt);

	ret = btree->type == BTREE_ROW ?
	    __wt_row_search(session, cbt, 0) : __wt_col_search(session, cbt, 0);
	if (ret == 0) {
		if (cbt->compare != 0 || __cursor_deleted(cbt))
			ret = WT_NOTFOUND;
		else
			ret = __wt_kv_return(session, cbt, 0);
	}

	__cursor_flags_end(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_search_near --
 *	Search for a record in the tree.
 */
int
__wt_btcur_search_near(WT_CURSOR_BTREE *cbt, int *exact)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int (*srch)(WT_SESSION_IMPL *, WT_CURSOR_BTREE *, int);
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	srch = btree->type == BTREE_ROW ? __wt_row_search : __wt_col_search;

	WT_BSTAT_INCR(session, file_readnear);

	/*
	 * We assume "range-prefix" semantics are more likely, so where we don't
	 * have an exact match we prefer to return tree keys greater than the
	 * search key, rather than less than the search key.
	 *
	 * If we find an exact match, or the search key is smaller than the tree
	 * key, and the tree key has not been deleted, return the tree key.
	 */
	__cursor_flags_begin(cbt);
	WT_ERR(srch(session, cbt, 0));
	if (cbt->compare == 0 || cbt->compare == 1)
		if (!__cursor_deleted(cbt)) {
			*exact = cbt->compare;
			ret = __wt_kv_return(session, cbt, 1);
			goto done;
		}

	/*
	 * Otherwise, we have a deleted key, or the tree key is smaller than the
	 * search key: move to the next key in the tree.  Cursor next skips over
	 * deleted records, so we don't have to test for them.
	 */
	*exact = 1;
	if ((ret = __wt_btcur_next(cbt)) != WT_NOTFOUND)
		goto done;

	/*
	 * If there's no larger tree key, we repeat the search (we've discarded
	 * the original position information).   This time we'll take anything
	 * that's not deleted, and we'll look for a previous record instead of
	 * a subsequent record.  If we don't find a previous record, there's no
	 * record to return, quit.
	 */
	__cursor_flags_begin(cbt);
	WT_ERR(srch(session, cbt, 0));
	if (!__cursor_deleted(cbt)) {
		*exact = cbt->compare;
		ret = __wt_kv_return(session, cbt, 1);
		goto done;
	}

	*exact = -1;
	ret = __wt_btcur_prev(cbt);

err:
done:	__cursor_flags_end(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_insert --
 *	Insert a record into the tree.
 */
int
__wt_btcur_insert(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_inserts);

retry:	__cursor_flags_begin(cbt);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		/*
		 * Insert in column stores allocates a new key (ignoring the
		 * application's key), and creates a new record.
		 *
		 * XXX
		 * This semantic not yet implemented.
		 */
		WT_ERR(__wt_col_search(session, cbt, 1));
		if ((ret = __wt_col_modify(session, cbt, 0)) == WT_RESTART)
			goto retry;
		break;
	case BTREE_ROW:
		/*
		 * Insert in row stores fails if the key exists (and the
		 * configuration "overwrite" not set), otherwise creates
		 * a new record.
		 */
		while ((ret = __wt_row_search(session, cbt, 1)) == WT_RESTART)
			;
		if (ret == 0) {
			if (cbt->compare == 0 && !__cursor_deleted(cbt) &&
			    !F_ISSET(cursor, WT_CURSTD_OVERWRITE))
				ret = WT_DUPLICATE_KEY;
			else
				if ((ret = __wt_row_modify(
				    session, cbt, 0)) == WT_RESTART)
					goto retry;
		}
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_flags_end(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_remove --
 *	Remove a record from the tree.
 */
int
__wt_btcur_remove(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_removes);

retry:	__cursor_flags_begin(cbt);

	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__wt_col_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_deleted(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_col_modify(session, cbt, 1)) == WT_RESTART)
			goto retry;
		break;
	case BTREE_ROW:
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_deleted(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_row_modify(session, cbt, 1)) == WT_RESTART)
			goto retry;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_flags_end(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_update --
 *	Update a record in the tree.
 */
int
__wt_btcur_update(WT_CURSOR_BTREE *cbt)
{
	WT_BTREE *btree;
	WT_CURSOR *cursor;
	WT_SESSION_IMPL *session;
	int ret;

	btree = cbt->btree;
	cursor = &cbt->iface;
	session = (WT_SESSION_IMPL *)cursor->session;
	WT_BSTAT_INCR(session, file_updates);

retry:	__cursor_flags_begin(cbt);

	switch (btree->type) {
	case BTREE_COL_FIX:
		if (cursor->value.size != 1) {
			__wt_errx(session,
			    "item size of %" PRIu32 " does not match "
			    "fixed-length file requirement of 1 byte",
			    cursor->value.size);
			return (WT_ERROR);
		}
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		/* Update in column stores is an unconditional overwrite. */
		WT_ERR(__wt_col_search(session, cbt, 1));
		if ((ret  = __wt_col_modify(session, cbt, 0)) == WT_RESTART)
			goto retry;
		break;
	case BTREE_ROW:
		/*
		 * Update in row stores fails if the key doesn't exist, else
		 * overwrites the value.
		 */
		WT_ERR(__wt_row_search(session, cbt, 1));
		if (cbt->compare != 0 || __cursor_deleted(cbt))
			ret = WT_NOTFOUND;
		else if ((ret = __wt_row_modify(session, cbt, 0)) == WT_RESTART)
			goto retry;
		break;
	WT_ILLEGAL_FORMAT(session);
	}

err:	__cursor_flags_end(cbt, ret);

	return (ret);
}

/*
 * __wt_btcur_close --
 *	Close a btree cursor.
 */
int
__wt_btcur_close(WT_CURSOR_BTREE *cbt, const char *config)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(config);
	session = (WT_SESSION_IMPL *)cbt->iface.session;

	__wt_cursor_clear(cbt);

	__wt_buf_free(session, &cbt->value);

	return (0);
}
