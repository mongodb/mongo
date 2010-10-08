/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_col_update(WT_TOC *, u_int64_t, DBT *, int);

/*
 * __wt_db_col_put --
 *	Db.put method.
 */
inline int
__wt_db_col_put(WT_TOC *toc, u_int64_t recno, DBT *data)
{
	return (__wt_db_col_update(toc, recno, data, 1));
}

/*
 * __wt_db_col_del --
 *	Db.col_del method.
 */
inline int
__wt_db_col_del(WT_TOC *toc, u_int64_t recno)
{
	return (__wt_db_col_update(toc, recno, NULL, 0));
}

/*
 * __wt_db_col_update --
 *	Column store delete and update.
 */
static int
__wt_db_col_update(WT_TOC *toc, u_int64_t recno, DBT *data, int insert)
{
	ENV *env;
	IDB *idb;
	WT_COL_EXPAND *exp, **new_expcol;
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	int ret;

	env = toc->env;
	idb = toc->db->idb;

	page = NULL;
	exp = NULL;
	new_expcol = NULL;
	new_repl = NULL;
	repl = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_col(toc, recno, insert ? WT_INSERT : 0));
	page = toc->srch_page;

	/*
	 * Repeat-count compressed (RCC) column store operations are difficult
	 * because each original on-disk index for an RCC can represent large
	 * numbers of records, and we're only deleting a single one of those
	 * records, which means working in the WT_COL_EXPAND array.  All other
	 * column store deletes are simple changes where a new WT_REPL entry is
	 * entered into the page's modification array.  There are three code
	 * paths:
	 *
	 * 1: column store deletes other than RCC column stores: delete an entry
	 * from the on-disk page by creating a new WT_REPL entry, and linking it
	 * into the WT_REPL array.
	 *
	 * 2: an RCC column store delete of a record not yet modified: create
	 * a new WT_COL_EXPAND/WT_REPL pair, and link it into the WT_COL_EXPAND
	 * array.
	 *
	 * 3: an RCC columstore n delete of an already modified record: create
	 * a new WT_REPL entry, and link it to the WT_COL_EXPAND entry's WT_REPL
	 * list.
	 */
	if (!F_ISSET(idb, WT_REPEAT_COMP)) {		/* #1 */
		/* Allocate a page replacement array if necessary. */
		if (page->repl == NULL)
			WT_ERR(__wt_calloc(env,
			    page->indx_count, sizeof(WT_REPL *), &new_repl));

		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_update_alloc(toc,
		    sizeof(WT_REPL) + (data == NULL ? 0 : data->size), &repl));
		if (data == NULL)
			repl->data = WT_REPL_DELETED_VALUE;
		else {
			repl->data = (u_int8_t *)repl + sizeof(WT_REPL);
			repl->size = data->size;
			memcpy(repl->data, data->data, data->size);
		}

		/* Schedule the workQ to insert the WT_REPL structure. */
		__wt_bt_update_serial(toc, page, toc->srch_write_gen,
		    WT_COL_SLOT(page, toc->srch_ip), new_repl, repl, ret);
	} else if (toc->srch_repl == NULL) {		/* #2 */
		/* Allocate a page expansion array as necessary. */
		if (page->expcol == NULL)
			WT_ERR(__wt_calloc(env, page->indx_count,
			    sizeof(WT_COL_EXPAND *), &new_expcol));

		/* Allocate a WT_COL_EXPAND structure and fill it in. */
		WT_ERR(__wt_calloc(env, 1, sizeof(WT_COL_EXPAND), &exp));
		exp->rcc_offset = toc->srch_rcc_offset;
		WT_ERR(__wt_update_alloc(toc,
		    sizeof(WT_REPL) + (data == NULL ? 0 : data->size), &repl));
		exp->repl = repl;
		if (data == NULL)
			repl->data = WT_REPL_DELETED_VALUE;
		else {
			repl->data = (u_int8_t *)repl + sizeof(WT_REPL);
			repl->size = data->size;
			memcpy(repl->data, data->data, data->size);
		}

		/* Schedule the workQ to link in the WT_COL_EXPAND structure. */
		__wt_bt_rcc_expand_serial(toc, page, toc->srch_write_gen,
		    WT_COL_SLOT(page, toc->srch_ip), new_expcol, exp, ret);
		goto done;
	} else {					/* #3 */
		/* Allocate a WT_REPL structure and fill it in. */
		WT_ERR(__wt_update_alloc(toc,
		    sizeof(WT_REPL) + (data == NULL ? 0 : data->size), &repl));
		if (data == NULL)
			repl->data = WT_REPL_DELETED_VALUE;
		else {
			repl->data = (u_int8_t *)repl + sizeof(WT_REPL);
			repl->size = data->size;
			memcpy(repl->data, data->data, data->size);
		}

		/* Schedule the workQ to insert the WT_REPL structure. */
		__wt_bt_rcc_expand_repl_serial(
		    toc, page, toc->srch_write_gen, toc->srch_exp, repl, ret);
	}

	if (0) {
err:		if (exp != NULL)
			__wt_free(env, exp, sizeof(WT_COL_EXPAND));
	}

done:	/* Free any allocated page expansion array unless the workQ used it. */
	if (new_expcol != NULL && new_expcol != page->expcol)
		__wt_free(env,
		    new_expcol, page->indx_count * sizeof(WT_COL_EXPAND *));

	/* Free any replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != page->repl)
		__wt_free(env, new_repl, page->indx_count * sizeof(WT_REPL *));

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}

/*
 * __wt_bt_rcc_expand_serial_func --
 *	Server function to expand a repeat-count compressed column store
 *	during a delete.
 */
int
__wt_bt_rcc_expand_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_COL_EXPAND **new_exp, *exp;
	int slot;
	u_int16_t write_gen;

	__wt_bt_rcc_expand_unpack(toc, page, write_gen, slot, new_exp, exp);

	/* Check the page's write-generation, then update it. */
	WT_PAGE_WRITE_GEN_CHECK(page);

	/*
	 * If the page does not yet have an expansion array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->expcol == NULL)
		page->expcol = new_exp;

	/*
	 * Insert the new WT_COL_EXPAND as the first item in the forward-linked
	 * list of expansion structures.  Flush memory to ensure the list is
	 * never broken.
	 */
	exp->next = page->expcol[slot];
	WT_MEMORY_FLUSH;
	page->expcol[slot] = exp;
	WT_PAGE_MODIFY_SET(page);
	/* Depend on workQ's memory flush before scheduling thread proceeds. */

	return (0);
}

/*
 * __wt_bt_rcc_expand_repl_serial_func --
 *	Server function to update a WT_REPL entry in an already expanded
 *	repeat-count compressed column store during a delete.
 */
int
__wt_bt_rcc_expand_repl_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_COL_EXPAND *exp;
	WT_REPL *repl;
	u_int16_t write_gen;

	__wt_bt_rcc_expand_repl_unpack(toc, page, write_gen, exp, repl);

	/* Check the page's write-generation, then update it. */
	WT_PAGE_WRITE_GEN_CHECK(page);

	/*
	 * Insert the new WT_REPL as the first item in the forward-linked list
	 * of replacement structures from the WT_COL_EXPAND structure.  Flush
	 * memory to ensure the list is never broken.
	 */
	repl->next = exp->repl;
	WT_MEMORY_FLUSH;
	exp->repl = repl;
	WT_PAGE_MODIFY_SET(page);
	/* Depend on workQ's memory flush before scheduling thread proceeds. */

	return (0);
}
