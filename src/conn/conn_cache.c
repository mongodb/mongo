/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_cache_config --
 *	Configure the underlying cache.
 */
int
__wt_cache_config(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	cache = conn->cache;

	/*
	 * If not using a shared cache configure the cache size, otherwise
	 * check for a reserved size.
	 */
	if (!F_ISSET(conn, WT_CONN_CACHE_POOL)) {
		WT_RET(__wt_config_gets(session, cfg, "cache_size", &cval));
		conn->cache_size = (uint64_t)cval.val;
	} else {
		WT_RET(__wt_config_gets(
		    session, cfg, "shared_cache.reserve", &cval));
		if (cval.val == 0)
			WT_RET(__wt_config_gets(
			    session, cfg, "shared_cache.chunk", &cval));
		cache->cp_reserved = (uint64_t)cval.val;
	}

	WT_RET(__wt_config_gets(session, cfg, "eviction_target", &cval));
	cache->eviction_target = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
	cache->eviction_trigger = (u_int)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "eviction_dirty_target", &cval));
	cache->eviction_dirty_target = (u_int)cval.val;

	/*
	 * The eviction thread configuration options include the main eviction
	 * thread and workers. Our implementation splits them out. Adjust for
	 * the difference when parsing the configuration.
	 */
	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_max", &cval));
	WT_ASSERT(session, cval.val > 0);
	conn->evict_workers_max = (u_int)cval.val - 1;

	WT_RET(__wt_config_gets(session, cfg, "eviction.threads_min", &cval));
	WT_ASSERT(session, cval.val > 0);
	conn->evict_workers_min = (u_int)cval.val - 1;

	if (conn->evict_workers_min > conn->evict_workers_max)
		WT_RET_MSG(session, EINVAL,
		    "eviction=(threads_min) cannot be greater than "
		    "eviction=(threads_max)");

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

	conn = S2C(session);

	WT_ASSERT(session, conn->cache == NULL ||
	    (F_ISSET(conn, WT_CONN_CACHE_POOL) && conn->cache != NULL));

	WT_RET(__wt_calloc_one(session, &conn->cache));

	cache = conn->cache;

	/* Use a common routine for run-time configuration options. */
	WT_RET(__wt_cache_config(session, cfg));

	/* Add the configured cache to the cache pool. */
	if (F_ISSET(conn, WT_CONN_CACHE_POOL))
		WT_RET(__wt_conn_cache_pool_open(session));

	/*
	 * The target size must be lower than the trigger size or we will never
	 * get any work done.
	 */
	if (cache->eviction_target >= cache->eviction_trigger)
		WT_ERR_MSG(session, EINVAL,
		    "eviction target must be lower than the eviction trigger");

	WT_ERR(__wt_cond_alloc(session,
	    "cache eviction server", 0, &cache->evict_cond));
	WT_ERR(__wt_cond_alloc(session,
	    "eviction waiters", 0, &cache->evict_waiter_cond));
	WT_ERR(__wt_spin_init(session, &cache->evict_lock, "cache eviction"));
	WT_ERR(__wt_spin_init(session, &cache->evict_walk_lock, "cache walk"));

	/* Allocate the LRU eviction queue. */
	cache->evict_slots = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	WT_ERR(__wt_calloc_def(session, cache->evict_slots, &cache->evict));

	/*
	 * We get/set some values in the cache statistics (rather than have
	 * two copies), configure them.
	 */
	__wt_cache_stats_update(session);
	return (0);

err:	WT_RET(__wt_cache_destroy(session));
	return (ret);
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
	WT_CONNECTION_STATS *stats;

	conn = S2C(session);
	cache = conn->cache;
	stats = &conn->stats;

	WT_STAT_SET(stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
	WT_STAT_SET(stats, cache_bytes_dirty, cache->bytes_dirty);
	WT_STAT_SET(stats,
	    cache_eviction_maximum_page_size, cache->evict_max_page_size);
	WT_STAT_SET(stats, cache_pages_dirty, cache->pages_dirty);
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

	conn = S2C(session);
	cache = conn->cache;

	if (cache == NULL)
		return (0);

	WT_TRET(__wt_cond_destroy(session, &cache->evict_cond));
	WT_TRET(__wt_cond_destroy(session, &cache->evict_waiter_cond));
	__wt_spin_destroy(session, &cache->evict_lock);
	__wt_spin_destroy(session, &cache->evict_walk_lock);

	__wt_free(session, cache->evict);
	__wt_free(session, conn->cache);
	return (ret);
}
