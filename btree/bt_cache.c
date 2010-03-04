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

	ienv = env->ienv;
	cache = &ienv->cache;

	WT_RET(__wt_mtx_init(&cache->mtx));	/* Cache server mutex */
	__wt_lock(env, &cache->mtx);		/* Blocking mutex */

	WT_STAT_SET(
	    ienv->stats, CACHE_BYTES_MAX, env->cache_size * WT_MEGABYTE);

	/*
	 * Initialize the cache page queues.  No server support needed, this is
	 * done when the environment is first opened, before there are multiple
	 * threads of control using the cache.
	 *
	 * We don't sort the hash queues in page LRU order because that requires
	 * manipulating the linked list as part of each read operation.  As a
	 * result, WiredTiger is much more sensitive to long bucket chains than
	 * Berkeley DB, and the bucket chains need to be short to avoid spending
	 * all our time walking the linked list.  To help, we do put the bucket
	 * into LRU order when looking for pages to evict.
	 *
	 * By default, size for a cache filled with 8KB pages, and 4 pages per
	 * bucket (or, 32 buckets per MB).
	 */
	cache->hash_size = env->cache_hash_size;
	if (cache->hash_size == WT_CACHE_HASH_SIZE_DEFAULT)
		cache->hash_size = __wt_prime(env->cache_size * 32);
	WT_STAT_SET(ienv->stats, HASH_BUCKETS, cache->hash_size);

	WT_RET(
	    __wt_calloc(env, cache->hash_size, sizeof(WT_PAGE *), &cache->hb));

	F_SET(cache, WT_INITIALIZED);
	return (0);
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
	WT_PAGE *page;
	u_int i;
	int ret;

	ienv = env->ienv;
	cache = &ienv->cache;
	ret = 0;

	if (!F_ISSET(cache, WT_INITIALIZED))
		return (0);

	/* Diagnostic check: check flags against approved list. */
	WT_ENV_FCHK_RET(env, "Env.close", cache->flags, WT_APIMASK_CACHE, ret);

	/*
	 * Discard all pages.  No server support needed, this is done when the
	 * environment is closed, after all threads of control have exited the
	 * cache.
	 *
	 * There shouldn't be any modified pages, because all of the databases
	 * have been closed.
	 */
	for (i = 0; i < cache->hash_size; ++i)
		while ((page = cache->hb[i]) != NULL) {
			__wt_cache_discard(env, page);
			__wt_bt_page_recycle(env, page);
		}

	/* There shouldn't be any allocated bytes. */
	WT_ASSERT(env, WT_STAT(ienv->stats, CACHE_BYTES_INUSE) == 0);

	/* Discard allocated memory, and clear. */
	__wt_free(env, cache->hb, cache->hash_size * sizeof(WT_PAGE *));
	memset(cache, 0, sizeof(cache));

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
	WT_PAGE *page;
	u_int32_t i;

	ienv = env->ienv;
	cache = &ienv->cache;

	__wt_msg(env, "cache dump (%llu pages): ==================",
	    WT_STAT(ienv->stats, CACHE_PAGES));

	for (i = 0; i < cache->hash_size; ++i) {
		__wt_msg(env, "hash bucket %d:", i);
		for (page = cache->hb[i]; page != NULL; page = page->next)
			__wt_msg(env, "\t%#lx {addr: %lu, bytes: %lu}",
			    WT_ADDR_TO_ULONG(page), (u_long)page->addr,
			    (u_long)page->bytes);
	}

	return (0);
}
#endif
