/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * WT_EVICT_LIST --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_list {
	WT_PAGE	 *page;				/* Page */
	WT_BTREE *btree;			/* Underlying file object */
};

/*
 * WT_EVICT_REQ --
 *	Encapsulation of a eviction request.
 */
struct __wt_evict_req {
	WT_SESSION_IMPL *session;		/* Requesting thread */
	WT_BTREE *btree;			/* Btree */

	WT_PAGE **retry;			/* Pages to retry */
	uint32_t  retry_next;			/* Next retry slot */
	uint32_t  retry_entries;		/* Total retry slots */
	size_t    retry_allocated;		/* Bytes allocated */
	int	  retry_cnt;			/* We only try a few times. */

	int	  close_method;			/* Discard pages */
};

/*
 * WT_READ_REQ --
 *	Encapsulation of a read request.
 */
struct __wt_read_req {
	WT_SESSION_IMPL *session;		/* Requesting thread */
	WT_PAGE *parent;			/* Parent */
	WT_REF  *ref;				/* Reference/Address */
	int	 dsk_verify;			/* Verify the disk image */
};

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
	WT_MTX *mtx_evict;		/* Cache eviction server mutex */

	/*
	 * File sync can temporarily fail when a tree is active, that is, we may
	 * not be able to immediately reconcile all of the file's pages.  If the
	 * pending_retry value is non-zero, it means there are pending requests
	 * we need to handle.
	 */
	int pending_retry;		/* Eviction request needs completion */

	WT_EVICT_LIST *evict;		/* Pages being tracked for eviction */
	size_t   evict_allocated;	/* Bytes allocated */
	uint32_t evict_entries;		/* Total evict slots */

	WT_EVICT_REQ evict_request[20];	/* Eviction requests:
					   slot available if session is NULL */

	WT_MTX *mtx_read;		/* Cache read server mutex */
	u_int volatile read_lockout;	/* No reading until memory drains */

	WT_READ_REQ read_request[40];	/* Read requests:
					   slot available if session is NULL */

	uint32_t   read_gen;		/* Page read generation (LRU) */

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
};
