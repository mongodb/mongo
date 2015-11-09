/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * Tuning constants: I hesitate to call this tuning, but we want to review some
 * number of pages from each file's in-memory tree for each page we evict.
 */
#define	WT_EVICT_INT_SKEW  (1<<20)	/* Prefer leaf pages over internal
					   pages by this many increments of the
					   read generation. */
#define	WT_EVICT_WALK_PER_FILE	 10	/* Pages to queue per file */
#define	WT_EVICT_MAX_PER_FILE	100	/* Max pages to visit per file */
#define	WT_EVICT_WALK_BASE	300	/* Pages tracked across file visits */
#define	WT_EVICT_WALK_INCR	100	/* Pages added each walk */

/*
 * WT_EVICT_ENTRY --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_entry {
	WT_BTREE *btree;		/* Enclosing btree object */
	WT_REF	 *ref;			/* Page to flush/evict */
};

/*
 * WT_EVICT_WORKER --
 *	Encapsulation of an eviction worker thread.
 */
struct __wt_evict_worker {
	WT_SESSION_IMPL *session;
	u_int id;
	wt_thread_t tid;
#define	WT_EVICT_WORKER_RUN	0x01
	uint32_t flags;
};

/* Cache operations. */
typedef enum __wt_cache_op {
	WT_SYNC_CHECKPOINT,
	WT_SYNC_CLOSE,
	WT_SYNC_DISCARD,
	WT_SYNC_WRITE_LEAVES
} WT_CACHE_OP;

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
	uint64_t bytes_internal;	/* Bytes of internal pages */
	uint64_t bytes_overflow;	/* Bytes of overflow pages */
	uint64_t bytes_evict;		/* Bytes/pages discarded by eviction */
	uint64_t pages_evict;
	uint64_t bytes_dirty;		/* Bytes/pages currently dirty */
	uint64_t pages_dirty;
	uint64_t bytes_read;		/* Bytes read into memory */

	uint64_t app_evicts;		/* Pages evicted by user threads */
	uint64_t app_waits;		/* User threads waited for cache */

	uint64_t evict_max_page_size;	/* Largest page seen at eviction */

	/*
	 * Read information.
	 */
	uint64_t   read_gen;		/* Page read generation (LRU) */
	uint64_t   read_gen_oldest;	/* The oldest read generation that
					   eviction knows about */

	/*
	 * Eviction thread information.
	 */
	WT_CONDVAR *evict_cond;		/* Eviction server condition */
	WT_SPINLOCK evict_lock;		/* Eviction LRU queue */
	WT_SPINLOCK evict_walk_lock;	/* Eviction walk location */
	/* Condition signalled when the eviction server populates the queue */
	WT_CONDVAR *evict_waiter_cond;

	u_int eviction_trigger;		/* Percent to trigger eviction */
	u_int eviction_target;		/* Percent to end eviction */
	u_int eviction_dirty_target;    /* Percent to allow dirty */
	u_int eviction_dirty_trigger;	/* Percent to trigger dirty eviction */

	u_int overhead_pct;	        /* Cache percent adjustment */

	/*
	 * LRU eviction list information.
	 */
	WT_EVICT_ENTRY *evict_queue;	/* LRU pages being tracked */
	WT_EVICT_ENTRY *evict_current;	/* LRU current page to be evicted */
	uint32_t evict_candidates;	/* LRU list pages to evict */
	uint32_t evict_entries;		/* LRU entries in the queue */
	volatile uint32_t evict_max;	/* LRU maximum eviction slot used */
	uint32_t evict_slots;		/* LRU list eviction slots */
	WT_DATA_HANDLE
		*evict_file_next;	/* LRU next file to search */

	/*
	 * Cache pool information.
	 */
	uint64_t cp_pass_pressure;	/* Calculated pressure from this pass */
	uint64_t cp_quota;		/* Maximum size for this cache */
	uint64_t cp_reserved;		/* Base size for this cache */
	WT_SESSION_IMPL *cp_session;	/* May be used for cache management */
	uint32_t cp_skip_count;		/* Post change stabilization */
	wt_thread_t cp_tid;		/* Thread ID for cache pool manager */
	/* State seen at the last pass of the shared cache manager */
	uint64_t cp_saved_app_evicts;	/* User eviction count at last review */
	uint64_t cp_saved_app_waits;	/* User wait count at last review */
	uint64_t cp_saved_read;		/* Read count at last review */

	/*
	 * Work state.
	 */
#define	WT_EVICT_PASS_AGGRESSIVE	0x01
#define	WT_EVICT_PASS_ALL		0x02
#define	WT_EVICT_PASS_DIRTY		0x04
#define	WT_EVICT_PASS_WOULD_BLOCK	0x08
	uint32_t state;

	/*
	 * Flags.
	 */
#define	WT_CACHE_POOL_MANAGER	0x01	/* The active cache pool manager */
#define	WT_CACHE_POOL_RUN	0x02	/* Cache pool thread running */
#define	WT_CACHE_CLEAR_WALKS	0x04	/* Clear eviction walks */
#define	WT_CACHE_STUCK		0x08	/* Eviction server is stuck */
#define	WT_CACHE_WALK_REVERSE	0x10	/* Scan backwards for candidates */
#define	WT_CACHE_WOULD_BLOCK	0x20	/* Pages that would block apps */
	uint32_t flags;
};

/*
 * WT_CACHE_POOL --
 *	A structure that represents a shared cache.
 */
struct __wt_cache_pool {
	WT_SPINLOCK cache_pool_lock;
	WT_CONDVAR *cache_pool_cond;
	const char *name;
	uint64_t size;
	uint64_t chunk;
	uint64_t quota;
	uint64_t currently_used;
	uint32_t refs;		/* Reference count for structure. */
	/* Locked: List of connections participating in the cache pool. */
	TAILQ_HEAD(__wt_cache_pool_qh, __wt_connection_impl) cache_pool_qh;

	uint8_t pool_managed;		/* Cache pool has a manager thread */

#define	WT_CACHE_POOL_ACTIVE	0x01	/* Cache pool is active */
	uint8_t flags;
};
