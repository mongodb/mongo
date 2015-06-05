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
	uint64_t bytes_inuse;

	/* Adjust the cache size to take allocation overhead into account. */
	bytes_inuse = cache->bytes_inmem;
	if (cache->overhead_pct != 0)
		bytes_inuse +=
		    (bytes_inuse * (uint64_t)cache->overhead_pct) / 100;

	return (bytes_inuse);
}

/*
 * __wt_cache_dirty_inuse --
 *	Return the number of dirty bytes in use.
 */
static inline uint64_t
__wt_cache_dirty_inuse(WT_CACHE *cache)
{
	uint64_t dirty_inuse;

	dirty_inuse = cache->bytes_dirty;
	if (cache->overhead_pct != 0)
		dirty_inuse +=
		    (dirty_inuse * (uint64_t)cache->overhead_pct) / 100;

	return (dirty_inuse);
}

/*
 * __wt_cache_status --
 *	Return if the cache usage exceeds the eviction or dirty targets.
 */
static inline void
__wt_cache_status(WT_SESSION_IMPL *session, int *evictp, int *dirtyp)
{
	WT_CONNECTION_IMPL *conn;
	WT_CACHE *cache;
	uint64_t bytes_inuse, bytes_max, dirty_inuse;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * There's an assumption "evict" overrides "dirty", that is, if eviction
	 * is required, we no longer care where we are with respect to the dirty
	 * target.
	 *
	 * Avoid division by zero if the cache size has not yet been set in a
	 * shared cache.
	 */
	bytes_max = conn->cache_size + 1;
	if (evictp != NULL) {
		bytes_inuse = __wt_cache_bytes_inuse(cache);
		if (bytes_inuse > (cache->eviction_target * bytes_max) / 100) {
			*evictp = 1;
			return;
		}
		*evictp = 0;
	}
	if (dirtyp != NULL) {
		dirty_inuse = __wt_cache_dirty_inuse(cache);
		if (dirty_inuse >
		    (cache->eviction_dirty_target * bytes_max) / 100) {
			*dirtyp = 1;
			return;
		}
		*dirtyp = 0;
	}
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
	    WT_SESSION_NO_CACHE_CHECK | WT_SESSION_LOCKED_SCHEMA))
		return (0);

	return (1);
}
