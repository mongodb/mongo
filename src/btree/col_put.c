/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_col_insert_alloc(SESSION *, uint64_t, WT_INSERT **);
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
__wt_btree_col_put(SESSION *session, uint64_t recno, WT_ITEM *value)
{
	BTREE *btree;

	btree = session->btree;

	if (btree->fixed_len != 0 && value->size != btree->fixed_len)
		WT_RET(__wt_file_wrong_fixed_size(
		    session, value->size, btree->fixed_len));

	return (__wt_col_update(session, recno, value, 1));
}

/*
 * __wt_col_update --
 *	Column-store delete and update.
 */
static int
__wt_col_update(SESSION *session, uint64_t recno, WT_ITEM *value, int is_write)
{
	WT_PAGE *page;
	WT_INSERT **new_ins, *ins;
	WT_UPDATE **new_upd, *upd;
	int ret;

	new_ins = NULL;
	ins = NULL;
	new_upd = NULL;
	upd = NULL;
	ret = 0;

	/* Search the btree for the key. */
	WT_RET(__wt_col_search(session, recno, is_write ? WT_WRITE : 0));
	page = session->srch_page;

	/*
	 * Delete or update a column-store entry.
	 *
	 * Run-length encoded (RLE) column-store changes are hard because each
	 * original on-disk index for an RLE can represent a large number of
	 * records, and we're only changing a single one of those records,
	 * which means working in the WT_INSERT array.  All other column-store
	 * modifications are simple, adding a new WT_UPDATE entry to the page's
	 * modification array.  There are three code paths:
	 *
	 * 1: column-store changes other than RLE column stores: update the
	 * original on-disk page entry by creating a new WT_UPDATE entry and
	 * linking it into the WT_UPDATE array.
	 *
	 * 2: RLE column-store changes of an already changed record: create
	 * a new WT_UPDATE entry, and link it to an existing WT_INSERT entry's
	 * WT_UPDATE list.
	 *
	 * 3: RLE column-store change of not-yet-changed record: create a new
	 * WT_INSERT/WT_UPDATE pair and link it into the WT_INSERT array.
	 */
	switch (page->type) {
	case WT_PAGE_COL_FIX:					/* #1 */
	case WT_PAGE_COL_VAR:
		/* Allocate an update array as necessary. */
		if (session->srch_upd == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->indx_count, &new_upd));
			/*
			 * If there was no update array, the search function
			 * could not have set the WT_UPDATE location.
			 */
			session->srch_upd = &new_upd[session->srch_slot];
		}
		goto simple_update;
	case WT_PAGE_COL_RLE:
		/* Allocate an update array as necessary. */
		if (session->srch_upd != NULL) {		/* #2 */
simple_update:		WT_ERR(__wt_update_alloc(session, value, &upd));

			/* workQ: insert the WT_UPDATE structure. */
			__wt_update_serial(
			    session, page, session->srch_write_gen,
			    new_upd, session->srch_upd, upd, ret);
			break;
		}
								/* #3 */
		/* Allocate an insert array as necessary. */
		if (session->srch_ins == NULL) {
			WT_ERR(__wt_calloc_def(
			    session, page->indx_count, &new_ins));
			/*
			 * If there was no insert array, the search function
			 * could not have set the WT_INSERT location.
			 */
			session->srch_ins = &new_ins[session->srch_slot];
		}

		/* Allocate a WT_INSERT/WT_UPDATE pair. */
		WT_ERR(__wt_col_insert_alloc(session, recno, &ins));
		WT_ERR(__wt_update_alloc(session, value, &upd));
		ins->upd = upd;

		/* workQ: insert the WT_INSERT structure. */
		__wt_insert_serial(
		    session, page, session->srch_write_gen,
		    new_ins, session->srch_ins, ins, ret);
		break;
	}

	if (ret != 0) {
err:		if (ins != NULL)
			__wt_sb_free_error(session, ins->sb);
		if (upd != NULL)
			__wt_sb_free_error(session, upd->sb);
	}

	/* Free any insert array unless the workQ used it. */
	if (new_ins != NULL && new_ins != page->u.col_leaf.ins)
		__wt_free(session, new_ins);

	/* Free any update array unless the workQ used it. */
	if (new_upd != NULL && new_upd != page->u.col_leaf.upd)
		__wt_free(session, new_upd);

	WT_PAGE_OUT(session, page);

	return (0);
}

/*
 * __wt_col_insert_alloc --
 *	Column-store insert: allocate a WT_INSERT structure from the SESSION's
 *	buffer and fill it in.
 */
static int
__wt_col_insert_alloc(SESSION *session, uint64_t recno, WT_INSERT **insp)
{
	SESSION_BUFFER *sb;
	WT_INSERT *ins;

	/*
	 * Allocate the WT_INSERT structure and room for the key, then copy
	 * the key into place.
	 */
	WT_RET(__wt_sb_alloc(
	    session, sizeof(WT_INSERT) + sizeof(uint64_t), &ins, &sb));

	ins->sb = sb;
	WT_INSERT_RECNO(ins) = recno;

	*insp = ins;
	return (0);
}
