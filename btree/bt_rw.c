/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_read_serial_func(WT_TOC *);

/*
 * __wt_page_alloc --
 *	Allocate bytes from a file and associate it with a WT_PAGE structure.
 */
int
__wt_page_alloc(WT_TOC *toc, uint32_t size, WT_PAGE **pagep)
{
	ENV *env;
	uint32_t addr;
	int ret;

	*pagep = NULL;
	env = toc->env;

	WT_ASSERT(env, size % WT_FRAGMENT == 0);

	addr = WT_ADDR_INVALID;
	__wt_cache_read_serial(toc, &addr, size, pagep, ret);
	return (ret);
}

/*
 * __wt_page_in --
 *	Return a database page, reading as necessary.
 */
int
__wt_page_in(WT_TOC *toc,
    uint32_t addr, uint32_t size, WT_PAGE **pagep, uint32_t flags)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_PAGE *page;
	uint32_t i;
	int found, ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	cache = env->ienv->cache;

	WT_ASSERT(env, size % WT_FRAGMENT == 0);
	WT_ENV_FCHK_ASSERT(env, "__wt_page_in", flags, WT_APIMASK_WT_PAGE_IN);

	/* Search the cache for the page. */
	found = ret = 0;
	for (i = WT_CACHE_ENTRY_CHUNK,
	    e = cache->hb[WT_ADDR_HASH(cache, addr)];;) {
		if (e->db == db && e->addr == addr &&
		    (e->state == WT_OK ||
		    (e->state == WT_DRAIN && F_ISSET(toc, WT_READ_DRAIN)))) {
			found = 1;
			break;
		}
		WT_CACHE_ENTRY_NEXT(e, i);
	}

	/* Get a hazard reference -- if that fails, the page isn't available. */
	if (found && __wt_hazard_set(toc, e, NULL)) {
		/* Update the generation number. */
		page = e->page;
		page->lru = ++cache->lru;
		*pagep = page;

		WT_STAT_INCR(idb->stats, DB_CACHE_HIT);
		WT_STAT_INCR(cache->stats, CACHE_HIT);
		return (0);
	}

	/* Optionally, only return in-cache entries. */
	if (LF_ISSET(WT_CACHE_ONLY))
		return (WT_NOTFOUND);

	/*
	 * If for any reason, we can't get the page we want, ask the read server
	 * to get it for us and go to sleep.  The read server is expensive, but
	 * serializes all the hard cases.
	 */
	__wt_cache_read_serial(toc, &addr, size, pagep, ret);
	if (ret == WT_RESTART)
		WT_STAT_INCR(cache->stats, CACHE_READ_RESTARTS);
	return (ret);
}

/*
 * __wt_cache_read_serial_func --
 *	Read/allocation serialization function called when a page-in requires
 *	allocation or a read.
 */
static int
__wt_cache_read_serial_func(WT_TOC *toc)
{
	WT_PAGE **pagep;
	uint32_t *addrp, size;

	__wt_cache_read_unpack(toc, addrp, size, pagep);

	return (__wt_cache_read_queue(toc, addrp, size, pagep));
}

/*
 * __wt_page_out --
 *	Return a page to the cache.
 */
void
__wt_page_out(WT_TOC *toc, WT_PAGE *page)
{
	__wt_hazard_clear(toc, page);
}

/*
 * __wt_page_read --
 *	Read a database page (same as read, but verify the checksum).
 */
int
__wt_page_read(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_FH *fh;
	WT_PAGE_HDR *hdr;
	off_t offset;
	uint32_t checksum;

	env = db->env;
	fh = db->idb->fh;
	hdr = page->hdr;

	offset = WT_ADDR_TO_OFF(db, page->addr);
	WT_RET(__wt_read(env, fh, offset, page->size, hdr));

	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, page->size)) {
		__wt_api_env_errx(env,
		    "read checksum error: addr/size %lu/%lu at offset %llu",
		    (u_long)page->addr,
		    (u_long)page->size, (unsigned long long)offset);
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_page_write --
 *	Write a database page.
 */
int
__wt_page_write(DB *db, WT_PAGE *page)
{
	ENV *env;
	WT_FH *fh;
	WT_PAGE_HDR *hdr;

	env = db->env;
	fh = db->idb->fh;

	hdr = page->hdr;
	hdr->checksum = 0;
	hdr->checksum = __wt_cksum(hdr, page->size);

	return (__wt_write(
	    env, fh, WT_ADDR_TO_OFF(db, page->addr), page->size, hdr));
}
