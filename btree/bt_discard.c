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
 * __wt_db_page_alloc --
 *	Allocate and initialize a new in-memory Btree page.
 */
int
__wt_db_page_alloc(DB *db, WT_PAGE **ipp)
{
	IENV *ienv;
	WT_PAGE *ip;
	int ret;
	void *p;

	ienv = db->ienv;

	/*
	 * Allocate memory for the page and in-memory structures; put the page
	 * first so it's appropriately aligned for direct I/O, followed by the
	 * WT_PAGE structure.
	 */
	if ((ret = __wt_calloc(
	    ienv, 1, (size_t)db->pagesize + sizeof(WT_PAGE), &p)) != 0)
		return (ret);

	ip = (WT_PAGE*)((u_int8_t *)p + db->pagesize);

	/*
	 * We allocate a pointer for every 30 bytes (or, in other words, assume
	 * a newly created page contains roughly 30B key and data items.
	 */
	ip->indx_size = db->pagesize / 30;
	if ((ret = __wt_calloc(
	    ienv, (size_t)ip->indx_size, sizeof(u_int8_t *), &ip->indx)) != 0)
		goto err;

	ip->space_avail = db->pagesize - sizeof(WT_PAGE_HDR);
	ip->hdr = p;

	*ipp = ip;
	return (0);

err:	__wt_free(ienv, p);
	return (WT_ERROR);
}

/*
 * __wt_db_page_free --
 *	Free an in-memory Btree page.
 */
int
__wt_db_page_free(DB *db, WT_PAGE *ip)
{
	IENV *ienv;

	/*
	 * The page comes first in the memory chunk to get better alignment,
	 * so it's what we free.
	 */
	__wt_free(ienv, ip->hdr);
	return (0);
}
