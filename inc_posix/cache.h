/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_cache_entry;	typedef struct __wt_cache_entry WT_CACHE_ENTRY;
struct __wt_read_req;		typedef struct __wt_read_req WT_READ_REQ;

/*
 * WT_READ_REQ --
 *	Encapsulation of a read request.
 */
struct __wt_read_req {
	WT_TOC *toc;				/* Requesting thread */
	WT_REF *ref;				/* Address */
	WT_OFF *off;				/* Bytes */
	int	dsk_verify;			/* Verify the disk image */
};
#define	WT_READ_REQ_ISEMPTY(r)						\
	((r)->toc == NULL)
#define	WT_READ_REQ_SET(r, _toc, _ref, _off, _dsk_verify) do {		\
	(r)->ref = _ref;						\
	(r)->off = _off;						\
	(r)->dsk_verify = _dsk_verify;					\
	WT_MEMORY_FLUSH;	/* Flush before turning entry on */	\
	(r)->toc = _toc;						\
	WT_MEMORY_FLUSH;	/* Turn entry on */			\
} while (0)
#define	WT_READ_REQ_CLR(r) do {						\
	(r)->toc = NULL;						\
	WT_MEMORY_FLUSH;	/* Turn entry off */			\
} while (0)

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
	/*
	 * The Db.sync method and the cache drain server both want to reconcile
	 * pages, and there are two problems: first, reconciliation updates
	 * parent pages, which means the Db.sync method and the cache drain
	 * server might update the same parent page at the same time.  Second,
	 * the Db.sync method and cache drain server might attempt to reconcile
	 * the same page at the same time which implies serialization anyway.
	 * We could probably handle that, but for now, I'm going to make page
	 * reconciliation single-threaded.
	 */
	WT_MTX *mtx_reconcile;		/* Single-thread page reconciliation */

	/*
	 * The cache thread sets/clears the drain_sleeping flag when blocked
	 * on the mtx_drain mutex.  The workQ thread uses the drain_sleeping
	 * flag to wake the cache drain thread as necessary.
	 */
	WT_MTX *mtx_drain;		/* Cache drain server mutex */
	u_int volatile drain_sleeping;	/* Sleeping */

	/*
	 * The I/O thread sets/clears the io_sleeping flag when blocked on the
	 * mtx_io mutex.  The cache thread uses the io_sleeping flag to wake
	 * the I/O thread as necessary.
	 */
	WT_MTX *mtx_read;		/* Cache read server mutex */
	u_int volatile read_sleeping;	/* Sleeping */
	u_int volatile read_lockout;	/* No reading until the cache drains */

	WT_READ_REQ read_request[40];	/* Read requests:
					   slot available if toc is NULL */

	uint32_t   read_gen;		/* Page read generation (LRU) */

	/*
	 * Different threads read/write pages to/from the cache, so we cannot
	 * know precisely how much memory is in use at any specific time.
	 * However, even though the values don't have to be exact, they can't
	 * be garbage -- we track what comes in and what goes out and calculate
	 * the difference as needed.
	 */
#define	WT_CACHE_PAGE_IN(c, bytes) do {					\
	++(c)->stat_pages_in;						\
	(c)->stat_bytes_in += bytes;					\
} while (0);
#define	WT_CACHE_PAGE_OUT(c, bytes) do {				\
	++(c)->stat_pages_out;						\
	(c)->stat_bytes_out += bytes;					\
} while (0);
	uint64_t stat_pages_in;
	uint64_t stat_bytes_in;
	uint64_t stat_pages_out;
	uint64_t stat_bytes_out;

	WT_CACHE_ENTRY **drain;		/* List of entries being drained */
	uint32_t drain_elem;		/* Number of entries in the list */
	uint32_t drain_len;		/* Bytes in the list */
	uint32_t bucket_cnt;		/* Drain review: last hash bucket */

	WT_PAGE **hazard;		/* Copy of the hazard references */
	uint32_t hazard_elem;		/* Number of entries in the list */
	uint32_t hazard_len;		/* Bytes in the list */

	WT_STATS *stats;		/* Cache statistics */
};
#if defined(__cplusplus)
}
#endif
