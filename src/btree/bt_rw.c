/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_disk_read --
 *	Read a file page.
 */
int
__wt_disk_read(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;
	uint32_t checksum;

	btree = session->btree;
	fh = btree->fh;

	WT_RET(__wt_read(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));

	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size))
		WT_FAILURE_RET(session, WT_ERROR,
		    "read checksum error: %" PRIu32 "/%" PRIu32, addr, size);

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, cache_page_read);

	WT_VERBOSE(session, READ,
	    "read addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));

	return (0);
}

/*
 * __wt_disk_write --
 *	Write a file page.
 */
int
__wt_disk_write(
    WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_FH *fh;

	btree = session->btree;
	fh = btree->fh;

	/*
	 * The disk write function sets a few things in the WT_PAGE_DISK header
	 * simply because it's easy to do it here.  In a transactional store,
	 * things may be a little harder.
	 *
	 * We increment the page LSN in non-transactional stores so it's easy
	 * to identify newer versions of pages during salvage: both pages are
	 * likely to be internally consistent, and might have the same initial
	 * and last keys, so we need a way to know the most recent state of the
	 * page.  Alternatively, we could check to see which leaf is referenced
	 * by the internal page, which implies salvaging internal pages (which
	 * I don't want to do), and it's not quite as good anyway, because the
	 * internal page may not have been written to disk after the leaf page
	 * was updated.
	 */
	WT_LSN_INCR(btree->lsn);
	dsk->lsn = btree->lsn;
	dsk->size = dsk->memsize = size;

	WT_ASSERT(session, __wt_verify_dsk(session, dsk, addr, size, 0) == 0);

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);
	WT_RET(
	    __wt_write(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, cache_page_write);

	WT_VERBOSE(session, WRITE,
	    "write addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));
	return (0);
}
