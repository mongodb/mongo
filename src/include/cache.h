/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_EVICT_ENTRY --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_entry {
	WT_BTREE *btree;			/* Enclosing btree object */
	WT_PAGE	 *page;				/* Page to flush/evict */
};

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
	/*
	 * Different threads read/write pages to/from the cache and create pages
	 * in the cache, so we cannot know precisely how much memory is in use
	 * at any specific time.  However, even though the values don't have to
	 * be exact, they can't be garbage, we track what comes in and what goes
	 * out and calculate the difference as needed.
	 */
	uint64_t bytes_read;		/* Bytes/pages read by read server */
	uint64_t pages_read;
	uint64_t bytes_inmem;		/* Bytes/pages created in memory */
	uint64_t bytes_evict;		/* Bytes/pages discarded by eviction */
	uint64_t pages_evict;

	/*
	 * Read information.
	 */
	uint32_t   read_gen;		/* Page read generation (LRU) */

	/*
	 * Eviction thread information.
	 */
	WT_CONDVAR *evict_cond;		/* Cache eviction server mutex */

	u_int eviction_trigger;		/* Percent to trigger eviction. */
	u_int eviction_target;		/* Percent to end eviction */

	/*
	 * LRU eviction list information.
	 */
	WT_SPINLOCK	lru_lock;	/* LRU serialization */
	WT_EVICT_ENTRY *evict;		/* LRU pages being tracked */
	WT_EVICT_ENTRY *evict_current;	/* LRU current page to be evicted */
	size_t   evict_allocated;	/* LRU list bytes allocated */
	uint32_t evict_entries;		/* LRU list eviction slots */

	/*
	 * Forced-page eviction request information.
	 */
	WT_EVICT_ENTRY *evict_request;	/* Forced page eviction request list */
	uint32_t max_evict_request;	/* Size of the eviction request array */

	/*
	 * Sync/flush request information.
	 */
	volatile uint64_t sync_request;	/* File sync requests */
	volatile uint64_t sync_complete;/* File sync requests completed */
};
