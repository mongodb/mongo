/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_col_update(SESSION *, uint64_t, WT_ITEM *, int);

/*
 * __wt_btree_col_del --
 *	Db.col_del method.
 */
int
__wt_btree_col_del(SESSION *session, uint64_t recno)
{
	return (__wt_col_update(session, recno, NULL, 0));
}

/*
 * __wt_btree_col_put --
 *	Db.put method.
 */
int
__wt_btree_col_put(SESSION *session, uint64_t recno, WT_ITEM *data)
{
	BTREE *btree;

	btree = session->btree;

	if (btree->fixed_len != 0 && data->size != btree->fixed_len)
		WT_RET(__wt_file_wrong_fixed_size(session, data->size));

	return (__wt_col_update(session, recno, data, 1));
}

/*
 * __wt_col_update --
 *	Column-store delete and update.
 */
static int
__wt_col_update(SESSION *session, uint64_t recno, WT_ITEM *data, int data_overwrite)
{
	BTREE *btree;
	WT_PAGE *page;
	WT_RLE_EXPAND *exp, **new_rleexp;
	WT_UPDATE **new_upd, *upd;
	int ret;

	btree = session->btree;

	page = NULL;
	exp = NULL;
	new_rleexp = NULL;
	new_upd = NULL;
	upd = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_col_search(
	    session, recno, data_overwrite ? WT_DATA_OVERWRITE : 0));
	page = session->srch_page;

	/*
	 * Run-length encoded (RLE) column-store operations are hard because
	 * each original on-disk index for an RLE can represent large numbers
	 * of records, and we're only deleting a single one of those records,
	 * which means working in the WT_RLE_EXPAND array.  All other column
	 * store deletes are simple changes where a new WT_UPDATE entry is
	 * added to the page's modification array.  There are three code paths:
	 *
	 * 1: column-store deletes other than RLE column stores: delete an entry
	 * from the on-disk page by creating a new WT_UPDTAE entry, and linking
	 * it into the WT_UPDATE array.
	 *
	 * 2: an RLE column-store delete of an already modified record: create
	 * a new WT_UPDATE entry, and link it to the WT_RLE_EXPAND entry's
	 * WT_UPDATE list.
	 *
	 * 3: an RLE column-store delete of a record not yet modified: create
	 * a new WT_RLE_EXPAND/WT_UPDATE pair, link it into the WT_RLE_EXPAND
	 * array.
	 */
	switch (page->dsk->type) {
	case WT_PAGE_COL_FIX:				/* #1 */
	case WT_PAGE_COL_VAR:
		/* Allocate an update array if necessary. */
		if (page->u.col_leaf.upd == NULL)
			WT_ERR(
			    __wt_calloc_def(session, page->indx_count, &new_upd));

		/* Allocate a WT_UPDATE structure and fill it in. */
		WT_ERR(__wt_update_alloc(session, &upd, data));

		/* workQ: schedule insert of the WT_UPDATE structure. */
		__wt_item_update_serial(session, page, session->srch_write_gen,
		    WT_COL_INDX_SLOT(page, session->srch_ip), new_upd, upd, ret);
		 break;
	case WT_PAGE_COL_RLE:
		if (session->srch_upd != NULL) {		/* #2 */
			/* Allocate a WT_UPDATE structure and fill it in. */
			WT_ERR(__wt_update_alloc(session, &upd, data));

			/* workQ: schedule insert of the WT_UPDATE structure. */
			__wt_rle_expand_update_serial(session, page,
			    session->srch_write_gen, session->srch_exp, upd, ret);
			break;
		}
							/* #3 */
		/* Allocate a page expansion array as necessary. */
		if (page->u.col_leaf.rleexp == NULL)
			WT_ERR(__wt_calloc_def(
			    session, page->indx_count, &new_rleexp));

		/* Allocate a WT_UPDATE structure and fill it in. */
		WT_ERR(__wt_update_alloc(session, &upd, data));

		/* Allocate a WT_RLE_EXPAND structure and fill it in. */
		WT_ERR(__wt_calloc_def(session, 1, &exp));
		exp->recno = recno;
		exp->upd = upd;

		/* Schedule the workQ to link in the WT_RLE_EXPAND structure. */
		__wt_rle_expand_serial(session, page, session->srch_write_gen,
		    WT_COL_INDX_SLOT(page, session->srch_ip), new_rleexp, exp, ret);
		break;
	WT_ILLEGAL_FORMAT_ERR(btree, ret);
	}

	if (ret != 0) {
err:		if (exp != NULL)
			__wt_free(session, exp, sizeof(WT_RLE_EXPAND));
		if (upd != NULL)
			__wt_update_free(session, upd);
	}

	/* Free any allocated page expansion array unless the workQ used it. */
	if (new_rleexp != NULL && new_rleexp != page->u.col_leaf.rleexp)
		__wt_free(session,
		    new_rleexp, page->indx_count * sizeof(WT_RLE_EXPAND *));

	/* Free any update array unless the workQ used it. */
	if (new_upd != NULL && new_upd != page->u.col_leaf.upd)
		__wt_free(session, new_upd, page->indx_count * sizeof(WT_UPDATE *));

	WT_PAGE_OUT(session, page);

	return (0);
}

/*
 * __wt_rle_expand_serial_func --
 *	Server function to expand a run-length encoded column-store during a
 *	delete.
 */
int
__wt_rle_expand_serial_func(SESSION *session)
{
	WT_PAGE *page;
	WT_RLE_EXPAND **new_rleexp, *exp;
	uint32_t slot, write_gen;
	int ret;

	ret = 0;

	__wt_rle_expand_unpack(session, page, write_gen, slot, new_rleexp, exp);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page does not yet have an expansion array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->u.col_leaf.rleexp == NULL)
		page->u.col_leaf.rleexp = new_rleexp;

	/*
	 * Insert the new WT_RLE_EXPAND as the first item in the forward-linked
	 * list of expansion structures.  Flush memory to ensure the list is
	 * never broken.
	 */
	exp->next = page->u.col_leaf.rleexp[slot];
	WT_MEMORY_FLUSH;
	page->u.col_leaf.rleexp[slot] = exp;

err:	__wt_session_serialize_wrapup(session, page, ret);
	return (0);
}

/*
 * __wt_rle_expand_update_serial_func --
 *	Server function to update a WT_UPDATE entry in an already expanded
 *	run-length encoded column-store during a delete.
 */
int
__wt_rle_expand_update_serial_func(SESSION *session)
{
	WT_PAGE *page;
	WT_RLE_EXPAND *exp;
	WT_UPDATE *upd;
	uint32_t write_gen;
	int ret;

	ret = 0;

	__wt_rle_expand_update_unpack(session, page, write_gen, exp, upd);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * Insert the new WT_UPDATE as the first item in the forward-linked list
	 * of update structures from the WT_RLE_EXPAND structure.  Flush memory
	 * to ensure the list is never broken.
	 */
	upd->next = exp->upd;
	WT_MEMORY_FLUSH;
	exp->upd = upd;

err:	__wt_session_serialize_wrapup(session, page, ret);
	return (0);
}
