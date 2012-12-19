/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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

	/* Ignore the cache size if a shared cache is configured. */
	if (!F_ISSET(conn, WT_CONN_CACHE_POOL) &&
	    (ret = __wt_config_gets(session, cfg, "cache_size", &cval)) == 0)
		conn->cache_size = (uint64_t)cval.val;
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

	if (F_ISSET(conn, WT_CONN_CACHE_POOL))
		WT_RET(__wt_conn_cache_pool_open(session));
	else
		WT_RET(__wt_calloc_def(session, 1, &conn->cache));

	cache = conn->cache;

	/* Use a common routine for run-time configuration options. */
	WT_RET(__wt_cache_config(conn, cfg));

	/*
	 * The target size must be lower than the trigger size or we will never
	 * get any work done.
	 */
	if (cache->eviction_target >= cache->eviction_trigger)
		WT_ERR_MSG(session, EINVAL,
		    "eviction target must be lower than the eviction trigger");

	WT_ERR(__wt_cond_alloc(session,
	    "cache eviction server", 0, &cache->evict_cond));
	__wt_spin_init(session, &cache->evict_lock);
	__wt_spin_init(session, &cache->evict_walk_lock);

	/*
	 * We pull some values from the cache statistics (rather than have two
	 * copies).  Set them.
	 */
	__wt_cache_stats_update(conn, 0);
	return (0);

err:	WT_RET(__wt_cache_destroy(conn));
	return (ret);
}

/*
 * __wt_cache_stats_update --
 *	Update the cache statistics for return to the application.
 */
void
__wt_cache_stats_update(WT_CONNECTION_IMPL *conn, uint32_t flags)
{
	WT_CACHE *cache;

	WT_UNUSED(flags);
	cache = conn->cache;

	WT_STAT_SET(conn->stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(
	    conn->stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(
	    conn->stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
	WT_STAT_SET(
	    conn->stats, cache_bytes_dirty, __wt_cache_dirty_bytes(cache));
	WT_STAT_SET(
	    conn->stats, cache_pages_dirty, cache->pages_dirty);
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

	if (cache->evict_cond != NULL)
		WT_TRET(__wt_cond_destroy(session, cache->evict_cond));
	__wt_spin_destroy(session, &cache->evict_lock);
	__wt_spin_destroy(session, &cache->evict_walk_lock);

	__wt_free(session, conn->cache);
	return (ret);
}
