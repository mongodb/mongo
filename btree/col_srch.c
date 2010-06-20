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
	DB *db;
	IDB *idb;
	WT_COL_INDX *ip;
	WT_PAGE *page;
	WT_SDBT *rdbt;
	u_int64_t record_cnt;
	u_int32_t addr, size, i;
	int ret;

	toc->srch_page = NULL;			/* Return values. */
	toc->srch_ip = NULL;

	db = toc->db;
	idb = db->idb;

restart:
	/* Check for a record past the end of the database. */
	page = idb->root_page;
	if (page->records < recno)
		return (WT_NOTFOUND);

	/* Search the tree. */
	for (record_cnt = 0;;) {
		/* If it's a leaf page, return the page and index. */
		if (page->hdr->type == WT_PAGE_COL_FIX ||
		    page->hdr->type == WT_PAGE_COL_VAR) {
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
		size = WT_COL_OFF_SIZE(ip);

		/* Walk down to the next page. */
		if (page != idb->root_page)
			__wt_bt_page_out(toc, &page, 0);
		switch (ret = __wt_bt_page_in(toc, addr, size, 1, &page)) {
		case 0:
			break;
		case WT_RESTART:
			goto restart;
		default:
			return (ret);
		}
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
		__wt_bt_page_out(toc, &page, 0);
	return (ret);
}
