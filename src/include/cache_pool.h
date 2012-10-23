/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_CACHE_POOL --
 *	A structure that represents a shared cache.
 */
struct __wt_cache_pool {
	WT_SPINLOCK cache_pool_lock;
	pthread_t cache_pool_tid;
	WT_CONDVAR *cache_pool_cond;
	const char *name;
	uint64_t size;
	uint64_t quota;
	uint64_t chunk;
	uint64_t currently_used;
	uint32_t flags;
	TAILQ_HEAD(__wt_cache_pool_qh,
	    __wt_cache_pool_entry) cache_pool_qh;
};

/*
 * WT_CACHE_POOL_ENTRY --
 *	Each connection using the cache pool has an entry in the queue.
 */
struct __wt_cache_pool_entry {
	WT_CONNECTION_IMPL *conn;
	uint64_t cache_size;		/* Amount of cache assigned */
	uint64_t saved_evict;		/* Evict count from last pass */
	uint64_t current_evict;		/* Evict count from current pass */
	int32_t skip_count;		/* Post change stabilization */
	uint32_t active;		/* Whether to include the chunk */
	TAILQ_ENTRY(__wt_cache_pool_entry) q;
};
