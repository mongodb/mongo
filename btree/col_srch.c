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
	WT_COL_INDX *cip;
	WT_PAGE *page;
	WT_SDBT *sdbt;
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
		/* Walk the page looking for the record. */
		switch (page->hdr->type) {
		case WT_PAGE_COL_FIX:
			if (F_ISSET(idb, WT_REPEAT_COMP)) {
				WT_INDX_FOREACH(page, cip, i) {
					record_cnt +=
					    WT_FIX_REPEAT_COUNT(cip->data);
					if (record_cnt >= recno)
						break;
				}
				goto done;
			}
			/* FALLTHROUGH */
		case WT_PAGE_COL_VAR:
			cip = page->u.c_indx + ((recno - record_cnt) - 1);
			goto done;
		default:
			/* Walk the page, counting records. */
			WT_INDX_FOREACH(page, cip, i) {
				if (record_cnt +
				    WT_COL_OFF_RECORDS(cip) >= recno)
					break;
				record_cnt += WT_COL_OFF_RECORDS(cip);
			}
			break;
		}

		/* cip references the subtree containing the record. */
		addr = WT_COL_OFF_ADDR(cip);
		size = WT_COL_OFF_SIZE(cip);

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

done:
	/* Check for deleted items. */
	WT_REPL_CURRENT_SET(cip, sdbt);
	if (sdbt == NULL) {
		if (cip->data == NULL) {
			ret = WT_NOTFOUND;
			goto err;
		}
	} else if (WT_SDBT_DELETED_ISSET(sdbt->data)) {
		ret = WT_NOTFOUND;
		goto err;
	}

	toc->srch_page = page;
	toc->srch_ip = cip;
	return (0);

err:	if (page != idb->root_page)
		__wt_bt_page_out(toc, &page, 0);
	return (ret);
}
