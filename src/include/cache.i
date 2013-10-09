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
	dirty_inuse = cache->bytes_dirty;
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
	int lockout;

	/*
	 * Only wake the eviction server the first time through here (if the
	 * cache is too full).
	 */
	WT_RET(__wt_eviction_check(session, &lockout, 1));

	if (F_ISSET(session,
	    WT_SESSION_NO_CACHE_CHECK | WT_SESSION_SCHEMA_LOCKED))
		return (0);

	if ((btree = S2BT_SAFE(session)) != NULL &&
	    F_ISSET(btree, WT_BTREE_BULK | WT_BTREE_NO_EVICTION))
		return (0);

	for (;;) {
		if ((ret = __wt_evict_lru_page(session, 1)) == 0) {
			if (onepass)
				return (0);
		} else if (ret != EBUSY && ret != WT_NOTFOUND)
			return (ret);
		WT_RET(__wt_eviction_check(session, &lockout, 0));
		if (!lockout)
			return (0);
		if (ret == EBUSY)
			continue;
		/*
		 * The eviction queue was empty - wait for it to
		 * re-populate before trying again.
		 */
		WT_RET(__wt_cond_wait(session,
		    S2C(session)->cache->evict_waiter_cond, 10000));
	}
}
