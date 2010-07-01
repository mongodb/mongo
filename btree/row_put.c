/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_workq_repl(WT_TOC *, WT_SDBT **, void *, u_int32_t);

/*
 * __wt_db_row_del --
 *	Db.row_del method.
 */
int
__wt_db_row_del(WT_TOC *toc, DBT *key)
{
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	int ret;

	env = toc->env;
	idb = toc->db->idb;

	/* Make sure we have a spare replacement array in the WT_TOC. */
	if (toc->repl_spare == NULL)
		WT_RET(__wt_calloc(
		    env, WT_REPL_CHUNK + 1, sizeof(WT_SDBT), &toc->repl_spare));

	/* Search the btree for the key. */
	WT_RET(__wt_bt_search_row(toc, key, 0));
	page = toc->srch_page;

	/* Delete the item. */
	__wt_bt_delete_serial(toc, page, ret);

	if (page != NULL && page != idb->root_page)
		__wt_bt_page_out(toc, &page, ret == 0 ? WT_MODIFIED : 0);

	return (0);
}

/*
 * __wt_bt_del_serial_func --
 *	Server function to discard an entry.
 */
int
__wt_bt_del_serial_func(WT_TOC *toc)
{
	DB *db;
	WT_COL_INDX *cip;
	WT_PAGE *page;
	WT_ROW_INDX *rip;

	__wt_bt_delete_unpack(toc, page);
	db = toc->db;

	/*
	 * The entry we're updating is the last one pushed on the stack.
	 *
	 * If we need a new replacement array, check on that.
	 */
	switch (page->hdr->type) {
	case WT_PAGE_DUP_LEAF:
	case WT_PAGE_ROW_LEAF:
		rip = toc->srch_ip;
		__wt_workq_repl(toc, &rip->repl, WT_SDBT_DELETED_VALUE, 0);
		break;
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
		cip = toc->srch_ip;
		__wt_workq_repl(toc, &cip->repl, WT_SDBT_DELETED_VALUE, 0);
		break;
	WT_ILLEGAL_FORMAT(db);
	}
	return (0);
}

/*
 * __wt_workq_repl --
 *	Update a WT_{ROW,COL}_INDX replacement array.
 */
static void
__wt_workq_repl(WT_TOC *toc, WT_SDBT **replp, void *data, u_int32_t size)
{
	WT_SDBT *repl;

	/*
	 * The WorkQ thread updates the WT_{ROW,COL}_INDX replacement arrays,
	 * serializing changes or deletions of existing key/data items.
	 *
	 * The caller's WT_TOC structure has a cache, spare WT_SDBT array if we
	 * need one.  Update the replacement entry before making any new
	 * replacement array visible to anyone, the rest of the code depends on
	 * there being a replacement item if the replacement array exists.
	 *
	 * First, check if we're entering in a new replacment array -- if we
	 * are, enter the information into the new replacement array (including
	 * linking it into the list of replacement arrays), flush memory, and
	 * then update the WT_{ROW,COL}_INDX's replacement array reference.
	 */
	if ((repl = *replp) == NULL || repl->data != NULL) {
		repl = toc->repl_spare;
		repl[WT_REPL_CHUNK - 1].data = data;
		repl[WT_REPL_CHUNK - 1].size = size;
		repl[WT_REPL_CHUNK].data = *replp;
		WT_MEMORY_FLUSH;
		*replp = repl;
		toc->repl_spare = NULL;
	} else {
		/*
		 * There's an existing replacement array with at least one empty
		 * slot: find the first available empty slot and update it.
		 */
		for (; repl[1].data == NULL; ++repl)
			;
		repl->data = data;
		repl->size = size;
	}
	WT_MEMORY_FLUSH;
}
