/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_row_update(WT_TOC *, DBT *, DBT *, int);

/*
 * __wt_db_row_put --
 *	Db.row_put method.
 */
inline int
__wt_db_row_put(WT_TOC *toc, DBT *key, DBT *data)
{
	return (__wt_db_row_update(toc, key, data, 1));
}

/*
 * __wt_db_row_del --
 *	Db.row_del method.
 */
inline int
__wt_db_row_del(WT_TOC *toc, DBT *key)
{
	return (__wt_db_row_update(toc, key, NULL, 0));
}

/*
 * __wt_db_row_update --
 *	Row store delete and update.
 */
static int
__wt_db_row_update(WT_TOC *toc, DBT *key, DBT *data, int insert)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	int ret;

	env = toc->env;
	idb = toc->db->idb;
	new_repl = NULL;
	repl = NULL;

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_row(toc, key, insert ? WT_INSERT : 0));
	page = toc->srch_page;

	/* Allocate a page replacement array as necessary. */
	if (page->repl == NULL)
		WT_ERR(__wt_calloc(
		    env, page->indx_count, sizeof(WT_REPL *), &new_repl));

	/* Allocate a WT_REPL structure and fill it in. */
	WT_ERR(__wt_update_alloc(
	    toc, sizeof(WT_REPL) + (data == NULL ? 0 : data->size), &repl));
	if (data == NULL)
		repl->data = WT_REPL_DELETED_VALUE;
	else {
		repl->data = (u_int8_t *)repl + sizeof(WT_REPL);
		repl->size = data->size;
		memcpy(repl->data, data->data, data->size);
	}

	/* Schedule the workQ to insert the WT_REPL structure. */
	__wt_bt_update_serial(toc, page, toc->srch_write_gen,
	    WT_ROW_SLOT(page, toc->srch_ip), new_repl, repl, ret);

err:	/* Free any replacement array unless the workQ used it. */
	if (new_repl != NULL && new_repl != page->repl)
		__wt_free(env, new_repl, page->indx_count * sizeof(WT_REPL *));

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}

/*
 * __wt_bt_update_serial_func --
 *	Server function to update a WT_REPL entry in the modification array.
 */
int
__wt_bt_update_serial_func(WT_TOC *toc)
{
	WT_PAGE *page;
	WT_REPL **new_repl, *repl;
	u_int16_t write_gen;
	int slot;

	__wt_bt_update_unpack(toc, page, write_gen, slot, new_repl, repl);

	/* Check the page's write-generation, then update it. */
	WT_PAGE_WRITE_GEN_CHECK(page);

	/*
	 * If the page does not yet have a replacement array, our caller passed
	 * us one of the correct size.   (It's the caller's responsibility to
	 * detect & free the passed-in expansion array if we don't use it.)
	 */
	if (page->repl == NULL)
		page->repl = new_repl;

	/*
	 * Insert the new WT_REPL as the first item in the forward-linked list
	 * of replacement structures.  Flush memory to ensure the list is never
	 * broken.
	 */
	repl->next = page->repl[slot];
	WT_MEMORY_FLUSH;
	page->repl[slot] = repl;
	WT_PAGE_MODIFY_SET(page);
	/* Depend on workQ's memory flush before scheduling thread proceeds. */

	return (0);
}
