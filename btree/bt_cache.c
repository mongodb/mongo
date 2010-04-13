/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_cache_create --
 *	Create the underlying cache.
 */
int
__wt_cache_create(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_CACHE_HB *hb;
	u_int32_t i, j;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_RET(__wt_calloc(env, 1, sizeof(WT_CACHE), &ienv->cache));
	cache = ienv->cache;

	WT_ERR(__wt_mtx_alloc(env, "cache drain", 1, &cache->mtx_drain));
	WT_ERR(__wt_mtx_alloc(env, "cache I/O", 1, &cache->mtx_io));
	WT_ERR(__wt_mtx_alloc(env, "cache hash bucket", 0, &cache->mtx_hb));

	/*
	 * Initialize the cache hash buckets.
	 *
	 * By default, size for a cache filled with 8KB pages, and 4 pages per
	 * bucket (or, 32 buckets per MB).
	 */
	cache->hb_size = env->cache_hash_size == 0 ?
	    __wt_prime(env->cache_size * 32) : env->cache_hash_size;
	WT_ERR(
	    __wt_calloc(env, cache->hb_size, sizeof(WT_CACHE_HB), &cache->hb));
	WT_VERBOSE(env, WT_VERB_CACHE, (env,
		"cache initialization: %lu MB, %lu hash buckets",
		(u_long)env->cache_size, (u_long)cache->hb_size));

	/* Create an array of WT_CACHE_ENTRY structures in each bucket. */
	for (i = 0; i < cache->hb_size; ++i) {
		hb = &cache->hb[i];
		WT_ERR(__wt_calloc(env, WT_CACHE_ENTRY_DEFAULT,
		    sizeof(WT_CACHE_ENTRY), &hb->entry));
		hb->entry_size = WT_CACHE_ENTRY_DEFAULT;
		for (e = hb->entry, j = 0; j < WT_CACHE_ENTRY_DEFAULT; ++e, ++j)
			e->state = WT_EMPTY;
	}

	WT_ERR(__wt_stat_alloc_cache_stats(env, &cache->stats));

	WT_STAT_SET(
	    cache->stats, CACHE_BYTES_MAX, env->cache_size * WT_MEGABYTE);

	return (0);

err:	(void)__wt_cache_destroy(env);
	return (ret);
}

/*
 * __wt_cache_stats --
 *	Update the cache statistics for return to the application.
 */
void
__wt_cache_stats(ENV *env)
{
	WT_CACHE *cache;
	WT_STATS *stats;

	cache = env->ienv->cache;
	stats = cache->stats;

	WT_STAT_SET(stats, CACHE_BYTES_INUSE, WT_CACHE_BYTES_INUSE(cache));
	WT_STAT_SET(stats, CACHE_PAGES_INUSE, WT_CACHE_PAGES_INUSE(cache));
	WT_STAT_SET(stats, CACHE_HASH_BUCKETS, cache->hb_size);
}

/*
 * __wt_cache_destroy --
 *	Discard the underlying cache.
 */
int
__wt_cache_destroy(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	WT_CACHE_HB *hb;
	u_int32_t i, j;
	int ret;

	ienv = env->ienv;
	cache = ienv->cache;
	ret = 0;

	if (cache == NULL)
		return (0);

	/*
	 * Discard all pages -- no server support needed, this is done when the
	 * environment is closed, after all threads of control have exited the
	 * cache.
	 *
	 * There shouldn't be any modified pages, because all of the databases
	 * have been closed.
	 */
	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j)
		if (e->state != WT_EMPTY)
			__wt_bt_page_discard(env, e->page);

	/* Discard all hash bucket entries. */
	for (i = 0; i < cache->hb_size; ++i) {
		hb = &cache->hb[i];
		__wt_free(env,
		    hb->entry, hb->entry_size * sizeof(WT_CACHE_ENTRY));
	}

	/* Discard and destroy mutexes. */
	if (cache->mtx_drain != NULL)
		(void)__wt_mtx_destroy(env, cache->mtx_drain);
	if (cache->mtx_io != NULL)
		__wt_mtx_destroy(env, cache->mtx_io);
	if (cache->mtx_hb != NULL)
		__wt_mtx_destroy(env, cache->mtx_hb);

	/* Discard allocated memory, and clear. */
	__wt_free(env, cache->stats, 0);
	__wt_free(env, cache->hb, cache->hb_size * sizeof(WT_CACHE_HB));
	__wt_free(env, ienv->cache, sizeof(WT_CACHE));

	return (ret);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_cache_dump --
 *	Dump a cache.
 */
int
__wt_cache_dump(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	u_int32_t i, j;

	ienv = env->ienv;
	cache = ienv->cache;

	__wt_msg(env,
	    "cache dump (%llu pages): ==========", WT_CACHE_PAGES_INUSE(cache));

	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j)
		switch (e->state) {
		case WT_EMPTY:
			__wt_msg(env, "\tempty");
			break;
		case WT_OK:
		case WT_DRAIN:
			__wt_msg(env,
			    "\t%#lx {addr: %lu, bytes: %lu, state: %s}",
			    WT_PTR_TO_ULONG(e->page), (u_long)e->addr,
			    (u_long)e->page->bytes,
			    e->state == WT_OK ? "OK" : "cache-drain");
			break;
		}
	return (0);
}
#endif
