/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "btree.i"

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(CONNECTION *conn)
{
	WT_CACHE *cache;
	SESSION *session;
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
	    cache->stats, CACHE_BYTES_MAX, conn->cache_size * WT_MEGABYTE);

	return (0);

err:	(void)__wt_cache_destroy(conn);
	return (ret);
}

/*
 * __wt_cache_stats --
 *	Update the cache statistics for return to the application.
 */
void
__wt_cache_stats(CONNECTION *conn)
{
	WT_CACHE *cache;
	WT_STATS *stats;

	cache = conn->cache;
	stats = cache->stats;

	WT_STAT_SET(stats, CACHE_BYTES_INUSE, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(stats, CACHE_PAGES_INUSE, __wt_cache_pages_inuse(cache));
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
int
__wt_cache_destroy(CONNECTION *conn)
{
	SESSION *session;
	WT_CACHE *cache;
	WT_REC_LIST *reclist;
	struct rec_list *r_list;
	uint32_t i;
	int ret;

	session = &conn->default_session;
	cache = conn->cache;
	ret = 0;

	if (cache == NULL)
		return (0);

	if (cache->mtx_evict != NULL)
		(void)__wt_mtx_destroy(session, cache->mtx_evict);
	if (cache->mtx_read != NULL)
		(void)__wt_mtx_destroy(session, cache->mtx_read);

	reclist = &cache->reclist;
	if (reclist->list != NULL) {
		for (r_list = reclist->list,
		    i = 0; i < reclist->l_entries; ++r_list, ++i)
			__wt_buf_free(session, &r_list->key);
		__wt_free(session, reclist->list);
	}
	if (reclist->save != NULL)
		__wt_free(session, reclist->save);
	__wt_free(session, cache->stats);
	__wt_free(session, conn->cache);

	return (ret);
}
