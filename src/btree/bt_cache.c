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
__wt_cache_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_CACHE *cache;
	WT_CONFIG_ITEM cval;
	WT_SESSION_IMPL *session;
	int ret;

	session = &conn->default_session;
	ret = 0;

	WT_RET(__wt_calloc_def(session, 1, &conn->cache));
	cache = conn->cache;

	/* Configure the cache. */
	WT_ERR(__wt_config_gets(session, cfg, "eviction_target", &cval));
	cache->eviction_target = (u_int)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "eviction_trigger", &cval));
	cache->eviction_trigger = (u_int)cval.val;

	/*
	 * Sanity: the target size must be lower than the trigger size or we
	 * will never get any work done.
	 */
	if (cache->eviction_target >= cache->eviction_trigger)
		cache->eviction_target = cache->eviction_trigger - 1;

	WT_ERR(__wt_cond_alloc(session,
	    "cache eviction server", 1, &cache->evict_cond));

	/*
	 * Allocate the eviction request array.  We size it to allow one
	 * eviction request request per session.
	 */
	cache->max_evict_request = conn->session_size;
	WT_ERR(__wt_calloc_def(
	    session, cache->max_evict_request, &cache->evict_request));

	/*
	 * We pull some values from the cache statistics (rather than have two
	 * copies).   Set them.
	 */
	__wt_cache_stats_update(conn);

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

	WT_STAT_SET(conn->stats, cache_bytes_max, conn->cache_size);
	WT_STAT_SET(
	    conn->stats, cache_bytes_inuse, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(
	    conn->stats, cache_pages_inuse, __wt_cache_pages_inuse(cache));
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

	if (cache->evict_cond != NULL)
		(void)__wt_cond_destroy(session, cache->evict_cond);

	__wt_free(session, cache->evict_request);
	__wt_free(session, conn->cache);
}
