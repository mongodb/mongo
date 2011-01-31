/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
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
	WT_STATS *stats;
	off_t offset;
	uint32_t checksum;

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;
	stats = env->ienv->cache->stats;

	WT_STAT_INCR(stats, PAGE_READ);

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
	WT_STATS *stats;

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;
	stats = env->ienv->cache->stats;

	WT_ASSERT(env, __wt_verify_dsk_page(toc, dsk, addr, size) == 0);

	WT_STAT_INCR(stats, PAGE_WRITE);

	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, size);

	return (__wt_write(env, fh, WT_ADDR_TO_OFF(db, addr), size, dsk));
}
