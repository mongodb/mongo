/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_page_disk_read --
 *	Read a file page.
 */
int
__wt_page_disk_read(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	DB *db;
	ENV *env;
	WT_FH *fh;
	off_t offset;
	uint32_t checksum;

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;


	WT_STAT_INCR(db->idb->stats, FILE_PAGE_READ);
	WT_STAT_INCR(env->ienv->cache->stats, CACHE_PAGE_READ);

	offset = WT_ADDR_TO_OFF(db, addr);
	WT_RET(__wt_read(env, fh, offset, size, dsk));

	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size)) {
		__wt_api_env_errx(env,
		    "read checksum error: addr/size %lu/%lu at offset %llu",
		    (u_long)addr, (u_long)size, (unsigned long long)offset);
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_page_write --
 *	Write a file page.
 */
inline int
__wt_page_write(WT_TOC *toc, WT_PAGE *page)
{
	return (__wt_page_disk_write(toc, page->dsk, page->addr, page->size));
}

/*
 * __wt_page_disk_write --
 *	Write a file page.
 */
int
__wt_page_disk_write(
    WT_TOC *toc, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	DB *db;
	ENV *env;
	WT_FH *fh;

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;


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

	WT_ASSERT(env, __wt_verify_dsk_page(toc, dsk, addr, size) == 0);

	WT_STAT_INCR(db->idb->stats, FILE_PAGE_WRITE);
	WT_STAT_INCR(env->ienv->cache->stats, CACHE_PAGE_WRITE);

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);

	return (__wt_write(env, fh, WT_ADDR_TO_OFF(db, addr), size, dsk));
}
