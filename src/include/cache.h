/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
#define	WT_EVICT_WALK_BASE	300	/* Pages tracked across file visits */
#define	WT_EVICT_WALK_INCR	100	/* Pages added each walk */

/*
 * WT_EVICT_ENTRY --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_entry {
	WT_BTREE *btree;		/* Enclosing btree object */
	WT_REF	 *ref;			/* Page to flush/evict */
	uint64_t  score;		/* Relative eviction priority */
};

#define	WT_EVICT_QUEUE_MAX	3	/* Two ordinary queues plus urgent */
#define	WT_EVICT_URGENT_QUEUE	2	/* Urgent queue index */

/*
 * WT_EVICT_QUEUE --
 *	Encapsulation of an eviction candidate queue.
 */
struct __wt_evict_queue {
	WT_SPINLOCK evict_lock;		/* Eviction LRU queue */
	WT_EVICT_ENTRY *evict_queue;	/* LRU pages being tracked */
	WT_EVICT_ENTRY *evict_current;	/* LRU current page to be evicted */
	uint32_t evict_candidates;	/* LRU list pages to evict */
	uint32_t evict_entries;		/* LRU entries in the queue */
	volatile uint32_t evict_max;	/* LRU maximum eviction slot used */
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
	 * at any specific time. However, even though the values don't have to
	 * be exact, they can't be garbage, we track what comes in and what goes
	 * out and calculate the difference as needed.
	 */
	uint64_t bytes_dirty_intl;	/* Bytes/pages currently dirty */
	uint64_t pages_dirty_intl;
	uint64_t bytes_dirty_leaf;
	uint64_t pages_dirty_leaf;
	uint64_t bytes_evict;		/* Bytes/pages discarded by eviction */
	volatile uint64_t pages_evict;
	uint64_t pages_evicted;		/* Pages evicted during a pass */
	uint64_t bytes_image;		/* Bytes of disk images */
	uint64_t bytes_inmem;		/* Bytes/pages in memory */
	uint64_t pages_inmem;
	uint64_t bytes_internal;	/* Bytes of internal pages */
	uint64_t bytes_read;		/* Bytes read into memory */
	uint64_t bytes_written;

	uint64_t app_waits;		/* User threads waited for cache */
	uint64_t app_evicts;		/* Pages evicted by user threads */
	uint64_t server_evicts;		/* Pages evicted by server thread */
	uint64_t worker_evicts;		/* Pages evicted by worker threads */

	uint64_t evict_max_page_size;	/* Largest page seen at eviction */
#ifdef	HAVE_DIAGNOSTIC
	struct timespec stuck_ts;	/* Stuck timestamp */
#endif

	/*
	 * Read information.
	 */
	uint64_t read_gen;		/* Current page read generation */
	uint64_t read_gen_oldest;	/* Oldest read generation the eviction
					 * server saw in its last queue load */
	uint64_t evict_pass_gen;	/* Number of eviction passes */

	/*
	 * Eviction thread information.
	 */
	WT_CONDVAR *evict_cond;		/* Eviction server condition */
	WT_SPINLOCK evict_walk_lock;	/* Eviction walk location */

	u_int eviction_dirty_target;    /* Percent to allow dirty */
	u_int eviction_dirty_trigger;	/* Percent to trigger dirty eviction */
	u_int eviction_trigger;		/* Percent to trigger eviction */
	u_int eviction_target;		/* Percent to end eviction */

	u_int eviction_checkpoint_target;/* Percent to reduce dirty
					   to during checkpoint scrubs */
	double eviction_scrub_limit;	/* Percent of cache to trigger
					   dirty eviction during checkpoint
					   scrubs */

	u_int overhead_pct;	        /* Cache percent adjustment */

	/*
	 * Pass interrupt counter.
	 */
	volatile uint32_t pass_intr;	/* Interrupt eviction pass. */

	/*
	 * LRU eviction list information.
	 */
	WT_SPINLOCK evict_pass_lock;	/* Eviction pass lock */
	WT_SESSION_IMPL *walk_session;	/* Eviction pass session */
	WT_DATA_HANDLE *evict_file_next;/* LRU next file to search */

	WT_SPINLOCK evict_queue_lock;	/* Eviction current queue lock */
	WT_EVICT_QUEUE evict_queues[WT_EVICT_QUEUE_MAX];
	WT_EVICT_QUEUE *evict_current_queue; /* LRU current queue in use */
	WT_EVICT_QUEUE *evict_fill_queue;    /* LRU next queue to fill.
						This is usually the same as the
						"other" queue but under heavy
						load the eviction server will
						start filling the current queue
						before it switches. */
	WT_EVICT_QUEUE *evict_other_queue;   /* LRU queue not in use */
	WT_EVICT_QUEUE *evict_urgent_queue;  /* LRU urgent queue */
	uint32_t evict_slots;		/* LRU list eviction slots */

#define	WT_EVICT_SCORE_BUMP	10
#define	WT_EVICT_SCORE_CUTOFF	10
#define	WT_EVICT_SCORE_MAX	100
	uint32_t evict_aggressive_score;/* Score of how aggressive eviction
					   should be about selecting eviction
					   candidates. If eviction is
					   struggling to make progress, this
					   score rises (up to a maximum of
					   100), at which point the cache is
					   "stuck" and transaction will be
					   rolled back. */
	uint32_t evict_empty_score;	/* Score of how often LRU queues are
					   empty on refill. This score varies
					   between 0 (if the queue hasn't been
					   empty for a long time) and 100 (if
					   the queue has been empty the last 10
					   times we filled up. */

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
	 * Flags.
	 */
#define	WT_CACHE_EVICT_CLEAN	  0x001 /* Evict clean pages */
#define	WT_CACHE_EVICT_CLEAN_HARD 0x002 /* Clean % blocking app threads */
#define	WT_CACHE_EVICT_DIRTY	  0x004 /* Evict dirty pages */
#define	WT_CACHE_EVICT_DIRTY_HARD 0x008 /* Dirty % blocking app threads */
#define	WT_CACHE_EVICT_SCRUB	  0x010 /* Scrub dirty pages */
#define	WT_CACHE_EVICT_URGENT	  0x020 /* Pages are in the urgent queue */
#define	WT_CACHE_EVICT_ALL	(WT_CACHE_EVICT_CLEAN | WT_CACHE_EVICT_DIRTY)
#define	WT_CACHE_EVICT_MASK	  0x0FF
#define	WT_CACHE_POOL_MANAGER	  0x100 /* The active cache pool manager */
#define	WT_CACHE_POOL_RUN	  0x200 /* Cache pool thread running */
	uint32_t flags;
};

#define	WT_WITH_PASS_LOCK(session, op) do {				\
	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOCKED_PASS));	\
	WT_WITH_LOCK_WAIT(session,					\
	    &cache->evict_pass_lock, WT_SESSION_LOCKED_PASS, op);	\
} while (0)

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
