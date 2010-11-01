/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
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
	uint32_t i;
	int ret;

	ienv = env->ienv;
	ret = 0;

	WT_RET(__wt_calloc(env, 1, sizeof(WT_CACHE), &ienv->cache));
	cache = ienv->cache;

	WT_ERR(__wt_mtx_alloc(env, "cache drain server", 1, &cache->mtx_drain));
	WT_ERR(__wt_mtx_alloc(env, "cache read server", 1, &cache->mtx_read));
	WT_ERR(__wt_mtx_alloc(env, "reconciliation", 0, &cache->mtx_reconcile));

	/*
	 * Initialize the cache hash buckets.
	 *
	 * By default, size for a cache filled with 8KB pages, and 4 pages per
	 * bucket (or, 32 buckets per MB).
	 */
	cache->hb_size = env->cache_hash_size == 0 ?
	    __wt_prime(env->cache_size * 32) : env->cache_hash_size;
	WT_ERR(__wt_calloc(
	    env, cache->hb_size, sizeof(cache->hb[0]), &cache->hb));
	WT_VERBOSE(env, WT_VERB_CACHE, (env,
		"cache initialization: %lu MB, %lu hash buckets",
		(u_long)env->cache_size, (u_long)cache->hb_size));

	/*
	 * Create an array of WT_CACHE_ENTRY structures in each bucket -- we
	 * allocate 20 to start with, much larger than the 4 we calculated
	 * above.   Note we allocate one extra one, which serves as the fake
	 * entry, used to reference the next chunk of entries if we have to
	 * grow the structure.
	 */
	for (i = 0; i < cache->hb_size; ++i)
		WT_ERR(__wt_calloc(env, WT_CACHE_ENTRY_CHUNK + 1,
		    sizeof(WT_CACHE_ENTRY), &cache->hb[i]));

	WT_ERR(__wt_stat_alloc_cache_stats(env, &cache->stats));

	WT_STAT_SET(
	    cache->stats, CACHE_BYTES_MAX, env->cache_size * WT_MEGABYTE);
	WT_STAT_SET(
	    cache->stats, CACHE_MAX_BUCKET_ENTRIES, WT_CACHE_ENTRY_CHUNK);

	return (0);

err:	(void)__wt_cache_destroy(env);
	return (ret);
}

/*
 * __wt_cache_pages_inuse --
 *	Return the number of pages in use.
 */
uint64_t
__wt_cache_pages_inuse(WT_CACHE *cache)
{
	uint64_t pages_in, pages_out;

	/*
	 * Other threads of control may be modifying these fields -- we don't
	 * need exact values, but we do not want garbage, so read first, then
	 * use local variables for calculation, ensuring a reasonable return.
	 */
	pages_in = cache->stat_pages_in;
	pages_out = cache->stat_pages_out;
	return (pages_in > pages_out ? pages_in - pages_out : 0);
}

/*
 * __wt_cache_bytes_inuse --
 *	Return the number of bytes in use.
 */
uint64_t
__wt_cache_bytes_inuse(WT_CACHE *cache)
{
	uint64_t bytes_in, bytes_out;

	/*
	 * Other threads of control may be modifying these fields -- we don't
	 * need exact values, but we do not want garbage, so read first, then
	 * use local variables for calculation, ensuring a reasonable return.
	 */
	bytes_in = cache->stat_bytes_in;
	bytes_out = cache->stat_bytes_out;
	return (bytes_in > bytes_out ? bytes_in - bytes_out : 0);
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

	WT_STAT_SET(stats, CACHE_BYTES_INUSE, __wt_cache_bytes_inuse(cache));
	WT_STAT_SET(stats, CACHE_PAGES_INUSE, __wt_cache_pages_inuse(cache));
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
	WT_CACHE_ENTRY *e, *etmp;
	uint32_t i, j;
	int ret;

	ienv = env->ienv;
	cache = ienv->cache;
	ret = 0;

	if (cache == NULL)
		return (0);

	/*
	 * Discard all pages -- no hazard references needed, this is done when
	 * the environment is closed, after all threads of control have exited
	 * the cache.
	 *
	 * There shouldn't be any modified pages, because all of the databases
	 * have been closed.
	 */
	for (i = 0; i < cache->hb_size; ++i)
		for (j = WT_CACHE_ENTRY_CHUNK, e = cache->hb[i];;) {
			if (e->state != WT_EMPTY) {
				WT_ASSERT(env, !WT_PAGE_MODIFY_ISSET(e->page));
				__wt_bt_page_discard(env, e->page);
			}
			WT_CACHE_ENTRY_NEXT(e, j);
		}

	/* Discard all hash bucket WT_CACHE_ENTRY arrays. */
	for (i = 0; i < cache->hb_size; ++i) {
		e = cache->hb[i];
		do {
			etmp = (WT_CACHE_ENTRY *)e[WT_CACHE_ENTRY_CHUNK].db;
			__wt_free(env, e, WT_CACHE_ENTRY_CHUNK * sizeof(*e));
		} while ((e = etmp) != NULL);
	}

	/* Discard mutexes. */
	if (cache->mtx_drain != NULL)
		(void)__wt_mtx_destroy(env, cache->mtx_drain);
	if (cache->mtx_read != NULL)
		__wt_mtx_destroy(env, cache->mtx_read);
	if (cache->mtx_reconcile != NULL)
		__wt_mtx_destroy(env, cache->mtx_reconcile);

	/* Discard allocated memory, and clear. */
	__wt_free(env, cache->stats, 0);
	__wt_free(env, cache->hb, cache->hb_size * sizeof(cache->hb[0]));
	__wt_free(env, ienv->cache, sizeof(WT_CACHE));

	return (ret);
}

#ifdef HAVE_DIAGNOSTIC
static const char *__wt_cache_dump_state(WT_CACHE_ENTRY *);

/*
 * __wt_cache_dump --
 *	Dump a cache.
 */
void
__wt_cache_dump(ENV *env)
{
	IENV *ienv;
	WT_CACHE *cache;
	WT_CACHE_ENTRY *e;
	uint32_t i, j;

	ienv = env->ienv;
	cache = ienv->cache;

	__wt_msg(env, "cache dump (%llu pages)",
	    (unsigned long long)__wt_cache_pages_inuse(cache));

	for (i = 0; i < cache->hb_size; ++i) {
		__wt_msg(env, "==== cache bucket %lu", (u_long)i);
		for (j = WT_CACHE_ENTRY_CHUNK, e = cache->hb[i];;) {
			if (e->state != WT_EMPTY) {
				__wt_msg(env,
				    "{db: %p, addr: %6lu, state: %s}",
				    e->db,
				    (u_long)e->addr, __wt_cache_dump_state(e));
			}

			WT_CACHE_ENTRY_NEXT(e, j);
		}
	}
}

static const char *
__wt_cache_dump_state(WT_CACHE_ENTRY *e)
{
	switch (e->state) {
	case WT_EMPTY:
		return ("empty");
	case WT_DRAIN:
		return ("drain");
	case WT_OK:
		return ("OK");
	}
	return ("unknown");
}
#endif
