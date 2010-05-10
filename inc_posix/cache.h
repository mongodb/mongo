/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.  All rights reserved.
 *
 * $Id$
 */

#if defined(__cplusplus)
extern "C" {
#endif

struct __wt_cache_entry;	typedef struct __wt_cache_entry WT_CACHE_ENTRY;
struct __wt_cache_hb;		typedef struct __wt_cache_hb WT_CACHE_HB;
struct __wt_read_req;		typedef struct __wt_read_req WT_READ_REQ;

/*
 * WT_READ_REQ --
 *	Encapsulation of a read request.
 */
struct __wt_read_req {
	WT_TOC	  *toc;			/* Requesting thread */
	u_int32_t  addr;		/* Address */
	u_int32_t  size;		/* Bytes */
	WT_CACHE_ENTRY			/* Cache slot */
		  *entry;
	WT_PAGE  **pagep;		/* Returned page */
};

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {

	/*
	 * The cache drain thread sets/clears the drain_sleeping flag when
	 * it's blocked on the mtx_server mutex.  The I/O thread checks the
	 * drain_sleeping value and wakes the cache drain thread as necessary.
	 * The drain_sleeping flag is declared volatile so the I/O thread
	 * doesn't cache a value.
	 */
	WT_MTX *mtx_server;		/* Cache server mutex */
	u_int volatile server_sleeping;	/* Cache server is sleeping */

	/*
	 * The I/O thread sets/clears the io_sleeping flag when it's blocked on
	 * the mtx_io mutex.  The workQ thread checks the io_sleeping value and
	 * wakes the I/O thread as necessary.   The io_sleeping flag is declared
	 * volatile so the workQ thread doesn't cache a value.
	 */
	WT_MTX *mtx_io;			/* Cache I/O server mutex */
	u_int volatile io_sleeping;	/* Cache I/O server is sleeping */

	/* Each in-memory page is in a hash bucket based on its file offset. */
#define	WT_HASH(cache, addr)	((addr) % (cache)->hb_size)

	WT_CACHE_HB *hb;		/* Array of hash buckets */
	u_int32_t    hb_size;		/* Number of hash buckets */

	WT_READ_REQ read_request[20];	/* Cache read requests:
					   slot available if toc is NULL. */

#define	WT_CACHE_PAGE_IN(c, bytes) do {					\
	WT_STAT_INCR(cache->stats, CACHE_PAGES_INUSE);			\
	WT_STAT_INCRV(cache->stats, CACHE_BYTES_INUSE, bytes);		\
} while (0);
#define	WT_CACHE_PAGE_OUT(c, bytes) do {				\
	WT_STAT_DECR(cache->stats, CACHE_PAGES_INUSE);			\
	WT_STAT_DECRV(cache->stats, CACHE_BYTES_INUSE, bytes);		\
} while (0);

	WT_CACHE_ENTRY **drain;		/* List of entries being drained */
	u_int32_t drain_elem;		/* Number of entries in the list */
	u_int32_t drain_len;		/* Bytes in the list */

	WT_PAGE **hazard;		/* Copy of the hazard references */
	u_int32_t hazard_elem;		/* Number of entries in the list */
	u_int32_t hazard_len;		/* Bytes in the list */

	u_int32_t bucket_cnt;		/* Drain review: last hash bucket */

	u_int8_t *recbuf;		/* Reconcilation buffer */
	u_int32_t recbuf_size;

	WT_STATS *stats;		/* Cache statistics */
};

/*
 * Hash bucket structure: a hash bucket holds an array of entries, each of
 * which references a page.
 *
 * Entries are only added by the I/O server thread (which may also grow the
 * entry array by allocating a new array, copying the old one over it, and
 * then updating the WT_CACHE_HB->entry field).
 *
 * Entries are only removed by the cache drain thread.
 */
struct __wt_cache_hb {
	WT_CACHE_ENTRY	*entry;		/* Array of cache pages */
	u_int32_t	 entry_size;	/* Number of cache pages */
};

/*
 * Hash bucket entry: each references a single page.  Logically, anything in
 * the WT_CACHE_ENTRY and WT_PAGE structures could be in either structure,
 * they both represent a page in memory.   However, we want to be able to
 * search arrays of WT_CACHE_ENTRY structures quickly because it's how we
 * find pages in the cache, and for that reason the WT_CACHE_ENTRY structures
 * are kept small.
 *
 * Items in a hash bucket entry
 *
 * There are three sets of threads accessing these entries: readers, the I/O
 * server, and the cache drain server.  In general, readers and the I/O server
 * behave the same, that is, they are both "readers" of information which is
 * only updated by the cache drain server.
 *
 * The synchronization between readers and the cache drain server is based on
 * the WT_CACHE_ENTRY->state field.  When readers find a WT_CACHE_ENTRY they
 * want, they set a hazard reference to the page, flush memory and re-confirm
 * the validity of the page.  If the page is still valid, they continue.
 *
 * When the cache drain server wants to remove a page from the cache, it sets
 * the state field to WT_DRAIN, flushes memory, then checks hazard references.
 * If the cache drain server finds a hazard reference, it resets the state
 * field to WT_OK, restoring the page to readers.  If the cache drain server
 * doesn't find a hazard reference, it knows the page is safe to discard.
 *
 * There is an additional synchronization between the cache drain server and
 * the I/O server; only the cache drain server can remove pages from the cache,
 * and only the I/O server can add pages to the cache.  The cache drain server
 * sets the state field to WT_EMPTY after it has discarded the page, and the
 * I/O server sets the state field to WT_OK when it's instantiated a new page.
 *
 * WT_CACHE_ENTRY_SET --
 *	Fill in everything but the state, flush, then fill in the state.  No
 * additional flush is necessary, the state field is declared volatile.  The
 * state turns on the entry for both the cache drain server thread as well as
 * any readers.
 */
#define	WT_CACHE_ENTRY_SET(e, _db, _page, _addr, _read_gen, _state) do {\
	(e)->db = _db;							\
	(e)->page = _page;						\
	(e)->addr = _addr;						\
	(e)->read_gen = _read_gen;					\
	(e)->write_gen = 0;						\
	WT_MEMORY_FLUSH;						\
	(e)->state = _state;						\
} while (0);

struct __wt_cache_entry {
	DB	 *db;			/* Page's backing database */
	WT_PAGE	 *page;			/* Page */

	u_int32_t addr;			/* Page's allocation address */

	/*
	 * We track two generation numbers: First, the page's read generation
	 * which we use to find pages which are no longer useful in the cache
	 * (it's our LRU value).  Second, the write generation, which we use
	 * to find pages which are clean and can be discarded.
	 */
	u_int32_t read_gen;		/* Read generation */
	u_int32_t write_gen;		/* Write generation */

	/*
	 * State is used in lots of different places, so declare it volatile to
	 * ensure we don't miss an explicit flush.
	 */
#define	WT_EMPTY	0		/* Nothing here, available for re-use */
#define	WT_DRAIN	1		/* Draining (requested) */
#define	WT_OK		2		/* In-use, valid data */
#define	WT_READ		3		/* Reading */
	u_int32_t volatile state;	/* State */
};

/*
 * WT_CACHE_FOREACH_PAGE --
 *	Macro to visit every page in a single hash bucket.
 */
#define	WT_CACHE_FOREACH_PAGE(cache, hb, e, i)				\
	for ((i) = 0, (e) = (hb)->entry;				\
	    (i) < (hb)->entry_size; ++(i), ++(e))

/*
 * WT_CACHE_FOREACH_PAGE_ALL --
 *	Macro to visit every page in the cache.
 */
#define	WT_CACHE_FOREACH_PAGE_ALL(cache, e, i, j)			\
	for ((i) = 0; (i) < (cache)->hb_size; ++(i))			\
		WT_CACHE_FOREACH_PAGE(cache, &(cache)->hb[i], e, j)

#if defined(__cplusplus)
}
#endif
