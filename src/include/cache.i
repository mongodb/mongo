/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_eviction_check --
 *	Wake the eviction server if necessary.
 */
static inline int
__wt_eviction_check(WT_SESSION_IMPL *session, int *read_lockoutp, int wake)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If we're over the maximum cache, shut out reads (which
	 * include page allocations) until we evict to back under the
	 * maximum cache.  Eviction will keep pushing out pages so we
	 * don't run on the edge all the time.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	dirty_inuse = __wt_cache_bytes_dirty(cache);
	bytes_max = conn->cache_size;
	if (read_lockoutp != NULL)
		*read_lockoutp = (bytes_inuse > bytes_max);

	/* Wake eviction when we're over the trigger cache size. */
	if (wake &&
	    (bytes_inuse > (cache->eviction_trigger * bytes_max) / 100 ||
	    dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100))
		WT_RET(__wt_evict_server_wake(session));
	return (0);
}

/*
 * __wt_cache_full_check --
 *	Wait for there to be space in the cache before a read or update.
 *	If one pass is true we are trying to catch a pathological case where
 *	the application can't make progress because the cache is too full.
 *	Freeing a single page is enough in that case.
 */
static inline int
__wt_cache_full_check(WT_SESSION_IMPL *session, int onepass)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	int lockout, wake;

	btree = S2BT(session);

	/*
	 * Only wake the eviction server the first time through here (if the
	 * cache is too full), or every thousand times after that.  Otherwise,
	 * we are just wasting effort and making a busy condition variable
	 * busier.
	 */
	for (wake = 0;; wake = (wake + 1) % 1000) {
		WT_RET(__wt_eviction_check(session, &lockout, wake));
		if (!lockout || F_ISSET(session,
		    WT_SESSION_NO_CACHE_CHECK | WT_SESSION_SCHEMA_LOCKED))
			return (0);
		if (btree != NULL &&
		    F_ISSET(btree, WT_BTREE_BULK | WT_BTREE_NO_EVICTION))
			return (0);
		ret = __wt_evict_lru_page(session, 1);
		if (ret == 0 || ret == EBUSY) {
			if (onepass)
				return (0);
		} else if (ret == WT_NOTFOUND)
			/*
			 * The eviction queue was empty - give the server time
			 * to re-populate before trying again.
			 */
			__wt_sleep(0, 10);
		else
			/*
			 * We've dealt with expected returns - we came across
			 * a real error.
			 */
			return (ret);
	}
}
