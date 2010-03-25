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
 * __wt_cache_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_cache_alloc(WT_TOC *toc, u_int32_t bytes, WT_PAGE **pagep)
{
	ENV *env;

	*pagep = NULL;
	env = toc->env;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/* Schedule the allocation and sleep until it's completed. */
	WT_RET(__wt_toc_serialize_io(toc, WT_ADDR_INVALID, bytes));

	/* Then go get the page. */
	WT_RET(__wt_cache_in(toc, toc->wq_addr, toc->wq_bytes, pagep));

	return (0);
}

/*
 * __wt_cache_in --
 *	Return a database page, reading as necessary.
 */
int
__wt_cache_in(WT_TOC *toc, u_int32_t addr, u_int32_t bytes, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_CACHE_HB *hb;
	u_int32_t i;
	int found;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = ienv->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

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
	 * state field, which can be reset from WT_OK to WT_CACHE_DRAIN at any
	 * time by the cache server.
	 *
	 * Add the page to the WT_TOC's hazard list (which flushes the write),
	 * then see if the state field is still WT_OK.  If it's still WT_OK,
	 * we know we can use the page because the cache drain server will see
	 * our hazard reference before it discards the buffer (the drain server
	 * sets the WT_CACHE_DRAIN state, flushes memory, and then checks the
	 * the hazard references).
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
			e->gen = ++ienv->page_gen;
			F_CLR(e->page, WT_DISCARD);
			*pagep = e->page;

			WT_STAT_INCR(ienv->stats, CACHE_HIT);
			WT_STAT_INCR(idb->stats, DB_CACHE_HIT);
			return (0);
		}
		__wt_hazard_clear(toc, e->page);
	}

	/*
	 * If the caller doesn't know how big the page is, the caller is only
	 * looking for a cache hit -- fail, we can't read the page.  (This is
	 * the path we take when opening a new database where the root page
	 * might be in the cache as a result of a bulk load.  Yeah, obscure,
	 * but currently necessary.
	 */
	if (bytes == 0)
		return (0);

	WT_RET(__wt_toc_serialize_io(toc, addr, bytes));
	goto retry;
}

/*
 * __wt_cache_out --
 *	Discard a reference to a database page.
 */
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	/*
	 * If the page has been modified or flagged as useless, set the
	 * local flag.
	 */
	if (LF_ISSET(WT_DISCARD | WT_MODIFIED))
		F_SET(page, LF_ISSET(WT_DISCARD | WT_MODIFIED));

	__wt_hazard_clear(toc, page);

	return (0);
}
