/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cache_read_gen --
 *      Get the current read generation number.
 */
static inline uint64_t
__wt_cache_read_gen(WT_SESSION_IMPL *session)
{
	return (S2C(session)->cache->read_gen);
}

/*
 * __wt_cache_read_gen_incr --
 *      Increment the current read generation number.
 */
static inline void
__wt_cache_read_gen_incr(WT_SESSION_IMPL *session)
{
	++S2C(session)->cache->read_gen;
}

/*
 * __wt_cache_read_gen_set --
 *      Get the read generation to store in a page.
 */
static inline uint64_t
__wt_cache_read_gen_set(WT_SESSION_IMPL *session)
{
	/*
	 * We return read-generations from the future (where "the future" is
	 * measured by increments of the global read generation).  The reason
	 * is because when acquiring a new hazard pointer for a page, we can
	 * check its read generation, and if the read generation isn't less
	 * than the current global generation, we don't bother updating the
	 * page.  In other words, the goal is to avoid some number of updates
	 * immediately after each update we have to make.
	 */
	return (__wt_cache_read_gen(session) + WT_READGEN_STEP);
}

/*
 * __wt_cache_pages_inuse --
 *	Return the number of pages in use.
 */
static inline uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
	return (cache->pages_inmem - cache->pages_evict);
}

/*
 * __wt_cache_bytes_inuse --
 *	Return the number of bytes in use.
 */
static inline uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
	return (cache->bytes_inmem);
}

/*
 * __wt_eviction_check --
 *	Wake the eviction server if necessary.
 */
static inline int
__wt_eviction_check(WT_SESSION_IMPL *session, int *fullp, int wake)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If we're over the maximum cache, shut out reads (which include page
	 * allocations) until we evict to back under the maximum cache.
	 * Eviction will keep pushing out pages so we don't run on the edge all
	 * the time.  Avoid division by zero if the cache size has not yet been
	 * in a shared cache.
	 */
	bytes_inuse = __wt_cache_bytes_inuse(cache);
	dirty_inuse = cache->bytes_dirty;
	bytes_max = conn->cache_size + 1;

	/* Adjust the cache size to take allocation overhead into account. */
	if (conn->cache_overhead != 0)
		bytes_max -= (bytes_max * (uint64_t)conn->cache_overhead) / 100;

	/* Calculate the cache full percentage. */
	*fullp = (int)((100 * bytes_inuse) / bytes_max);

	/* Wake eviction when we're over the trigger cache size. */
	if (wake &&
	    (bytes_inuse > (cache->eviction_trigger * bytes_max) / 100 ||
	    dirty_inuse > (cache->eviction_dirty_target * bytes_max) / 100))
		WT_RET(__wt_evict_server_wake(session));

	return (0);
}

/*
 * __wt_session_can_wait --
 *	Return if a session available for a potentially slow operation.
 */
static inline int
__wt_session_can_wait(WT_SESSION_IMPL *session)
{
	/*
	 * Return if a session available for a potentially slow operation;
	 * for example, used by the block manager in the case of flushing
	 * the system cache.
	 */
	if (!F_ISSET(session, WT_SESSION_CAN_WAIT))
		return (0);

	/*
	 * LSM sets the no-cache-check flag when holding the LSM tree lock,
	 * in that case, or when holding the schema lock, we don't want to
	 * highjack the thread for eviction.
	 */
	if (F_ISSET(session,
	    WT_SESSION_NO_CACHE_CHECK | WT_SESSION_SCHEMA_LOCKED))
		return (0);

	return (1);
}

/*
 * __wt_cache_full_check --
 *	Wait for there to be space in the cache before a read or update.
 */
static inline int
__wt_cache_full_check(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	int full;

	/*
	 * LSM sets the no-cache-check flag when holding the LSM tree lock, in
	 * that case, or when holding the schema or handle list locks (which
	 * block eviction), we don't want to highjack the thread for eviction.
	 */
	if (F_ISSET(session, WT_SESSION_NO_CACHE_CHECK |
	    WT_SESSION_SCHEMA_LOCKED | WT_SESSION_HANDLE_LIST_LOCKED))
		return (0);

	/*
	 * Threads operating on trees that cannot be evicted are ignored,
	 * mostly because they're not contributing to the problem.
	 */
	if ((btree = S2BT_SAFE(session)) != NULL &&
	    F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return (0);

	/*
	 * Only wake the eviction server the first time through here (if the
	 * cache is too full).
	 *
	 * If the cache is less than 95% full, no work to be done.  If we are
	 * at the API boundary and the cache is more than 95% full, try to
	 * evict at least one page before we start an operation.  This helps
	 * with some eviction-dominated workloads.
	 */
	WT_RET(__wt_eviction_check(session, &full, 1));
	if (full < 95)
		return (0);

	return (__wt_cache_wait(session, full));
}
