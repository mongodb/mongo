/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
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
__wt_page_in(WT_TOC *toc, u_int32_t addr, u_int32_t size, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_CACHE_HB *hb;
	u_int32_t i;
	int found, ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = ienv->cache;

	WT_ASSERT(env, size % WT_FRAGMENT == 0);

retry:	/* Search the cache for the page. */
	found = 0;
	hb = &cache->hb[WT_HASH(cache, addr)];
	WT_CACHE_FOREACH_PAGE(cache, hb, e, i)
		if (e->addr == addr && e->db == db && e->state == WT_OK) {
			found = 1;
			break;
		}

	/*
	 * The memory location making this page "real" is the WT_CACHE_ENTRY's
	 * state field, which can be reset from WT_OK to WT_DRAIN at any time
	 * by the cache server.
	 *
	 * Add the page to the WT_TOC's hazard list (which flushes the write),
	 * then see if the state field is still WT_OK.  If it's still WT_OK,
	 * we know we can use the page because the cache drain server will see
	 * our hazard reference before it discards the buffer (the drain server
	 * sets the WT_DRAIN state, flushes memory, and then checks the hazard
	 * references).
	 *
	 * If for any reason, we can't get the page we want, ask the I/O server
	 * to get it for us and go to sleep.  The I/O server is expensive, but
	 * serializes all the hard cases.
	 */
	if (found) {
		__wt_hazard_set(toc, e->page);
		if (e->state == WT_OK) {
			/*
			 * Update the generation number and clear any discard
			 * flag, it's clearly wrong.
			 */
			e->read_gen = ++ienv->read_gen;
			F_CLR(e->page, WT_DISCARD);
			*pagep = e->page;

			WT_STAT_INCR(idb->stats, DB_CACHE_HIT);
			WT_STAT_INCR(cache->stats, CACHE_HIT);
			return (0);
		}
		__wt_hazard_clear(toc, e->page);
	}

	__wt_cache_in_serial(toc, addr, size, pagep, ret);
	if (ret == WT_RESTART) {
		WT_STAT_INCR(cache->stats, CACHE_READ_RESTARTS);
		goto retry;
	}
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
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE **pagep;
	WT_READ_REQ *rr, *rr_end;
	u_int32_t addr, size;

	__wt_cache_in_unpack(toc, addr, size, pagep);

	env = toc->env;
	ienv = env->ienv;
	cache = ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end =
	    rr + sizeof(cache->read_request) / sizeof(cache->read_request[0]);
	for (; rr < rr_end; ++rr)
		if (rr->toc == NULL) {
			/*
			 * Fill in the arguments, flush memory, then the WT_TOC
			 * field that turns the slot on.
			 */
			rr->addr = addr;
			rr->size = size;
			rr->entry = NULL;
			rr->pagep = pagep;
			WT_MEMORY_FLUSH;
			rr->toc = toc;
			WT_MEMORY_FLUSH;
			return (0);
		}
	__wt_api_env_errx(env, "cache server read request table full");
	return (WT_RESTART);
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
