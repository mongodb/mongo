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
	WT_PAGE  **pagep;		/* Returned page */
};
#define	WT_READ_REQ_SET(r, _toc, _addr, _size, _pagep) do {		\
	(r)->addr = _addr;						\
	(r)->size = _size;						\
	(r)->pagep = _pagep;						\
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
	 * In-memory database pages are in a hash bucket based on their file
	 * address; the hash bucket references an array of WT_CACHE_ENTRY
	 * structures, each of which references a single database page.
	 *
	 * The WT_CACHE_ENTRY array may have to grow, which is hard, because
	 * different threads modify the state of elements in the array, and we
	 * don't want to acquire a lock on it.  To make this work, the last
	 * element of the WT_CACHE_ENTRY array is a fake entry, that points
	 * to a subsequent WT_CACHE_ENTRY array (it's really a forward-linked
	 * list, albeit an odd one), which allows us to extend the array without
	 * replacing it.
	 */
#define	WT_ADDR_HASH(cache, addr)	((addr) % (cache)->hb_size)
#define	WT_CACHE_ENTRY_CHUNK	20	/* Entries in a WT_CACHE_ENTRY array */
	WT_CACHE_ENTRY **hb;		/* Array of hash buckets */
	u_int32_t	 hb_size;	/* Number of hash buckets */

	u_int32_t	 lru;		/* LRU generation number */

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
	u_int64_t stat_pages_in;
	u_int64_t stat_bytes_in;
	u_int64_t stat_pages_out;
	u_int64_t stat_bytes_out;

	WT_CACHE_ENTRY **drain;		/* List of entries being drained */
	u_int32_t drain_elem;		/* Number of entries in the list */
	u_int32_t drain_len;		/* Bytes in the list */
	u_int32_t bucket_cnt;		/* Drain review: last hash bucket */

	WT_PAGE **hazard;		/* Copy of the hazard references */
	u_int32_t hazard_elem;		/* Number of entries in the list */
	u_int32_t hazard_len;		/* Bytes in the list */

	WT_STATS *stats;		/* Cache statistics */
};

/*
 * Hash bucket entry: each references a single page.  Logically, anything in
 * the WT_CACHE_ENTRY and WT_PAGE structures could be in either structure,
 * they both represent a page in memory.   However, we want to be able to
 * search arrays of WT_CACHE_ENTRY structures fast because it's how we find
 * pages in the cache, and for that reason the WT_CACHE_ENTRY structures are
 * as small as possible -- the more of a hash bucket we get on each memory
 * read, the better off we are.
 *
 * There are lots of threads accessing these entries, including both readers
 * and writers.   Readers are usually application threads reading database
 * pages, or perhaps calling the Db.sync method.  Writers are library threads
 * doing I/O into the cache.
 *
 * The reader/write synchronization is based on the WT_CACHE_ENTRY->state field:
 * if the state is WT_EMPTY, there is no entry, and the entry is available for
 * use.  Readers first check for a valid entry (the db and addr fields match),
 * and then look at the state field.   If the state is WT_OK, They set a hazard
 * reference to the page, flush memory and re-confirm the validity of the page.
 * If the page is still valid, they have a reference and proceed.
 *
 * When the cache drain server wants to discard a page from the cache, it sets
 * the WT_CACHE_ENTRY->state field to WT_DRAIN, flushes memory, then checks
 * hazard references.  If the cache drain server finds a hazard reference, it
 * resets the state field to WT_OK, restoring the page to the readers.  If the
 * drain server doesn't find a hazard reference, the page is safe to discard.
 */
struct __wt_cache_entry {
	DB	 *db;			/* Page's backing database */
	u_int32_t addr;			/* Page's allocation address */
	WT_PAGE	 *page;			/* Page */

	/*
	 * Page state --
	 * WT_EMPTY
	 *	Slot empty, available for use by the reading thread.
	 * WT_DRAIN
	 *	The cache drain server selected this page to discard, and is
	 *	checking hazard references: readers wait.  If a hazard
	 *	reference is found, the page reverts to WT_OK, otherwise it's
	 *	discarded and goes to WT_EMPTY.
	 * WT_OK
	 *	In-use, valid data.
	 *
	 * The state is used to communicate between threads, so it's volatile;
	 * if we're changing it, we want everybody to know.
	 */
#define	WT_EMPTY	0		/* 0 so cleared memory works */
#define	WT_DRAIN	1
#define	WT_OK		2
	u_int32_t volatile state;
};
#define	WT_CACHE_ENTRY_SET(e, _db, _addr, _page, _state) do {		\
	(e)->db = _db;							\
	(e)->addr = _addr;						\
	(e)->page = _page;						\
	WT_MEMORY_FLUSH;	/* Flush before setting state */	\
	(e)->state = _state;	/* Volatile, no flush needed */		\
} while (0);
#define	WT_CACHE_ENTRY_CLR(e)						\
	WT_CACHE_ENTRY_SET(e, NULL, WT_ADDR_INVALID, NULL, WT_EMPTY)

/*
 * WT_CACHE_ENTRY_NEXT --
 *	The array of WT_CACHE_ENTRY is really a strange forward-linked list.
 * There's an extra WT_CACHE_ENTRY structure at the end, and the DB handle
 * field is overloaded to reference the next array of WT_CACH_ENTRY structs.
 * The WT_CACHE_ENTRY_NEXT macro encapsulates that magic.
 */
#define	WT_CACHE_ENTRY_NEXT(e, iter)					\
	++(e);								\
	if (--(iter) == 0) {						\
		if ((e = (WT_CACHE_ENTRY *)(e)->db) == NULL)		\
			break;						\
		iter = WT_CACHE_ENTRY_CHUNK;				\
	}
#if defined(__cplusplus)
}
#endif
