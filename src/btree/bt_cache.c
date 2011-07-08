/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(WT_CONNECTION_IMPL *conn)
{
	WT_CACHE *cache;
	WT_SESSION_IMPL *session;
	int ret;

	session = &conn->default_session;
	ret = 0;

	WT_RET(__wt_calloc_def(session, 1, &conn->cache));
	cache = conn->cache;

	WT_ERR(__wt_mtx_alloc(session,
	    "cache eviction server", 1, &cache->mtx_evict));
	WT_ERR(__wt_mtx_alloc(session,
	    "cache read server", 1, &cache->mtx_read));

	WT_ERR(__wt_stat_alloc_cache_stats(session, &cache->stats));

	WT_STAT_SET(
	    cache->stats, cache_bytes_max, conn->cache_size * WT_MEGABYTE);
	WT_STAT_SET(cache->stats, cache_bytes_max, 30 * WT_MEGABYTE);

	return (0);

err:	__wt_cache_destroy(conn);
	return (ret);
}

/*
 * __wt_cache_stats_update --
 *	Update the cache statistics for return to the application.
 */
void
__wt_cache_stats_update(WT_CONNECTION_IMPL *conn)
{
	WT_CACHE *cache;

	cache = conn->cache;

	WT_STAT_SET(
	    cache->stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(
	    cache->stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
void
__wt_cache_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_SESSION_IMPL *session;
	WT_CACHE *cache;

	session = &conn->default_session;
	cache = conn->cache;

	if (cache == NULL)
		return;

	if (cache->mtx_evict != NULL)
		(void)__wt_mtx_destroy(session, cache->mtx_evict);
	if (cache->mtx_read != NULL)
		(void)__wt_mtx_destroy(session, cache->mtx_read);

	__wt_rec_destroy(session);

	__wt_free(session, cache->stats);
	__wt_free(session, conn->cache);
}
