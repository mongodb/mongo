/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
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
__wt_cache_config(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONFIG_ITEM cval;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;
	cache = conn->cache;

	/*
	 * If not using a shared cache configure the cache size, otherwise
	 * check for a reserved size.
	 */
	if (!F_ISSET(conn, WT_CONN_CACHE_POOL) &&
	    (ret = __wt_config_gets(session, cfg, "cache_size", &cval)) == 0)
		conn->cache_size = (uint64_t)cval.val;

	if (F_ISSET(conn, WT_CONN_CACHE_POOL) &&
	    (ret = __wt_config_gets(session, cfg,
	    "shared_cache.reserve", &cval)) == 0 && cval.val != 0)
		cache->cp_reserved = (uint64_t)cval.val;
	else if ((ret = __wt_config_gets(session, cfg,
	    "shared_cache.chunk", &cval)) == 0)
		cache->cp_reserved = (uint64_t)cval.val;
	WT_RET_NOTFOUND_OK(ret);

	if ((ret =
	    __wt_config_gets(session, cfg, "eviction_target", &cval)) == 0)
		cache->eviction_target = (u_int)cval.val;
	WT_RET_NOTFOUND_OK(ret);

	if ((ret =
	    __wt_config_gets(session, cfg, "eviction_trigger", &cval)) == 0)
		cache->eviction_trigger = (u_int)cval.val;
	WT_RET_NOTFOUND_OK(ret);

	if ((ret = __wt_config_gets(
	    session, cfg, "eviction_dirty_target", &cval)) == 0)
		cache->eviction_dirty_target = (u_int)cval.val;
	WT_RET_NOTFOUND_OK(ret);

	return (0);
}

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;

	WT_ASSERT(session, conn->cache == NULL ||
	    (F_ISSET(conn, WT_CONN_CACHE_POOL) && conn->cache != NULL));

	WT_RET(__wt_calloc_def(session, 1, &conn->cache));

	cache = conn->cache;

	/* Use a common routine for run-time configuration options. */
	WT_RET(__wt_cache_config(conn, cfg));

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
	__wt_spin_init(session, &cache->evict_lock);
	__wt_spin_init(session, &cache->evict_walk_lock);

	/* Allocate the LRU eviction queue. */
	cache->evict_entries = WT_EVICT_WALK_BASE + WT_EVICT_WALK_INCR;
	WT_ERR(__wt_calloc_def(session, cache->evict_entries, &cache->evict));

	/*
	 * We get/set some values in the cache statistics (rather than have
	 * two copies), configure them.
	 */
	__wt_cache_stats_update(session);
	return (0);

err:	WT_RET(__wt_cache_destroy(conn));
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

	/*
	 * Some statistics are always set, regardless of the configuration of
	 * run-time statistics in the system.
	 */
	WT_STAT_SET(session, stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(
	    session, stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(
	    session, stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
	WT_STAT_SET(
	    session, stats, cache_bytes_dirty, __wt_cache_bytes_dirty(cache));
	WT_STAT_SET(
	    session, stats, cache_pages_dirty, __wt_cache_pages_dirty(cache));
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
int
__wt_cache_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = conn->default_session;
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
