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
 * __wt_cache_in_serial_func --
 *	Server function to read bytes from a file.
 */
int
__wt_cache_in_serial_func(WT_TOC *toc)
{
	DB *db;
	IENV *ienv;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_PAGE **hb, *page, *tp;
	u_int32_t addr, bytes;

	__wt_cache_in_unpack(toc, page, addr, bytes);

	db = toc->db;
	ienv = toc->env->ienv;
	cache = &ienv->cache;

	/*
	 * If we're reading a page into memory, check to see if some other
	 * thread brought the page in while we were waiting to run.
	 */
	if (addr != WT_ADDR_INVALID)
		for (tp =
		    cache->hb[WT_HASH(cache, addr)]; tp != NULL; tp = tp->next)
			if (tp->addr == addr && tp->db == db)
				return (WT_RESTART);

	/* Initialize the page structure. */
	page->db = db;
	if (addr == WT_ADDR_INVALID) {
		fh = db->idb->fh;
		addr = WT_OFF_TO_ADDR(db, fh->file_size);
		fh->file_size += bytes;
	}
	page->addr = addr;
	page->bytes = bytes;
	page->page_gen = ++ienv->page_gen;

	/*
	 * Add the page to the WT_TOC's hazard list and flush the write; we
	 * don't need to do any further checking, no possible cache walk can
	 * find the page without also finding our hazard reference.
	 */
	__wt_hazard_set(toc, page);

	/*
	 * Insert as the head of the linked list.  The insert has to be thread
	 * safe, that is, other threads may be walking the linked list at the
	 * same time we're doing the insert.
	 */
	hb = &cache->hb[WT_HASH(cache, addr)];
	page->next = *hb;
	*hb = page;

	/* Increment total cache byte and page count. */
	WT_STAT_INCR(ienv->stats, CACHE_PAGES);
	WT_STAT_INCRV(ienv->stats, CACHE_BYTES_INUSE, bytes);

	return (0);
}

/*
 * __wt_cache_write --
 *	Write a page to the backing database file.
 */
int
__wt_cache_write(ENV *env, WT_PAGE *page)
{
	DB *db;
	WT_PAGE_HDR *hdr;

	db = page->db;

	/* Update the checksum. */
	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->bytes);

	/* Write, and if successful, clear the modified flag. */
	WT_RET(__wt_write(env, db->idb->fh,
	    WT_ADDR_TO_OFF(page->db, page->addr), page->bytes, hdr));

	F_CLR(page, WT_MODIFIED);

	WT_STAT_INCR(env->ienv->stats, CACHE_WRITE);

	return (0);
}
