/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_search_col --
 *	Search a column-store tree for a specific record-based key.
 */
int
__wt_bt_search_col(WT_TOC *toc, u_int64_t recno)
{
	IDB *idb;
	WT_COL_INDX *ip;
	WT_PAGE *page;
	WT_SDBT *rdbt;
	u_int64_t record_cnt;
	u_int32_t addr, i;
	int isleaf, ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;

	idb = toc->db->idb;

	/* Check for a record past the end of the database. */
	if (idb->root_page->records < recno)
		return (WT_NOTFOUND);

	if ((page = idb->root_page) == NULL)
		return (WT_NOTFOUND);
	isleaf = page->hdr->type == WT_PAGE_COL_VAR ? 1 : 0;

	/* Search the tree. */
	for (record_cnt = 0;;) {
		/* If it's a leaf page, return the page and index. */
		if (isleaf) {
			ip = page->u.c_indx + ((recno - record_cnt) - 1);
			break;
		}

		/* Walk the page, counting records. */
		WT_INDX_FOREACH(page, ip, i) {
			if (record_cnt + WT_COL_OFF_RECORDS(ip) >= recno)
				break;
			record_cnt += WT_COL_OFF_RECORDS(ip);
		}

		/* ip references the subtree containing the record. */
		addr = WT_COL_OFF_ADDR(ip);
		isleaf = F_ISSET(page->hdr, WT_OFFPAGE_REF_LEAF) ? 1 : 0;

		/* We're done with the page. */
		if (page != idb->root_page)
			WT_RET(__wt_bt_page_out(toc, page, 0));

		/* Get the next page. */
		WT_RET(__wt_bt_page_in(toc, addr, isleaf, 1, &page));
	}

	/* Check for deleted items. */
	if ((rdbt =
	    WT_REPL_CURRENT(ip)) != NULL && rdbt->data == WT_DATA_DELETED) {
		ret = WT_NOTFOUND;
		goto err;
	}

	toc->srch_page = page;
	toc->srch_ip = ip;
	return (0);

err:	if (page != idb->root_page)
		WT_RET(__wt_bt_page_out(toc, page, 0));
}
