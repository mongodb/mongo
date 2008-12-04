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
 * __wt_db_set_maxitemsize_verify --
 *	Verify an argument to the Db.set_maxitemsize setter.
 */
int
__wt_db_set_maxitemsize_verify(DB *db, u_int32_t maxitemsize)
{
	/*
	 * The page must hold at least 4 keys, otherwise the whole Btree
	 * thing breaks down because we can't split.
	 */
	if (maxitemsize > WT_DATA_SPACE(db->pagesize) / 4) {
		__wt_db_errx(db,
		    "The specified item size is too large for the current"
		    " database page size; at least two key/data pairs must"
		    " fit on each page.  The current page size is %lu,"
		    " making the  minimum maximum item size %lu.",
		    (u_long)db->pagesize,
		    (u_long)WT_DATA_SPACE(db->pagesize) / 4);
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_db_set_pagesize_verify --
 *	Verify an argument to the Db.set_pagesize setter.
 */
int
__wt_db_set_pagesize_verify(DB *db, u_int32_t pagesize, u_int32_t extentsize)
{
	/*
	 * If the maximum key size hasn't been set, that is, it's still
	 * a default, reset it to something based on the new page size.
	 */
	if (F_ISSET(db, WT_MAXKEY_NOTSET)) {
		db->maxitemsize = WT_DATA_SPACE(pagesize) / 4;
		return (0);
	}

	/*
	 * The page must hold at least 4 keys, otherwise the whole Btree
	 * thing breaks down because we can't split.
	 */
	if (db->maxitemsize > WT_DATA_SPACE(pagesize) / 4) {
		__wt_db_errx(db,
		    "The specified page size is too small for the current"
		    " maximum item size; at least two key/data pairs must"
		    " fit on each page.  The current maximum item size is"
		    " %lu, making the minimum maximum page size %lu",
		    (u_long)db->maxitemsize, (u_long)db->maxitemsize * 4);
		return (WT_ERROR);
	}

	if (pagesize < 512 || pagesize % 512 != 0) {
		__wt_db_errx(db,
		    "The specified page size is illegal; the page size must"
		    " be a multiple of 512 bytes.");
		return (WT_ERROR);
	}

	if (extentsize < 512 || extentsize % 512 != 0) {
		__wt_db_errx(db,
		    "The specified page size is illegal; the page size must"
		    " be a multiple of 512 bytes.");
		return (WT_ERROR);
	}

	return (0);
}
