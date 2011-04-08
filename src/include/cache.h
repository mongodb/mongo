/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * WT_EVICT_LIST --
 *	Encapsulation of an eviction choice.
 */
struct __wt_evict_list {
	WT_REF	*ref;				/* WT_REF structure */
	BTREE	*btree;				/* Underlying file object */
};

/*
 * WT_REC_LIST --
 *	Information tracking a single page reconciliation.
 */
typedef struct {
	/*
	 * Each reconciliation function writes out some number of pages,
	 * normally one, occasionally more than one, and returns to its
	 * caller a list of addr/size pairs for the newly-written pages.
	 * That list is used to update the parent's references.  There's
	 * something hugely wrong if this list is ever longer than a few
	 * pages, that would make no sense at all (maybe, in a long-running
	 * system, an internal page might acquire that many entries!?)
	 * Dynamically allocated just in case.
	 */
	struct rec_list {
		WT_OFF_RECORD off;		/* Address, size, recno */

		/*
		 * The key for a row-store page; no column-store key is needed
		 * because the page's recno, stored in the WT_OFF_RECORD, is
		 * the column-store key.
		 */
		WT_BUF key;			/* Row key */

		int	 deleted;		/* Page deleted */
	} *list;
	u_int	 l_next;			/* Next list slot */
	u_int	 l_entries;			/* Total list slots */
	uint32_t l_allocated;			/* Bytes allocated */

	/*
	 * Reconciliation splits to a smaller-than-maximum page size when a
	 * split is required so that we don't repeatedly split a packed page.
	 *
	 * page_size is the number of bytes in the maximum page size.
	 * split_page_size is the page size of the split chunk.
	 */
	uint32_t page_size;			/* Maximum page size */
	uint32_t split_page_size;		/* Split page size */

	/*
	 * Instead of checking sizes all the time, we count down the number of
	 * times we'll approach a split boundary before we've gone to the end
	 * of the maximum page size.
	 *
	 * split_avail is the number of bytes available for each split chunk.
	 * split_count: count down to split
	 */
	uint32_t split_avail;			/* Split bytes available */
	uint32_t split_count;			/* Number of boundaries */

	/*
	 * We track the total number of entries in split chunks so we can
	 * easily figure out how many entries in the newest split chunk.
	 */
	uint32_t total_split_entries;		/* Total entries in splits */

	/*
	 * To keep from having to start building the page over when we reach
	 * the maximum page size, track the page information when we approach
	 * each split boundary.
	 */
	struct rec_save {
		uint64_t recno;			/* Split's starting record */
		uint32_t entries;		/* Split's entries */

		/*
		 * Start is the first byte in the split chunk; the difference
		 * between the next slot's first byte and this slot's first
		 * byte is the length of the split chunk.
		 */
		uint8_t *start;			/* Split's first byte */
	} *save;
	u_int s_next;				/* Next save slot */
	u_int s_entries;			/* Total save slots */

	WT_BUF *dsk_tmp;			/* Disk-image buffer */
	WT_BUF *expsort;			/* RLE expansion array buffer */

} WT_REC_LIST;

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
#define	WT_READ_REQ_ISEMPTY(r)						\
	((r)->session == NULL)
#define	WT_READ_REQ_SET(r, _session, _parent, _ref, _dsk_verify) do {	\
	(r)->parent = _parent;						\
	(r)->ref = _ref;						\
	(r)->dsk_verify = _dsk_verify;					\
	WT_MEMORY_FLUSH;	/* Flush before turning entry on */	\
	(r)->session = _session;					\
	WT_MEMORY_FLUSH;	/* Turn entry on */			\
} while (0)
#define	WT_READ_REQ_CLR(r) do {						\
	(r)->session = NULL;						\
	WT_MEMORY_FLUSH;	/* Turn entry off */			\
} while (0)

/*
 * WiredTiger cache structure.
 */
struct __wt_cache {
	/*
	 * The Db.sync method and cache eviction server both want to reconcile
	 * pages, and there are two problems: first, reconciliation updates
	 * parent pages, which means the Db.sync method and the cache eviction
	 * server might update the same parent page at the same time.  Second,
	 * the Db.sync method and cache eviction server may attempt to reconcile
	 * the same page at the same time which implies serialization anyway.
	 * We could probably handle that, but for now, I'm going to make page
	 * reconciliation single-threaded.
	 */
	WT_MTX *mtx_reconcile;		/* Single-thread page reconciliation */

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
	int only_evict_clean;

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

	uint32_t   read_gen;		/* Page read generation (LRU) */

	/* List of pages created from a single page reconciliation. */
	WT_REC_LIST reclist;

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
} while (0)
#define	WT_CACHE_PAGE_OUT(c, bytes) do {				\
	++(c)->stat_pages_out;						\
	(c)->stat_bytes_out += bytes;					\
} while (0)
	uint64_t stat_pages_in;
	uint64_t stat_bytes_in;
	uint64_t stat_pages_out;
	uint64_t stat_bytes_out;

	WT_EVICT_LIST *evict;		/* Pages being tracked for eviction */
	uint32_t evict_elem;		/* Number of elements in the array */
	uint32_t evict_len;		/* Bytes in the array */

	WT_HAZARD *hazard;		/* Copy of the hazard references */
	uint32_t   hazard_elem;		/* Number of entries in the list */
	uint32_t   hazard_len;		/* Bytes in the list */

	WT_CACHE_STATS *stats;		/* Cache statistics */
};
#if defined(__cplusplus)
}
#endif
