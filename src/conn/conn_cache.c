/*-
 * Copyright (c) 2014-2017 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __cache_config_local --
 *	Configure the underlying cache.
 */
static int
__cache_config_local(WT_SESSION_IMPL *session, bool shared, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	uint32_t evict_threads_max, evict_threads_min;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If not using a shared cache configure the cache size, otherwise
	 * check for a reserved size. All other settings are independent of
	 * whether we are using a shared cache or not.
	 */
	if (!shared) {
		WT_RET(__wt_config_gets(session, cfg, "cache_size", &cval));
		conn->cache_size = (uint64_t)cval.val;
	}

	WT_RET(__wt_config_gets(session, cfg, "cache_overhead", &cval));
	cache->overhead_pct = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_target", &cval));
	cache->eviction_target = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
	cache->eviction_trigger = (u_int)cval.val;

	WT_RET(__wt_config_gets(
	    session, cfg, "eviction_checkpoint_target", &cval));
	cache->eviction_checkpoint_target = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_target", &cval));
	cache->eviction_dirty_target = (u_int)cval.val;

	/*
	 * Don't allow the dirty target to be larger than the overall
	 * target.
	 */
	if (cache->eviction_dirty_target > cache->eviction_target)
		cache->eviction_dirty_target = cache->eviction_target;

	/*
	 * Sanity check the checkpoint target: don't allow a value
	 * lower than the dirty target.
	 */
	if (cache->eviction_checkpoint_target > 0 &&
	    cache->eviction_checkpoint_target < cache->eviction_dirty_target)
		cache->eviction_checkpoint_target =
		    cache->eviction_dirty_target;

	WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_trigger", &cval));
	cache->eviction_dirty_trigger = (u_int)cval.val;

	/*
	 * Don't allow the dirty trigger to be larger than the overall
	 * trigger or we can get stuck with a cache full of dirty data.
	 */
	if (cache->eviction_dirty_trigger > cache->eviction_trigger)
		cache->eviction_dirty_trigger = cache->eviction_trigger;

	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
	WT_ASSERT(session, cval.val > 0);
	evict_threads_max = (uint32_t)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_min", &cval));
	WT_ASSERT(session, cval.val > 0);
	evict_threads_min = (uint32_t)cval.val;

	if (evict_threads_min > evict_threads_max)
		WT_RET_MSG(session, EINVAL,
		    "eviction=(threads_min) cannot be greater than "
		    "eviction=(threads_max)");
	conn->evict_threads_max = evict_threads_max;
	conn->evict_threads_min = evict_threads_min;

	return (0);
}

/*
 * __wt_cache_config --
 *	Configure or reconfigure the current cache and shared cache.
 */
int
__wt_cache_config(WT_SESSION_IMPL *session, bool reconfigure, const char *cfg[])
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	bool now_shared, was_shared;

	conn = S2C(session);

	WT_ASSERT(session, conn->cache != NULL);

	WT_RET(__wt_config_gets_none(session, cfg, "shared_cache.name", &cval));
	now_shared = cval.len != 0;
	was_shared = F_ISSET(conn, WT_CONN_CACHE_POOL);

	/* Cleanup if reconfiguring */
	if (reconfigure && was_shared && !now_shared)
		/* Remove ourselves from the pool if necessary */
		WT_RET(__wt_conn_cache_pool_destroy(session));
	else if (reconfigure && !was_shared && now_shared)
		/*
		 * Cache size will now be managed by the cache pool - the
		 * start size always needs to be zero to allow the pool to
		 * manage how much memory is in-use.
		 */
		conn->cache_size = 0;

	/*
	 * Always setup the local cache - it's used even if we are
	 * participating in a shared cache.
	 */
	WT_RET(__cache_config_local(session, now_shared, cfg));
	if (now_shared) {
		WT_RET(__wt_cache_pool_config(session, cfg));
		WT_ASSERT(session, F_ISSET(conn, WT_CONN_CACHE_POOL));
		if (!was_shared)
			WT_RET(__wt_conn_cache_pool_open(session));
	}

	/*
	 * Resize the thread group if reconfiguring, otherwise the thread group
	 * will be initialized as part of creating the cache.
	 */
	if (reconfigure)
		WT_RET(__wt_thread_group_resize(
		    session, &conn->evict_threads,
		    conn->evict_threads_min,
		    conn->evict_threads_max,
		    WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL));

	return (0);
}

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int i;

	conn = S2C(session);

	WT_ASSERT(session, conn->cache == NULL);

	WT_RET(__wt_calloc_one(session, &conn->cache));

	cache = conn->cache;

	/* Use a common routine for run-time configuration options. */
	WT_RET(__wt_cache_config(session, false, cfg));

	/*
	 * The lowest possible page read-generation has a special meaning, it
	 * marks a page for forcible eviction; don't let it happen by accident.
	 */
	cache->read_gen = WT_READGEN_START_VALUE;

	/*
	 * The target size must be lower than the trigger size or we will never
	 * get any work done.
	 */
	if (cache->eviction_target >= cache->eviction_trigger)
		WT_RET_MSG(session, EINVAL,
		    "eviction target must be lower than the eviction trigger");

	WT_RET(__wt_cond_auto_alloc(session,
	    "cache eviction server", 10000, WT_MILLION, &cache->evict_cond));
	WT_RET(__wt_spin_init(session, &cache->evict_pass_lock, "evict pass"));
	WT_RET(__wt_spin_init(session,
	    &cache->evict_queue_lock, "cache eviction queue"));
	WT_RET(__wt_spin_init(session, &cache->evict_walk_lock, "cache walk"));
	if ((ret = __wt_open_internal_session(conn, "evict pass",
	    false, WT_SESSION_NO_DATA_HANDLES, &cache->walk_session)) != 0)
		WT_RET_MSG(NULL, ret,
		    "Failed to create session for eviction walks");

	/* Allocate the LRU eviction queue. */
	cache->evict_slots = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	for (i = 0; i < WT_EVICT_QUEUE_MAX; ++i) {
		WT_RET(__wt_calloc_def(session,
		    cache->evict_slots, &cache->evict_queues[i].evict_queue));
		WT_RET(__wt_spin_init(session,
		    &cache->evict_queues[i].evict_lock, "cache eviction"));
	}

	/* Ensure there are always non-NULL queues. */
	cache->evict_current_queue = cache->evict_fill_queue =
	    &cache->evict_queues[0];
	cache->evict_other_queue = &cache->evict_queues[1];
	cache->evict_urgent_queue = &cache->evict_queues[WT_EVICT_URGENT_QUEUE];

	/*
	 * We get/set some values in the cache statistics (rather than have
	 * two copies), configure them.
	 */
	__wt_cache_stats_update(session);
	return (0);
}

/*
 * __wt_cache_stats_update --
 *	Update the cache statistics for return to the application.
 */
void
__wt_cache_stats_update(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS **stats;
	uint64_t inuse, leaf;

	conn = S2C(session);
	cache = conn->cache;
	stats = conn->stats;

	inuse = __wt_cache_bytes_inuse(cache);
	/*
	 * There are races updating the different cache tracking values so
	 * be paranoid calculating the leaf byte usage.
	 */
	leaf = inuse > cache->bytes_internal ?
	    inuse - cache->bytes_internal : 0;

	WT_STAT_SET(session, stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(session, stats, cache_bytes_inuse, inuse);
	WT_STAT_SET(session, stats, cache_overhead, cache->overhead_pct);

	WT_STAT_SET(
	    session, stats, cache_bytes_dirty, __wt_cache_dirty_inuse(cache));
	WT_STAT_SET(
	    session, stats, cache_bytes_image, __wt_cache_bytes_image(cache));
	WT_STAT_SET(
	    session, stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
	WT_STAT_SET(
	    session, stats, cache_bytes_internal, cache->bytes_internal);
	WT_STAT_SET(session, stats, cache_bytes_leaf, leaf);
	WT_STAT_SET(
	    session, stats, cache_bytes_other, __wt_cache_bytes_other(cache));

	WT_STAT_SET(session, stats,
	    cache_eviction_maximum_page_size, cache->evict_max_page_size);
	WT_STAT_SET(session, stats, cache_pages_dirty,
	    cache->pages_dirty_intl + cache->pages_dirty_leaf);

	/*
	 * The number of files with active walks ~= number of hazard pointers
	 * in the walk session.  Note: reading without locking.
	 */
	if (conn->evict_server_running)
		WT_STAT_SET(session, stats, cache_eviction_walks_active,
		    cache->walk_session->nhazard);
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
int
__wt_cache_destroy(WT_SESSION_IMPL *session)
{
	WT_CACHE *cache;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	int i;

	conn = S2C(session);
	cache = conn->cache;

	if (cache == NULL)
		return (0);

	/* The cache should be empty at this point.  Complain if not. */
	if (cache->pages_inmem != cache->pages_evict)
		__wt_errx(session,
		    "cache server: exiting with %" PRIu64 " pages in "
		    "memory and %" PRIu64 " pages evicted",
		    cache->pages_inmem, cache->pages_evict);
	if (cache->bytes_inmem != 0)
		__wt_errx(session,
		    "cache server: exiting with %" PRIu64 " bytes in memory",
		    cache->bytes_inmem);
	if (cache->bytes_dirty_intl + cache->bytes_dirty_leaf != 0 ||
	    cache->pages_dirty_intl + cache->pages_dirty_leaf != 0)
		__wt_errx(session,
		    "cache server: exiting with %" PRIu64
		    " bytes dirty and %" PRIu64 " pages dirty",
		    cache->bytes_dirty_intl + cache->bytes_dirty_leaf,
		    cache->pages_dirty_intl + cache->pages_dirty_leaf);

	__wt_cond_destroy(session, &cache->evict_cond);
	__wt_spin_destroy(session, &cache->evict_pass_lock);
	__wt_spin_destroy(session, &cache->evict_queue_lock);
	__wt_spin_destroy(session, &cache->evict_walk_lock);
	wt_session = &cache->walk_session->iface;
	if (wt_session != NULL)
		WT_TRET(wt_session->close(wt_session, NULL));

	for (i = 0; i < WT_EVICT_QUEUE_MAX; ++i) {
		__wt_spin_destroy(session, &cache->evict_queues[i].evict_lock);
		__wt_free(session, cache->evict_queues[i].evict_queue);
	}

	__wt_free(session, conn->cache);
	return (ret);
}
