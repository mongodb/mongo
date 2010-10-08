/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_in_serial_func(WT_TOC *);

/*
 * __wt_page_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_page_alloc(WT_TOC *toc, u_int32_t size, WT_PAGE **pagep)
{
	ENV *env;
	int ret;

	*pagep = NULL;
	env = toc->env;

	WT_ASSERT(env, size % WT_FRAGMENT == 0);

	__wt_cache_in_serial(toc, WT_ADDR_INVALID, size, pagep, ret);
	return (ret);
}

/*
 * __wt_page_in --
 *	Return a database page, reading as necessary.
 */
int
__wt_page_in(WT_TOC *toc,
    u_int32_t addr, u_int32_t size, WT_PAGE **pagep, u_int32_t flags)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_PAGE *page;
	u_int32_t i;
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
	__wt_cache_in_serial(toc, addr, size, pagep, ret);
	if (ret == WT_RESTART)
		WT_STAT_INCR(cache->stats, CACHE_READ_RESTARTS);
	return (ret);
}

/*
 * __wt_cache_in_serial_func --
 *	Read/allocation serialization function called when a page-in requires
 *	allocation or a read.
 */
static int
__wt_cache_in_serial_func(WT_TOC *toc)
{
	ENV *env;
	WT_CACHE *cache;
	WT_PAGE **pagep;
	WT_READ_REQ *rr, *rr_end;
	u_int32_t addr, size;

	__wt_cache_in_unpack(toc, addr, size, pagep);

	env = toc->env;
	cache = env->ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);
	for (; rr < rr_end; ++rr)
		if (rr->toc == NULL) {
			WT_READ_REQ_SET(rr, toc, addr, size, pagep);
			return (0);
		}
	__wt_api_env_errx(env, "read server request table full");
	return (WT_RESTART);
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
	u_int32_t checksum;

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
		    (u_long)page->addr, (u_long)page->size, (u_quad)offset);
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
