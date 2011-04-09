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
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
	WT_FH *fh;
	off_t offset;
	uint32_t checksum;

	btree = session->btree;
	fh = btree->fh;

	WT_STAT_INCR(btree->stats, page_read);
	WT_STAT_INCR(S2C(session)->cache->stats, cache_page_read);

	offset = WT_ADDR_TO_OFF(btree, addr);
	WT_RET(__wt_read(session, fh, offset, size, dsk));

	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size)) {
		__wt_err(session, 0,
		    "read checksum error: addr/size %lu/%lu at offset %llu",
		    (u_long)addr, (u_long)size, (unsigned long long)offset);
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_disk_write --
 *	Write a file page.
 */
int
__wt_disk_write(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
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
	dsk->size = size;

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);

	WT_ASSERT(session, __wt_verify_dsk_page(session, dsk, addr, size) == 0);

	WT_STAT_INCR(btree->stats, page_write);
	WT_STAT_INCR(S2C(session)->cache->stats, cache_page_write);

	return (
	    __wt_write(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));
}
