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
 * WT_CACHE_DRAIN_CHECK --
 *	Macro to wait on the cache to drain.
 *
 * !!!
 * It might be better to sleep here, because it can take a long time for the
 * cache to drain -- again, once we have an I/O thread, it can wake us up.
 */
#define	WT_CACHE_DRAIN_CHECK(toc) do {					\
	while (F_ISSET((toc)->env->ienv, WT_CACHE_LOCKOUT)) {		\
		F_SET(toc, WT_CACHE_DRAIN_WAIT);			\
		__wt_toc_serialize_request(toc, NULL, NULL);		\
		F_CLR(toc, WT_CACHE_DRAIN_WAIT);			\
	}								\
} while (0)

/*
 * __wt_cache_alloc --
 *	Allocate bytes from a file.
 */
int
__wt_cache_alloc(WT_TOC *toc, u_int32_t bytes, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	WT_PAGE *page;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

	/* Check cache size before any allocation. */
	WT_CACHE_DRAIN_CHECK(toc);

	WT_STAT_INCR(env->ienv->stats, CACHE_ALLOC);
	WT_STAT_INCR(idb->stats, DB_CACHE_ALLOC);

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 * Clear the memory because code depends on initial values of 0.
	 */
	WT_RET(__wt_malloc(env, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_malloc(env, (size_t)bytes, &page->hdr));

	/*
	 * Allocate "bytes" bytes from the end of the file; we must serialize
	 * the allocation of bytes from the file, the change of total bytes
	 * in the cache, and the insert onto the hash queue.
	 */
	__wt_cache_in_serial(toc, page, WT_ADDR_INVALID, bytes);
	if ((ret = toc->serial_ret) != 0)
		goto err;

	*pagep = page;
	return (0);

err:	if (page->hdr != NULL)
		__wt_free(env, page->hdr, bytes);
	__wt_free(env, page, sizeof(WT_PAGE));
	return (ret);
}

/*
 * __wt_cache_in --
 *	Return a database page, reading as necessary.
 */
int
__wt_cache_in(WT_TOC *toc,
    u_int32_t addr, u_int32_t bytes, u_int32_t flags, WT_PAGE **pagep)
{
	DB *db;
	ENV *env;
	IDB *idb;
	IENV *ienv;
	WT_CACHE *cache;
	WT_PAGE *page;
	WT_PAGE_HDR *hdr;
	off_t offset;
	u_int32_t checksum;
	u_int bucket_cnt;
	int ret;

	*pagep = NULL;

	db = toc->db;
	env = toc->env;
	idb = db->idb;
	ienv = env->ienv;
	cache = &ienv->cache;

	WT_ASSERT(env, bytes % WT_FRAGMENT == 0);

retry:	/* Search for the page in the cache. */
	for (bucket_cnt = 0,
	    page = cache->hb[WT_HASH(cache, addr)];
	    page != NULL; page = page->next) {
		++bucket_cnt;
		if (page->addr == addr && page->db == db)
			break;
	}
	if (bucket_cnt > WT_STAT(ienv->stats, LONGEST_BUCKET))
		WT_STAT_SET(ienv->stats, LONGEST_BUCKET, bucket_cnt);
	if (page != NULL) {
		/*
		 * Add the page to the WT_TOC's hazard list and flush the write,
		 * then see if the page is being discarded by the cache server.
		 * That shouldn't happen often --  we select pages based on LRU
		 * generation.  If the page is being discarded, we can't use it,
		 * sleep awhile and retry.
		 *
		 * !!!
		 * When we have an I/O thread, we could schedule work for it,
		 * and sleep, and the I/O thread could wake us when the page is
		 * ready.
		 */
		__wt_hazard_set(toc, page);
		if (page->drain) {
			__wt_hazard_clear(toc, page);
			__wt_sleep(0, 100000);
			goto retry;
		}

		WT_STAT_INCR(env->ienv->stats, CACHE_HIT);
		WT_STAT_INCR(idb->stats, DB_CACHE_HIT);

		page->page_gen = ++ienv->page_gen;
		*pagep = page;
		return (0);
	}

	/* Check cache size before any allocation. */
	WT_CACHE_DRAIN_CHECK(toc);

	WT_STAT_INCR(env->ienv->stats, CACHE_MISS);
	WT_STAT_INCR(idb->stats, DB_CACHE_MISS);

	/*
	 * Allocate memory for the in-memory page information and for the page
	 * itself. They're two separate allocation calls so we (hopefully) get
	 * better alignment from the underlying heap memory allocator.
	 * Clear the memory because code depends on initial values of 0.
	 *
	 * Read the page.
	 */
	WT_RET(__wt_malloc(env, sizeof(WT_PAGE), &page));
	WT_ERR(__wt_malloc(env, (size_t)bytes, &page->hdr));
	offset = WT_ADDR_TO_OFF(db, addr);
	WT_ERR(__wt_read(env, idb->fh, offset, bytes, page->hdr));

	/*
	 * If this is an unformatted read, ensure we never find it again in the
	 * cache by not linking it in.
	 */
	if (LF_ISSET(WT_UNFORMATTED)) {
		F_SET(page, WT_UNFORMATTED);
		*pagep = page;
		return (0);
	}

	/* Verify the checksum. */
	hdr = page->hdr;
	checksum = hdr->checksum;
	hdr->checksum = 0;
	if (checksum != __wt_cksum(hdr, bytes)) {
		__wt_api_db_errx(db,
		    "file offset %llu with length %lu was read and had a "
		    "checksum error", (u_quad)offset, (u_long)bytes);
		ret = WT_ERROR;
		goto err;
	}

	/* Serialize the insert onto the hash queue. */
	__wt_cache_in_serial(toc, page, addr, bytes);
	if ((ret = toc->serial_ret) == WT_RESTART)
		goto err;

	*pagep = page;
	return (0);

err:	__wt_free(env, page->hdr, bytes);
	__wt_free(env, page, sizeof(WT_PAGE));
	if (ret == WT_RESTART)
		goto retry;
	return (ret);
}

/*
 * __wt_cache_out --
 *	Discard a database page, writing as necessary.
 */
int
__wt_cache_out(WT_TOC *toc, WT_PAGE *page, u_int32_t flags)
{
	ENV *env;

	env = toc->env;

	/* Unformatted pages may not be modified. */
	WT_ASSERT(env,
	    !(LF_ISSET(WT_MODIFIED) && F_ISSET(page, WT_UNFORMATTED)));

	/* If the page has been modified, set the local flag. */
	if (LF_ISSET(WT_MODIFIED))
		F_SET(page, WT_MODIFIED);

	/*
	 * Unformatted pages were never linked on the cache chains, and are
	 * discarded as soon as they're returned.
	 */
	if (F_ISSET(page, WT_UNFORMATTED))
		__wt_bt_page_recycle(env, page);
	else
		__wt_hazard_clear(toc, page);

	return (0);
}
