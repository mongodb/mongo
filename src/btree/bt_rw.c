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

	WT_STAT_INCR(btree->stats, FILE_PAGE_READ);
	WT_STAT_INCR(S2C(session)->cache->stats, CACHE_PAGE_READ);

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
	if (dsk->lsn_off == UINT32_MAX) {
		++dsk->lsn_file;
		dsk->lsn_off = 0;
	} else
		++dsk->lsn_off;

	WT_ASSERT(session, __wt_verify_dsk_page(session, dsk, addr, size) == 0);

	WT_STAT_INCR(btree->stats, FILE_PAGE_WRITE);
	WT_STAT_INCR(S2C(session)->cache->stats, CACHE_PAGE_WRITE);

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);

	return (__wt_write(session, fh, WT_ADDR_TO_OFF(btree, addr), size, dsk));
}
