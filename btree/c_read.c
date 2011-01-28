/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_cache_read(WT_READ_REQ *);

/*
 * __wt_workq_read_server --
 *	See if the read server thread needs to be awakened.
 */
void
__wt_workq_read_server(ENV *env, int force)
{
	WT_CACHE *cache;
	uint64_t bytes_inuse, bytes_max;

	cache = env->ienv->cache;

	/*
	 * If we're 10% over the maximum cache, shut out reads (which include
	 * page allocations) until we evict to at least 5% under the maximum
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
		WT_VERBOSE(env, WT_VERB_READ, (env,
		    "workQ locks out reads: bytes-inuse %llu of bytes-max %llu",
		    (unsigned long long)bytes_inuse,
		    (unsigned long long)bytes_max));
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
 * __wt_cache_read_serial_func --
 *	Read/allocation serialization function called when a page-in requires
 *	allocation or a read.
 */
int
__wt_cache_read_serial_func(WT_TOC *toc)
{
	ENV *env;
	WT_CACHE *cache;
	WT_OFF *off;
	WT_READ_REQ *rr, *rr_end;
	WT_REF *ref;
	int dsk_verify;

	__wt_cache_read_unpack(toc, ref, off, dsk_verify);

	env = toc->env;
	cache = env->ienv->cache;

	/* Find an empty slot and enter the read request. */
	rr = cache->read_request;
	rr_end = rr + WT_ELEMENTS(cache->read_request);
	for (; rr < rr_end; ++rr)
		if (WT_READ_REQ_ISEMPTY(rr)) {
			WT_READ_REQ_SET(rr, toc, ref, off, dsk_verify);
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
		    WT_VERB_READ, (env, "cache read server sleeping"));
		cache->read_sleeping = 1;
		__wt_lock(env, cache->mtx_read);
		WT_VERBOSE(
		    env, WT_VERB_READ, (env, "cache read server waking"));

		/*
		 * Check for environment exit; do it here, instead of the top of
		 * the loop because doing it here keeps us from doing a bunch of
		 * worked when simply awakened to quit.
		 */
		if (!F_ISSET(ienv, WT_SERVER_RUN))
			break;

		/*
		 * Walk the read-request queue, looking for reads (defined by
		 * a valid WT_TOC handle).  If we find a read request, perform
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
				ret = __wt_cache_read(rr);

				WT_READ_REQ_CLR(rr);
				__wt_toc_serialize_wrapup(toc, NULL, ret);

				didwork = 1;

				/*
				 * Any error terminates the request; a serious
				 * error causes the read server to exit.
				 */
				if (ret != 0) {
					if (ret != WT_RESTART)
						goto err;
					ret = 0;
				}
			}
		} while (didwork);
	}

	if (ret != 0)
err:		__wt_api_env_err(env, ret, "cache read server error");

	WT_VERBOSE(env, WT_VERB_READ, (env, "cache read server exiting"));
	return (NULL);
}

/*
 * __wt_cache_read --
 *	Read a page from the file.
 */
static int
__wt_cache_read(WT_READ_REQ *rr)
{
	DB *db;
	ENV *env;
	WT_CACHE *cache;
	WT_FH *fh;
	WT_OFF *off;
	WT_PAGE *page;
	WT_REF *ref;
	WT_TOC *toc;
	uint32_t addr, size;
	int ret;

	toc = rr->toc;
	ref = rr->ref;
	off = rr->off;
	addr = off->addr;
	size = off->size;

	db = toc->db;
	env = toc->env;
	cache = env->ienv->cache;
	fh = db->idb->fh;
	ret = 0;

	/*
	 * Check to see if some other thread brought the page into the cache
	 * while our request was in the queue.   If the state is anything
	 * other than empty, it's not our problem.
	 */
	if (ref->state != WT_EMPTY)
		return (0);

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
	WT_ERR(__wt_calloc(env, (size_t)size, sizeof(uint8_t), &page->hdr));

	/* Read the page. */
	WT_VERBOSE(env, WT_VERB_READ,
	    (env, "cache read addr/size %lu/%lu", (u_long)addr, (u_long)size));
	WT_STAT_INCR(cache->stats, PAGE_READ);

	page->addr = addr;
	page->size = size;
	WT_ERR(__wt_page_read(db, page));
	WT_CACHE_PAGE_IN(cache, size);

	/* If the page needs to be verified, that's next. */
	if (rr->dsk_verify)
		WT_ERR(__wt_bt_verify_dsk_page(toc, page));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_bt_page_inmem(toc, page));

	/*
	 * Reference the WT_OFF structure that read the page -- typically it's
	 * the WT_OFF structure on the parent's page.
	 */
	page->parent_ref = off;

	/*
	 * The page is now available -- set the LRU so the page is not selected
	 * for eviction.
	 */
	page->read_gen = ++cache->read_gen;
	ref->page = page;
	ref->state = WT_OK;

	return (0);

err:	if (page != NULL) {
		if (page->hdr != NULL)
			__wt_free(env, page->hdr, size);
		__wt_free(env, page, sizeof(WT_PAGE));
	}
	return (ret);
}
