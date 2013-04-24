/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some
 * number of pages from each file's in-memory tree for each page we evict.
 */
#define	WT_EVICT_INT_SKEW  (1<<12)	/* Prefer leaf pages over internal
					   pages by this many increments of the
					   read generation. */
#define	WT_EVICT_WALK_PER_FILE	10	/* Pages to visit per file */
#define	WT_EVICT_WALK_BASE     100	/* Pages tracked across file visits */
#define	WT_EVICT_WALK_INCR     100	/* Pages added each walk */

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
	uint64_t bytes_inmem;		/* Bytes/pages in memory */
	uint64_t pages_inmem;
	uint64_t bytes_evict;		/* Bytes/pages discarded by eviction */
	uint64_t pages_evict;
	uint64_t bytes_dirty;		/* Bytes/pages currently dirty */
	uint64_t pages_dirty;

	/*
	 * Read information.
	 */
	uint64_t   read_gen;		/* Page read generation (LRU) */

	/*
	 * Eviction thread information.
	 */
	WT_CONDVAR *evict_cond;		/* Cache eviction server mutex */
	WT_SPINLOCK evict_lock;		/* Eviction LRU queue */
	WT_SPINLOCK evict_walk_lock;	/* Eviction walk location */

	u_int eviction_trigger;		/* Percent to trigger eviction */
	u_int eviction_target;		/* Percent to end eviction */
	u_int eviction_dirty_target;    /* Percent to allow dirty */

	/*
	 * LRU eviction list information.
	 */
	WT_EVICT_ENTRY *evict;		/* LRU pages being tracked */
	WT_EVICT_ENTRY *evict_current;	/* LRU current page to be evicted */
	uint32_t evict_entries;		/* LRU list eviction slots */
	uint32_t evict_candidates;	/* LRU list pages to evict */
	u_int    evict_file_next;	/* LRU: next file to search */
	uint32_t force_entries;		/* Forced eviction page count */

	/*
	 * Sync/flush request information.
	 */
	volatile uint64_t sync_request;	/* File sync requests */
	volatile uint64_t sync_complete;/* File sync requests completed */

	/*
	 * Cache pool information.
	 */
	uint64_t cp_saved_evict;	/* Evict count from last pass */
	uint64_t cp_current_evict;	/* Evict count from current pass */
	uint32_t cp_skip_count;		/* Post change stabilization */
	uint64_t cp_reserved;		/* Base size for this cache */

	/*
	 * Flags.
	 */
#define	WT_EVICT_NO_PROGRESS	0x01	/* Check if pages are being evicted */
#define	WT_EVICT_STUCK		0x02	/* Eviction server is stuck */
	uint32_t flags;
};

/*
 * WT_CACHE_POOL --
 *	A structure that represents a shared cache.
 */
struct __wt_cache_pool {
	WT_SPINLOCK cache_pool_lock;
	pthread_t cache_pool_tid;
	WT_CONDVAR *cache_pool_cond;
	WT_SESSION_IMPL *session;
	const char *name;
	uint64_t size;
	uint64_t chunk;
	uint64_t currently_used;
	uint32_t flags;
	uint32_t refs;		/* Reference count for structure. */
	/* Locked: List of connections participating in the cache pool. */
	TAILQ_HEAD(__wt_cache_pool_qh, __wt_connection_impl) cache_pool_qh;
};
