/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_cache_aggressive --
 *      Indicate if the cache is operating in aggressive mode.
 */
static inline bool
__wt_cache_aggressive(WT_SESSION_IMPL *session)
{
	return (S2C(session)->cache->evict_aggressive_score >=
	    WT_EVICT_SCORE_CUTOFF);
}

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
 * __wt_cache_read_gen_bump --
 *      Update the page's read generation.
 */
static inline void
__wt_cache_read_gen_bump(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	/* Ignore pages set for forcible eviction. */
	if (page->read_gen == WT_READGEN_OLDEST)
		return;

	/* Ignore pages already in the future. */
	if (page->read_gen > __wt_cache_read_gen(session))
		return;

	/*
	 * We set read-generations in the future (where "the future" is measured
	 * by increments of the global read generation).  The reason is because
	 * when acquiring a new hazard pointer for a page, we can check its read
	 * generation, and if the read generation isn't less than the current
	 * global generation, we don't bother updating the page.  In other
	 * words, the goal is to avoid some number of updates immediately after
	 * each update we have to make.
	 */
	page->read_gen = __wt_cache_read_gen(session) + WT_READGEN_STEP;
}

/*
 * __wt_cache_read_gen_new --
 *      Get the read generation for a new page in memory.
 */
static inline void
__wt_cache_read_gen_new(WT_SESSION_IMPL *session, WT_PAGE *page)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;
	page->read_gen =
	    (__wt_cache_read_gen(session) + cache->read_gen_oldest) / 2;
}

/*
 * __wt_cache_stuck --
 *      Indicate if the cache is stuck (i.e., not making progress).
 */
static inline bool
__wt_cache_stuck(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;

	cache = S2C(session)->cache;
	return (cache->evict_aggressive_score == WT_EVICT_SCORE_MAX &&
	    F_ISSET(cache,
		WT_CACHE_EVICT_CLEAN_HARD | WT_CACHE_EVICT_DIRTY_HARD));
}

/*
 * __wt_page_evict_soon --
 *      Set a page to be evicted as soon as possible.
 */
static inline void
__wt_page_evict_soon(WT_SESSION_IMPL *session, WT_REF *ref)
{
	WT_UNUSED(session);

	ref->page->read_gen = WT_READGEN_OLDEST;
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
 * __wt_cache_bytes_plus_overhead --
 *	Apply the cache overhead to a size in bytes.
 */
static inline uint64_t
__wt_cache_bytes_plus_overhead(WT_CACHE *cache, uint64_t sz)
{
	if (cache->overhead_pct != 0)
		sz += (sz * (uint64_t)cache->overhead_pct) / 100;

	return (sz);
}

/*
 * __wt_cache_bytes_inuse --
 *	Return the number of bytes in use.
 */
static inline uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
	return (__wt_cache_bytes_plus_overhead(cache, cache->bytes_inmem));
}

/*
 * __wt_cache_dirty_inuse --
 *	Return the number of dirty bytes in use.
 */
static inline uint64_t
__wt_cache_dirty_inuse(WT_CACHE *cache)
{
	return (__wt_cache_bytes_plus_overhead(cache,
	    cache->bytes_dirty_intl + cache->bytes_dirty_leaf));
}

/*
 * __wt_cache_dirty_leaf_inuse --
 *	Return the number of dirty bytes in use by leaf pages.
 */
static inline uint64_t
__wt_cache_dirty_leaf_inuse(WT_CACHE *cache)
{
	return (__wt_cache_bytes_plus_overhead(cache, cache->bytes_dirty_leaf));
}

/*
 * __wt_cache_bytes_image --
 *	Return the number of page image bytes in use.
 */
static inline uint64_t
__wt_cache_bytes_image(WT_CACHE *cache)
{
	return (__wt_cache_bytes_plus_overhead(cache, cache->bytes_image));
}

/*
 * __wt_cache_bytes_other --
 *	Return the number of bytes in use not for page images.
 */
static inline uint64_t
__wt_cache_bytes_other(WT_CACHE *cache)
{
	uint64_t bytes_image, bytes_inmem;

	bytes_image = cache->bytes_image;
	bytes_inmem = cache->bytes_inmem;

	/*
	 * The reads above could race with changes to the values, so protect
	 * against underflow.
	 */
	return ((bytes_image > bytes_inmem) ? 0 :
	    __wt_cache_bytes_plus_overhead(cache, bytes_inmem - bytes_image));
}

/*
 * __wt_session_can_wait --
 *	Return if a session available for a potentially slow operation.
 */
static inline bool
__wt_session_can_wait(WT_SESSION_IMPL *session)
{
	/*
	 * Return if a session available for a potentially slow operation;
	 * for example, used by the block manager in the case of flushing
	 * the system cache.
	 */
	if (!F_ISSET(session, WT_SESSION_CAN_WAIT))
		return (false);

	/*
	 * LSM sets the no-eviction flag when holding the LSM tree lock, in that
	 * case, or when holding the schema lock, we don't want to highjack the
	 * thread for eviction.
	 */
	return (!F_ISSET(
	    session, WT_SESSION_NO_EVICTION | WT_SESSION_LOCKED_SCHEMA));
}

/*
 * __wt_eviction_clean_needed --
 *	Return if an application thread should do eviction due to the total
 *	volume of dirty data in cache.
 */
static inline bool
__wt_eviction_clean_needed(WT_SESSION_IMPL *session, u_int *pct_fullp)
{
	WT_CACHE *cache;
	uint64_t bytes_inuse, bytes_max;

	cache = S2C(session)->cache;

	/*
	 * Avoid division by zero if the cache size has not yet been set in a
	 * shared cache.
	 */
	bytes_max = S2C(session)->cache_size + 1;
	bytes_inuse = __wt_cache_bytes_inuse(cache);

	if (pct_fullp != NULL)
		*pct_fullp = (u_int)((100 * bytes_inuse) / bytes_max);

	return (bytes_inuse > (cache->eviction_trigger * bytes_max) / 100);
}

/*
 * __wt_eviction_dirty_needed --
 *	Return if an application thread should do eviction due to the total
 *	volume of dirty data in cache.
 */
static inline bool
__wt_eviction_dirty_needed(WT_SESSION_IMPL *session, u_int *pct_fullp)
{
	WT_CACHE *cache;
	double dirty_trigger;
	uint64_t dirty_inuse, bytes_max;

	cache = S2C(session)->cache;

	/*
	 * Avoid division by zero if the cache size has not yet been set in a
	 * shared cache.
	 */
	bytes_max = S2C(session)->cache_size + 1;
	dirty_inuse = __wt_cache_dirty_leaf_inuse(cache);

	if (pct_fullp != NULL)
		*pct_fullp = (u_int)((100 * dirty_inuse) / bytes_max);

	if ((dirty_trigger = cache->eviction_scrub_limit) < 1.0)
		dirty_trigger = (double)cache->eviction_dirty_trigger;

	return (dirty_inuse > (uint64_t)(dirty_trigger * bytes_max) / 100);
}

/*
 * __wt_eviction_needed --
 *	Return if an application thread should do eviction, and the cache full
 *      percentage as a side-effect.
 */
static inline bool
__wt_eviction_needed(WT_SESSION_IMPL *session, bool busy, u_int *pct_fullp)
{
	WT_CACHE *cache;
	u_int pct_dirty, pct_full;
	bool clean_needed, dirty_needed;

	cache = S2C(session)->cache;

	/*
	 * If the connection is closing we do not need eviction from an
	 * application thread.  The eviction subsystem is already closed.
	 */
	if (F_ISSET(S2C(session), WT_CONN_CLOSING))
		return (false);

	clean_needed = __wt_eviction_clean_needed(session, &pct_full);
	dirty_needed = __wt_eviction_dirty_needed(session, &pct_dirty);

	/*
	 * Calculate the cache full percentage; anything over the trigger means
	 * we involve the application thread.
	 */
	if (pct_fullp != NULL)
		*pct_fullp = (u_int)WT_MAX(0, 100 - WT_MIN(
		    (int)cache->eviction_trigger - (int)pct_full,
		    (int)cache->eviction_dirty_trigger - (int)pct_dirty));

	/*
	 * Only check the dirty trigger when the session is not busy.
	 *
	 * In other words, once we are pinning resources, try to finish the
	 * operation as quickly as possible without exceeding the cache size.
	 * The next transaction in this session will not be able to start until
	 * the cache is under the limit.
	 */
	return (clean_needed || (!busy && dirty_needed));
}

/*
 * __wt_cache_full --
 *	Return if the cache is at (or over) capacity.
 */
static inline bool
__wt_cache_full(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_CACHE *cache;

	conn = S2C(session);
	cache = conn->cache;

	return (__wt_cache_bytes_inuse(cache) >= conn->cache_size);
}

/*
 * __wt_cache_eviction_check --
 *	Evict pages if the cache crosses its boundaries.
 */
static inline int
__wt_cache_eviction_check(WT_SESSION_IMPL *session, bool busy, bool *didworkp)
{
	WT_BTREE *btree;
	WT_TXN_GLOBAL *txn_global;
	WT_TXN_STATE *txn_state;
	u_int pct_full;

	if (didworkp != NULL)
		*didworkp = false;

	/*
	 * If the current transaction is keeping the oldest ID pinned, it is in
	 * the middle of an operation.	This may prevent the oldest ID from
	 * moving forward, leading to deadlock, so only evict what we can.
	 * Otherwise, we are at a transaction boundary and we can work harder
	 * to make sure there is free space in the cache.
	 */
	txn_global = &S2C(session)->txn_global;
	txn_state = WT_SESSION_TXN_STATE(session);
	busy = busy || txn_state->id != WT_TXN_NONE ||
	    session->nhazard > 0 ||
	    (txn_state->pinned_id != WT_TXN_NONE &&
	    txn_global->current != txn_global->oldest_id);

	/*
	 * LSM sets the no-cache-check flag when holding the LSM tree lock, in
	 * that case, or when holding the schema or handle list locks (which
	 * block eviction), we don't want to highjack the thread for eviction.
	 */
	if (F_ISSET(session, WT_SESSION_NO_EVICTION |
	    WT_SESSION_LOCKED_HANDLE_LIST_WRITE | WT_SESSION_LOCKED_SCHEMA))
		return (0);

	/* In memory configurations don't block when the cache is full. */
	if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
		return (0);

	/*
	 * Threads operating on cache-resident trees are ignored because they're
	 * not contributing to the problem.
	 */
	btree = S2BT_SAFE(session);
	if (btree != NULL && F_ISSET(btree, WT_BTREE_IN_MEMORY))
		return (0);

	/* Check if eviction is needed. */
	if (!__wt_eviction_needed(session, busy, &pct_full))
		return (0);

	/*
	 * Some callers (those waiting for slow operations), will sleep if there
	 * was no cache work to do. After this point, let them skip the sleep.
	 */
	if (didworkp != NULL)
		*didworkp = true;

	return (__wt_cache_eviction_worker(session, busy, pct_full));
}
