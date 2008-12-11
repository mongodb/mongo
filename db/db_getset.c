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
 * __wt_db_set_btree_compare_int_verify --
 *	Verify an argument to the Db.set_btree_compare_int setter.
 */
int
__wt_db_set_btree_compare_int_verify(DB *db, int *bytesp)
{
	int bytes;

	bytes = *bytesp;
	if (bytes >= 0 && bytes <= 8) {
		db->btree_compare = __wt_btree_compare_int;
		return (0);
	}

	__wt_db_errx(db,
	    "The number of bytes must be an integral value between 1 and 8");
	return (WT_ERROR);
}

/*
 * __wt_db_set_pagesize_verify --
 *	Verify an argument to the Db.set_pagesize setter.
 */
int
__wt_db_set_pagesize_verify(DB *db, u_int32_t *pagesizep,
    u_int32_t *fragsizep, u_int32_t *extentsizep, u_int32_t *maxitemsizep)
{
	u_int32_t pagesize, fragsize, extentsize, maxitemsize;

	/* Copy in defaults, if not being set. */
	pagesize = *pagesizep == 0 ? db->pagesize : *pagesizep;
	fragsize = *fragsizep == 0 ? db->fragsize : *fragsizep;
	extentsize = *extentsizep == 0 ? db->extentsize : *extentsizep;
	maxitemsize = *maxitemsizep == 0 ? db->maxitemsize : *maxitemsizep;
	if ((maxitemsize = *maxitemsizep) == 0)
		if ((maxitemsize = db->maxitemsize) == 0)
			maxitemsize = WT_DATA_SPACE(pagesize) / 4;

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

	*pagesizep = pagesize;
	*fragsizep = fragsize;
	*extentsizep = extentsize;
	*maxitemsizep = maxitemsize;

	return (0);
}
