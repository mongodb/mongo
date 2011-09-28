/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * WT_FREE_ENTRY  --
 *	Encapsulation of an entry on the Btree free list.
 */
struct __wt_free_entry {
	TAILQ_ENTRY(__wt_free_entry) qa;	/* Address queue */
	TAILQ_ENTRY(__wt_free_entry) qs;	/* Size queue */

	uint32_t addr;				/* Disk offset */
	uint32_t size;				/* Size */
};

/*
 * WT_SALVAGE_COOKIE --
 *	Encapsulation of salvage information for reconciliation.
 */
struct __wt_salvage_cookie {
	uint64_t missing;			/* Initial items to create */
	uint64_t skip;				/* Initial items to skip */
	uint64_t take;				/* Items to take */

	int	 done;				/* Ignore the rest */
};

/*
 * WT_NAMED_COMPRESSOR --
 *	A compressor list entry
 */
typedef struct __wt_named_compressor {
	TAILQ_ENTRY(__wt_named_compressor) q;	/* Linked list of compressors */
	const char *name;		/* Name of compressor */
	WT_COMPRESSOR *compressor;	/* User supplied callbacks */
} WT_NAMED_COMPRESSOR;

/*
 * Split page size calculation -- we don't want to repeatedly split every time
 * a new entry is added, so we split to a smaller-than-maximum page size.
 */
#define	WT_SPLIT_PAGE_SIZE(pagesize, allocsize, pct)			\
	WT_ALIGN(((uintmax_t)(pagesize) * (pct)) / 100, allocsize)

/*
 * WT_BTREE --
 *	A btree handle.
 */
struct __wt_btree {
	WT_RWLOCK *rwlock;		/* Lock for shared/exclusive ops. */
	uint32_t refcnt;		/* Sessions using this tree. */
	TAILQ_ENTRY(__wt_btree) q;	/* Linked list of handles */

	const char *name;		/* Logical name */
	const char *filename;		/* File name */
	const char *config;		/* Configuration string */

	enum {	BTREE_COL_FIX=1,	/* Fixed-length column store */
		BTREE_COL_VAR=2,	/* Variable-length column store */
		BTREE_ROW=3		/* Row-store */
	} type;				/* Type */

	uint8_t bitcnt;			/* Fixed-length field size in bits */

	const char *key_format;		/* Key format */
	const char *key_plan;		/* Key projection plan */
	const char *idxkey_format;	/* Index key format (hides primary) */
	const char *value_format;	/* Value format */
	const char *value_plan;		/* Value projection plan */

					/* Row-store comparison function */
	WT_COLLATOR *collator;          /* Comparison function */

	uint32_t key_gap;		/* Row-store prefix key gap */

	uint32_t allocsize;		/* Allocation size */
	uint32_t intlmax;		/* Internal page size max */
	uint32_t intlovfl;		/* Internal page overflow size */
	uint32_t leafmax;		/* Leaf page size max */
	uint32_t leafovfl;		/* Leaf page overflow size */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_value;		/* Value huffman encoding */

	WT_COMPRESSOR *compressor;	/* Page compressor */

	WT_REF	 root_page;		/* Root page reference */
	WT_FH	*fh;			/* Backing file handle */
	uint64_t lsn;			/* LSN file/offset pair */

	uint64_t freelist_bytes;	/* Free-list byte count */
	uint32_t freelist_entries;	/* Free-list entry count */
					/* Free-list queues */
	TAILQ_HEAD(__wt_free_qah, __wt_free_entry) freeqa;
	TAILQ_HEAD(__wt_free_qsh, __wt_free_entry) freeqs;
	int	 freelist_dirty;	/* Free-list has been modified */
	uint32_t free_addr;		/* Free-list addr/size pair */
	uint32_t free_size;

	WT_PAGE *evict_page;		/* Eviction thread's location */

	WT_PAGE *last_page;		/* Col-store append, last page */
	uint64_t last_recno;		/* Col-store append, last recno */

	void	*reconcile;		/* Reconciliation structure */

	WT_BTREE_STATS *stats;		/* Btree statistics */

#define	WT_BTREE_BULK		0x01	/* Handle is for bulk load. */
#define	WT_BTREE_EXCLUSIVE	0x02	/* Need exclusive access to handle */
#define	WT_BTREE_OPEN		0x08	/* Handle is open. */
#define	WT_BTREE_NO_EVICTION	0x10	/* Ignored by the eviction thread */
#define	WT_BTREE_NO_LOCK	0x20	/* Do not lock the handle. */
#define	WT_BTREE_SALVAGE	0x40	/* Handle is for salvage */
#define	WT_BTREE_VERIFY		0x80	/* Handle is for verify */
	uint32_t flags;
};

/*
 * In diagnostic mode we track the locations from which hazard references
 * were acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_page_in(a, b, c, d)					\
	__wt_page_in_func(a, b, c, d, __FILE__, __LINE__)
#else
#define	__wt_page_in(a, b, c, d)					\
	__wt_page_in_func(a, b, c, d)
#endif
