/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __wt_eviction_check --
 *	Wake the eviction server if necessary.
 */
static inline int
__wt_eviction_check(WT_SESSION_IMPL *session, WT_PAGE *page, int *read_lockoutp)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If the page is pathologically large, force eviction.
	 * Otherwise, if the cache is more than 95% full, wake up the eviction
	 * thread.
	 */
	if (page != NULL &&
	    (int64_t)page->memory_footprint > conn->cache_size / 2 &&
	    !F_ISSET(page, WT_PAGE_FORCE_EVICT | WT_PAGE_PINNED)) {
		F_SET(page, WT_PAGE_FORCE_EVICT);
		/*
		 * XXX We're already inside a serialized function, so
		 * we need to take some care.
		 */
		WT_RET(__wt_evict_page_serial(session, page));
		__wt_evict_server_wake(session);
	} else {
		/*
		 * If we're over the maximum cache, shut out reads (which
		 * include page allocations) until we evict to back under the
		 * maximum cache.  Eviction will keep pushing out pages so we
		 * don't run on the edge all the time.
		 */
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		bytes_max = conn->cache_size;
#if 1
		if (read_lockoutp != NULL)
			*read_lockoutp = (bytes_inuse > bytes_max);
#else
		if (!cache->read_lockout && bytes_inuse > bytes_max) {
			WT_VERBOSE(session, readserver,
			    "lock out reads: bytes-inuse %" PRIu64
			    " of bytes-max %" PRIu64,
			    bytes_inuse, bytes_max);
			cache->read_lockout = 1;
		} else if (cache->read_lockout && bytes_inuse < bytes_max) {
			WT_VERBOSE(session, readserver,
			    "restore reads: bytes-inuse %" PRIu64
			    " of bytes-max %" PRIu64,
			    bytes_inuse, bytes_max);
			cache->read_lockout = 0;
		}
#endif

		/* Wake eviction when we're over the trigger cache size. */
		if (bytes_inuse > cache->eviction_trigger * (bytes_max / 100))
			__wt_evict_server_wake(session);
	}

	return (0);
}
