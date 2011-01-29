/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_col_update(WT_TOC *, uint64_t, DBT *, int);

/*
 * __wt_db_col_del --
 *	Db.col_del method.
 */
inline int
__wt_db_col_del(WT_TOC *toc, uint64_t recno)
{
	return (__wt_col_update(toc, recno, NULL, 0));
}

/*
 * __wt_db_col_put --
 *	Db.put method.
 */
inline int
__wt_db_col_put(WT_TOC *toc, uint64_t recno, DBT *data)
{
	DB *db;

	db = toc->db;

	if (db->fixed_len != 0 && data->size != db->fixed_len)
		WT_RET(__wt_database_wrong_fixed_size(toc, data->size));

	return (__wt_col_update(toc, recno, data, 1));
}

/*
 * __wt_col_update --
 *	Column store delete and update.
 */
static int
__wt_col_update(WT_TOC *toc, uint64_t recno, DBT *data, int data_overwrite)
{
	DB *db;
	ENV *env;
	WT_PAGE *page;
	WT_RCC_EXPAND *exp, **new_rccexp;
	WT_REPL **new_repl, *repl;
	int ret;

	env = toc->env;
	db = toc->db;

	page = NULL;
	exp = NULL;
	new_rccexp = NULL;
	new_repl = NULL;
	repl = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_col_search(
	    toc, recno, WT_NOLEVEL, data_overwrite ? WT_DATA_OVERWRITE : 0));
	page = toc->srch_page;

	/*
	 * Repeat-count compressed (RCC) column store operations are difficult
	 * because each original on-disk index for an RCC can represent large
	 * numbers of records, and we're only deleting a single one of those
	 * records, which means working in the WT_RCC_EXPAND array.  All other
	 * column store deletes are simple changes where a new WT_REPL entry is
	 * entered into the page's modification array.  There are three code
	 * paths:
	 *
	 * 1: column store deletes other than RCC column stores: delete an entry
	 * from the on-disk page by creating a new WT_REPL entry, and linking it
	 * into the WT_REPL array.
	 *
	 * 2: an RCC column store delete of an already modified record: create
	 * a new WT_REPL entry, and link it to the WT_RCC_EXPAND entry's WT_REPL
	 * list.
	 *
	 * 3: an RCC column store delete of a record not yet modified: create
	 * a new WT_RCC_EXPAND/WT_REPL pair, and link it into the WT_RCC_EXPAND
	 * array.
	 */
	switch (page->dsk->type) {
	case WT_PAGE_COL_FIX:				/* #1 */
	case WT_PAGE_COL_VAR:
		/* Allocate a page replacement array if necessary. */
		if (page->u2.repl == NULL)
			WT_ERR(__wt_calloc(env,
			    page->indx_count, sizeof(WT_REPL *), &new_repl));

		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_repl_alloc(toc, &repl, data));

		/* workQ: schedule insert of the WT_REPL structure. */
		__wt_item_update_serial(toc, page, toc->srch_write_gen,
		    WT_COL_SLOT(page, toc->srch_ip), new_repl, repl, ret);
		 break;
	case WT_PAGE_COL_RCC:
		if (toc->srch_repl != NULL) {		/* #2 */
			/* Allocate a WT_REPL structure and fill it in. */
			WT_ERR(__wt_repl_alloc(toc, &repl, data));

			/* workQ: schedule insert of the WT_REPL structure. */
			__wt_rcc_expand_repl_serial(toc, page,
			    toc->srch_write_gen, toc->srch_exp, repl, ret);
			break;
		}
							/* #3 */
		/* Allocate a page expansion array as necessary. */
		if (page->u2.rccexp == NULL)
			WT_ERR(__wt_calloc(env, page->indx_count,
			    sizeof(WT_RCC_EXPAND *), &new_rccexp));

		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_repl_alloc(toc, &repl, data));

		/* Allocate a WT_RCC_EXPAND structure and fill it in. */
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_RCC_EXPAND), &exp));
		exp->recno = recno;
		exp->repl = repl;

		/* Schedule the workQ to link in the WT_RCC_EXPAND structure. */
		__wt_rcc_expand_serial(toc, page, toc->srch_write_gen,
		    WT_COL_SLOT(page, toc->srch_ip), new_rccexp, exp, ret);
		break;
	WT_ILLEGAL_FORMAT_ERR(db, ret);
	}

	if (ret != 0) {
err:		if (exp != NULL)
			__wt_free(env, exp, sizeof(WT_RCC_EXPAND));
		if (repl != NULL)
			__wt_repl_free(toc, repl);
	}

	/* Free any allocated page expansion array unless the workQ used it. */
	if (new_rccexp != NULL && new_rccexp != page->u2.rccexp)
		__wt_free(env,
		    new_rccexp, page->indx_count * sizeof(WT_RCC_EXPAND *));

	/* Free any page replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != page->u2.repl)
		__wt_free(env, new_repl, page->indx_count * sizeof(WT_REPL *));

	WT_PAGE_OUT(toc, page);

	return (0);
}

/*
 * __wt_rcc_expand_serial_func --
 *	Server function to expand a repeat-count compressed column store
 *	during a delete.
 */
int
__wt_rcc_expand_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_RCC_EXPAND **new_rccexp, *exp;
	uint32_t slot, write_gen;
	int ret;

	ret = 0;

	__wt_rcc_expand_unpack(toc, page, write_gen, slot, new_rccexp, exp);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * If the page does not yet have an expansion array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->u2.rccexp == NULL)
		page->u2.rccexp = new_rccexp;

	/*
	 * Insert the new WT_RCC_EXPAND as the first item in the forward-linked
	 * list of expansion structures.  Flush memory to ensure the list is
	 * never broken.
	 */
	exp->next = page->u2.rccexp[slot];
	WT_MEMORY_FLUSH;
	page->u2.rccexp[slot] = exp;

err:	__wt_toc_serialize_wrapup(toc, page, ret);
	return (0);
}

/*
 * __wt_rcc_expand_repl_serial_func --
 *	Server function to update a WT_REPL entry in an already expanded
 *	repeat-count compressed column store during a delete.
 */
int
__wt_rcc_expand_repl_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_RCC_EXPAND *exp;
	WT_REPL *repl;
	uint32_t write_gen;
	int ret;

	ret = 0;

	__wt_rcc_expand_repl_unpack(toc, page, write_gen, exp, repl);

	/* Check the page's write-generation. */
	WT_ERR(__wt_page_write_gen_check(page, write_gen));

	/*
	 * Insert the new WT_REPL as the first item in the forward-linked list
	 * of replacement structures from the WT_RCC_EXPAND structure.  Flush
	 * memory to ensure the list is never broken.
	 */
	repl->next = exp->repl;
	WT_MEMORY_FLUSH;
	exp->repl = repl;

err:	__wt_toc_serialize_wrapup(toc, page, ret);
	return (0);
}
