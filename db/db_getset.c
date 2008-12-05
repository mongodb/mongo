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
 * __wt_db_set_pagesize_verify --
 *	Verify an argument to the Db.set_pagesize setter.
 */
int
__wt_db_set_pagesize_verify(DB *db, u_int32_t pagesize,
    u_int32_t fragsize, u_int32_t extentsize, u_int32_t maxitemsize)
{
	/* Copy in defaults, if not being set. */
	if (pagesize == 0)
		pagesize = db->pagesize;
	if (fragsize == 0)
		fragsize = db->fragsize;
	if (extentsize == 0)
		extentsize = db->extentsize;
	if (maxitemsize == 0) {
		maxitemsize = db->maxitemsize;
		if (maxitemsize == 0)
			maxitemsize = WT_DATA_SPACE(pagesize) / 4;
	}

	if (fragsize % WT_FRAG_MINIMUM_SIZE != 0) {
		__wt_db_errx(db,
		    "The fragment size must be a multiple of 512B");
		return (WT_ERROR);
	}
	if (pagesize % fragsize != 0) {
		__wt_db_errx(db,
		    "The page size must be a multiple of the fragment size");
		return (WT_ERROR);
	}
	if (extentsize % pagesize != 0) {
		__wt_db_errx(db,
		    "The extent size must be a multiple of the page size");
		return (WT_ERROR);
	}

	/*
	 * The page must hold at least 4 keys, otherwise the whole Btree
	 * thing breaks down because we can't split.
	 */
	if (maxitemsize > WT_DATA_SPACE(pagesize) / 4) {
		__wt_db_errx(db,
		    "The specified page size is too small for the current"
		    " maximum item size; at least two key/data pairs must"
		    " fit on each page.  The current maximum item size is"
		    " %lu, making the minimum maximum page size %lu",
		    (u_long)maxitemsize, (u_long)maxitemsize * 4);
		return (WT_ERROR);
	}

	db->maxitemsize = maxitemsize;
	db->fragsize = fragsize;
	db->pagesize = pagesize;
	db->extentsize = extentsize;
	db->frags_per_page = pagesize / fragsize;

	return (0);
}
