/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_EVICT_LIST --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_list {
	WT_BTREE *btree;			/* File object */
	WT_PAGE	 *page;				/* Page */
};

/*
 * WT_EVICT_REQ --
 *	Encapsulation of a eviction request.
 */
struct __wt_evict_req {
	WT_SESSION_IMPL *session;		/* Requesting thread */
	WT_BTREE *btree;			/* Btree */
	WT_PAGE *page;                          /* Single page to flush */

#define	WT_EVICT_REQ_CLOSE      0x1		/* Discard pages */
#define	WT_EVICT_REQ_PAGE       0x2		/* Force out a page */
	uint32_t flags;
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

	WT_SPINLOCK lru_lock;		/* Manage the eviction list. */

	WT_EVICT_LIST *evict;		/* Pages being tracked for eviction */
	WT_EVICT_LIST *evict_current;	/* Current page to be evicted */
	size_t   evict_allocated;	/* Bytes allocated */
	uint32_t evict_entries;		/* Total evict slots */

	u_int eviction_trigger;		/* Percent to trigger eviction. */
	u_int eviction_target;		/* Percent to end eviction. */

	WT_EVICT_REQ *evict_request;	/* Eviction requests:
					   slot available if session is NULL */
	uint32_t max_evict_request;	/* Size of the evict request array */
};
