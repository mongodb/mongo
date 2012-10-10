/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_eviction_check --
 *	Wake the eviction server if necessary.
 */
static inline void
__wt_eviction_check(WT_SESSION_IMPL *session, int *read_lockoutp, int wake)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If we're over the maximum cache, shut out reads (which
	 * include page allocations) until we evict to back under the
	 * maximum cache.  Eviction will keep pushing out pages so we
	 * don't run on the edge all the time.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	bytes_max = conn->cache_size;
	if (read_lockoutp != NULL)
		*read_lockoutp = (bytes_inuse > bytes_max);

	/* Wake eviction when we're over the trigger cache size. */
	if (wake && bytes_inuse > cache->eviction_trigger * (bytes_max / 100))
		__wt_evict_server_wake(session);
}

/*
 * __wt_cache_full_check --
 *      Wait for there to be space in the cache before a read or update.
 */
static inline int
__wt_cache_full_check(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	int lockout, wake;

	/*
	 * Only wake the eviction server the first time through here (if the
	 * cache is too full), or after we fail to evict a page.  Otherwise, we
	 * are just wasting effort and making a busy mutex busier.
	 */
	for (wake = 1;; wake = 0) {
		__wt_eviction_check(session, &lockout, wake);
		if (!lockout ||
		    F_ISSET(session, WT_SESSION_SCHEMA_LOCKED) ||
		    F_ISSET(session->btree, WT_BTREE_NO_CACHE))
			return (0);
		if ((ret = __wt_evict_lru_page(session, 1)) == EBUSY)
			__wt_yield();
		else
			WT_RET_NOTFOUND_OK(ret);
	}
}
