/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_entry_grow(WT_TOC *, u_int32_t, WT_CACHE_ENTRY **);
static int __wt_cache_read(WT_READ_REQ *);

/*
 * __wt_workq_read_server --
 *	See if the read server thread needs to be awakened.
 */
void
__wt_workq_read_server(ENV *env, int force)
{
	WT_CACHE *cache;
	u_int64_t bytes_inuse, bytes_max;

	cache = env->ienv->cache;

	/*
	 * If we're 10% over the maximum cache, shut out reads (which include
	 * page allocations) until we drain to at least 5% under the maximum
	 * cache.  The idea is that we don't want to run on the edge all the
	 * time -- if we're seriously out of space, get things under control
	 * before opening up for more reads.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = WT_STAT(cache->stats, CACHE_BYTES_MAX);
	if (cache->read_lockout) {
		if (bytes_inuse <= bytes_max - (bytes_max / 20))
			cache->read_lockout = 0;
	} else if (bytes_inuse > bytes_max + (bytes_max / 10)) {
		WT_VERBOSE(env, WT_VERB_SERVERS, (env,
		    "workQ locks out reads: bytes-inuse %llu of bytes-max %llu",
		    (u_quad)bytes_inuse, (u_quad)bytes_max));
		cache->read_lockout = 1;
	}

	/* If the cache read server is running, there's nothing to do. */
	if (!cache->read_sleeping)
		return;

	/*
	 * If reads are locked out and we're not forcing the issue (that's when
	 * closing the environment, or if there's a priority read waiting to be
	 * handled), we're done.
	 */
	if (!force && cache->read_lockout)
		return;

	cache->read_sleeping = 0;
	__wt_unlock(env, cache->mtx_read);
}

/*
 * __wt_cache_read_queue --
 *	Enter a new request into the cache read queue.
 */
int
__wt_cache_read_queue(
    WT_TOC *toc, u_int32_t *addrp, u_int32_t size, WT_PAGE **pagep)
{
	ENV *env;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;

	env = toc->env;
	cache = env->ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);
	for (; rr < rr_end; ++rr)
		if (WT_READ_REQ_ISEMPTY(rr)) {
			WT_READ_REQ_SET(rr, toc, addrp, size, pagep);
			return (0);
		}
	__wt_api_env_errx(env, "read server request table full");
	return (WT_RESTART);
}

/*
 * __wt_cache_read_server --
 *	Thread to do database reads.
 */
void *
__wt_cache_read_server(void *arg)
{
	ENV *env;
	IENV *ienv;
	WT_CACHE *cache;
	WT_READ_REQ *rr, *rr_end;
	WT_TOC *toc;
	int didwork, ret;

	env = arg;
	ienv = env->ienv;
	cache = ienv->cache;

	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);

	for (;;) {
		WT_VERBOSE(env,
		    WT_VERB_SERVERS, (env, "cache read server sleeping"));
		cache->read_sleeping = 1;
		__wt_lock(env, cache->mtx_read);
		WT_VERBOSE(
		    env, WT_VERB_SERVERS, (env, "cache read server waking"));

		/*
		 * Check for environment exit; do it here, instead of the top of
		 * the loop because doing it here keeps us from doing a bunch of
		 * worked when simply awakened to quit.
		 */
		if (!F_ISSET(ienv, WT_SERVER_RUN))
			break;

		/*
		 * Walk the read-request queue, looking for reads (defined by
		 * a valid WT_TOC handle.  If we find a read request, perform
		 * it, flush the result and clear the request slot, then wake
		 * up the requesting thread.  The request slot clear doesn't
		 * need to be flushed, but we have to flush the read result,
		 * might as well include it.  If we don't find any work, go to
		 * sleep.
		 */
		do {
			didwork = 0;
			for (rr = cache->read_request; rr < rr_end; ++rr) {
				if ((toc = rr->toc) == NULL)
					continue;
				if (cache->read_lockout &&
				    !F_ISSET(toc, WT_READ_PRIORITY))
					continue;

				/*
				 * The read server thread does both general file
				 * allocation and cache page instantiation.   In
				 * a file allocation, there's no pagep field in
				 * in which to return a page.
				 */
				ret = rr->pagep == NULL ?
				    __wt_cache_alloc(
					rr->toc, rr->addrp, rr->size) :
				    __wt_cache_read(rr);
				if (ret != 0)
					break;

				WT_READ_REQ_CLR(rr);
				__wt_toc_serialize_wrapup(toc, ret);

				didwork = 1;
			}
		} while (didwork);
	}

	if (ret != 0)
		__wt_api_env_err(env, ret, "cache read server error");

	WT_VERBOSE(env, WT_VERB_SERVERS, (env, "cache read server exiting"));
	return (NULL);
}

/*
 * __wt_cache_read --
 *	Read or allocate a new page for the cache.
 */
static int
__wt_cache_read(WT_READ_REQ *rr)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, *empty;
	WT_FH *fh;
	WT_PAGE *page;
	WT_TOC *toc;
	u_int32_t addr, addr_hash, size, i;
	int newpage, ret;

	toc = rr->toc;
	db = toc->db;
	env = toc->env;
	idb = db->idb;
	cache = env->ienv->cache;
	fh = idb->fh;
	ret = 0;

	addr = *rr->addrp;
	size = rr->size;
	newpage = addr == WT_ADDR_INVALID ? 1 : 0;

	/*
	 * Read a page from the file -- allocations end up here too, because
	 * we have to single-thread extending the file, and we can share page
	 * allocation and cache insertion code.
	 *
	 * Check to see if some other thread brought the page into the cache
	 * while we waited to run.   We don't care what state the page is in,
	 * we have to restart the operation since it appears in the cache.
	 * The obvious problem is the page is waiting to drain.   For all we
	 * know, the drain server will find a hazard reference and restore the
	 * page to the cache.
	 *
	 * While we're walking the hash bucket, keep track of any empty slots
	 * we find, we may need one.
	 */
	empty = NULL;
	if (!newpage) {
		for (i = WT_CACHE_ENTRY_CHUNK,
		    e = cache->hb[WT_ADDR_HASH(cache, addr)];;) {
			if (e->state == WT_EMPTY)
				empty = e;
			else if (e->db == db && e->addr == addr)
				return (WT_RESTART);

			WT_CACHE_ENTRY_NEXT(e, i);
		}
		WT_STAT_INCR(cache->stats, CACHE_MISS);
		WT_STAT_INCR(idb->stats, DB_CACHE_MISS);
	}

	/*
	 * The page isn't in the cache, and since we're the only path for the
	 * page to get into the cache, we don't have to worry further, and
	 * we might as well get to it.
	 *
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 */
	WT_RET(__wt_calloc(env, 1, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_calloc(env, (size_t)size, sizeof(u_int8_t), &page->hdr));

	/* If it's an allocation, extend the file; otherwise read the page. */
	if (newpage)
		WT_ERR(__wt_cache_alloc(toc, &addr, size));
	page->addr = addr;
	page->size = size;
	page->lru = ++cache->lru;
	if (!newpage)
		WT_ERR(__wt_page_read(db, page));

	/*
	 * If we found an empty slot in our original hash bucket walk, use it;
	 * otherwise, look for another one.  If there aren't any empty slots,
	 * grow the array.
	 */
	if (empty == NULL) {
		addr_hash = WT_ADDR_HASH(cache, addr);
		for (i = WT_CACHE_ENTRY_CHUNK, e = cache->hb[addr_hash];;) {
			if (e->state == WT_EMPTY) {
				empty = e;
				break;
			}
			WT_CACHE_ENTRY_NEXT(e, i);
		}
		if (empty == NULL)
			WT_ERR(__wt_cache_entry_grow(toc, addr_hash, &empty));
	}

	/*
	 * Get a hazard reference before we fill in the entry: the cache drain
	 * server should not pick a new page with a high read-generation, but
	 * it's theoretically possible.
	 */
	__wt_hazard_set(toc, NULL, page);
	WT_CACHE_ENTRY_SET(empty, db, addr, page, WT_OK);

	WT_VERBOSE(env,
	    WT_VERB_CACHE, (env, "cache %s addr %lu (element %p, page %p)",
	    newpage ? "allocated" : "read", (u_long)addr, empty, empty->page));

	WT_CACHE_PAGE_IN(cache, size);

	/* Return the page to the caller. */
	*rr->pagep = page;

	return (0);

err:	if (page != NULL) {
		if (page->hdr != NULL)
			__wt_free(env, page->hdr, size);
		__wt_free(env, page, sizeof(*page));
	}
	return (ret);
}

/*
 * __wt_cache_entry_grow --
 *	Grow the hash bucket's WT_CACHE_ENTRY array.
 */
static int
__wt_cache_entry_grow(WT_TOC *toc, u_int32_t bucket, WT_CACHE_ENTRY **emptyp)
{
	ENV *env;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e, *new;
	u_int32_t entries;

	env = toc->env;
	cache = env->ienv->cache;

	/* Allocate the new WT_ENTRY array. */
	WT_RET(__wt_calloc(env,
	    (size_t)WT_CACHE_ENTRY_CHUNK + 1, sizeof(WT_CACHE_ENTRY), &new));
	*emptyp = new;

	/*
	 * Find the end of the linked list of entries, and append the new
	 * array.
	 */
	for (entries = 0, e = cache->hb[bucket];;) {
		entries += WT_CACHE_ENTRY_CHUNK;
		if (e[WT_CACHE_ENTRY_CHUNK].db == NULL) {
			entries += WT_CACHE_ENTRY_CHUNK;
			e[WT_CACHE_ENTRY_CHUNK].db = (DB *)new;
			break;
		}
		e = (WT_CACHE_ENTRY *)e[WT_CACHE_ENTRY_CHUNK].db;
	}

	/* Make it real, it's ready to go. */
	WT_MEMORY_FLUSH;

	if (WT_STAT(cache->stats, CACHE_MAX_BUCKET_ENTRIES) < entries)
		WT_STAT_SET(cache->stats, CACHE_MAX_BUCKET_ENTRIES, entries);

	WT_VERBOSE(env, WT_VERB_CACHE, (env,
	    "growing cache hash bucket %lu to %lu",
	    (u_long)bucket, (u_long)entries));

	return (0);
}
