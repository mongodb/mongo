/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

#define	WT_REC_CLOSE	1			/* Reconcile callers */
#define	WT_REC_EVICT	2
#define	WT_REC_SYNC	3

/*
 * WT_EVICT_LIST --
 *	Encapsulation of an eviction candidate.
 */
struct __wt_evict_list {
	WT_REF	*ref;				/* WT_REF structure */
	BTREE	*btree;				/* Underlying file object */
};

/*
 * WT_EVICT_REQ --
 *	Encapsulation of a eviction request.
 */
struct __wt_evict_req {
	SESSION *session;			/* Requesting thread */
	BTREE	*btree;				/* Btree */
	int	 close_method;			/* Discard pages */

	WT_PAGE **retry;			/* Pages to retry */
	uint32_t  retry_next;			/* Next retry slot */
	uint32_t  retry_entries;		/* Total retry slots */
	uint32_t  retry_allocated;		/* Bytes allocated */
};

/*
 * WT_READ_REQ --
 *	Encapsulation of a read request.
 */
struct __wt_read_req {
	SESSION *session;			/* Requesting thread */
	WT_PAGE *parent;			/* Parent */
	WT_REF  *ref;				/* Reference/Address */
	int	 dsk_verify;			/* Verify the disk image */
};

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
	/*
	 * The cache thread sets/clears the evict_sleeping flag when blocked
	 * on the mtx_evict mutex.  The workQ thread uses the evict_sleeping
	 * flag to wake the cache eviction thread as necessary.
	 */
	WT_MTX *mtx_evict;		/* Cache eviction server mutex */
	u_int volatile evict_sleeping;	/* Sleeping */

	/*
	 * The verification code wants to ensure every fragment in the file is
	 * verified exactly once.  The problem is that if eviction runs during
	 * verification, it's possible for a fragment to be free'd and verified
	 * twice (once while in the tree, and once while on the free-list), or
	 * to be free'd and never verified (if the check of the free-list races
	 * with the eviction), and so on and so forth.  For that reason, we turn
	 * off reconciliation of dirty pages while verification is running.
	 */
	int volatile only_evict_clean;

	/*
	 * The I/O thread sets/clears the read_sleeping flag when blocked on the
	 * mtx_read mutex.  The cache thread uses the read_sleeping flag to wake
	 * the I/O thread as necessary.
	 */
	WT_MTX *mtx_read;		/* Cache read server mutex */
	u_int volatile read_sleeping;	/* Sleeping */
	u_int volatile read_lockout;	/* No reading until memory drains */

	WT_READ_REQ read_request[40];	/* Read requests:
					   slot available if session is NULL */

	WT_EVICT_REQ evict_request[20];	/* Eviction requests:
					   slot available if session is NULL */

	/*
	 * File sync can temporarily fail when a tree is active, that is, we may
	 * not be able to immediately reconcile all of the file's pages.  If the
	 * pending_retry value is non-zero, it means there are pending requests
	 * we need to handle.
	 */
	int pending_retry;		/* Eviction request needs completion */

	uint32_t   read_gen;		/* Page read generation (LRU) */

	void	  *rec;			/* Page reconciliation structure */

	/*
	 * Different threads read/write pages to/from the cache, so we cannot
	 * know precisely how much memory is in use at any specific time.
	 * However, even though the values don't have to be exact, they can't
	 * be garbage -- we track what comes in and what goes out and calculate
	 * the difference as needed.
	 */
	uint64_t pages_in;
	uint64_t bytes_in;
	uint64_t pages_out;
	uint64_t bytes_out;

	WT_EVICT_LIST *evict;		/* Pages being tracked for eviction */
	uint32_t evict_entries;		/* Total evict slots */
	uint32_t evict_allocated;	/* Bytes allocated */

	WT_HAZARD *hazard;		/* Copy of the hazard references */
	uint32_t   hazard_elem;		/* Number of entries in the list */

	WT_CACHE_STATS *stats;		/* Cache statistics */
};
#if defined(__cplusplus)
}
#endif
