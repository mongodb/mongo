/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#define	WT_BTREE_MAJOR_VERSION	1	/* Version */
#define	WT_BTREE_MINOR_VERSION	0

/*
 * WT_BTREE --
 *	A btree handle.
 */
struct __wt_btree {
	WT_RWLOCK *rwlock;		/* Lock for shared/exclusive ops */
	uint32_t   refcnt;		/* Sessions using this tree. */
	TAILQ_ENTRY(__wt_btree) q;	/* Linked list of handles */

	const char *name;		/* Logical name */
	const char *filename;		/* File name */
	const char *config;		/* Configuration string */

	enum {	BTREE_COL_FIX=1,	/* Fixed-length column store */
		BTREE_COL_VAR=2,	/* Variable-length column store */
		BTREE_ROW=3		/* Row-store */
	} type;				/* Type */

	const char *key_format;		/* Key format */
	const char *key_plan;		/* Key projection plan */
	const char *idxkey_format;	/* Index key format (hides primary) */
	const char *value_format;	/* Value format */
	const char *value_plan;		/* Value projection plan */
	uint8_t bitcnt;			/* Fixed-length field size in bits */

					/* Row-store comparison function */
	WT_COLLATOR *collator;          /* Comparison function */

	uint32_t key_gap;		/* Row-store prefix key gap */

	uint32_t allocsize;		/* Allocation size */
	uint32_t maxintlpage;		/* Internal page max size */
	uint32_t maxintlitem;		/* Internal page max item size */
	uint32_t maxleafpage;		/* Leaf page max size */
	uint32_t maxleafitem;		/* Leaf page max item size */

	void *huffman_key;		/* Key huffman encoding */
	void *huffman_value;		/* Value huffman encoding */

	/*
	 * Column-store: track the last record in the file, and keep the last
	 * page pinned in memory for fast appends, to a skiplist of appended
	 * entries.
	 */
	WT_PAGE *last_page;		/* Col-store append, last page */
	uint64_t last_recno;		/* Col-store append, last recno */
	WT_INSERT_HEAD **append;	/* Appended items */

	WT_PAGE *root_page;		/* Root page */
	WT_ADDR  root_addr;		/* Replacement root address */
	int	 root_update;		/* 0: free original root blocks
					   1: free saved root blocks and
					      update on close */

	void *block;			/* Block manager */
	u_int block_header;		/* Block manager header length */

	WT_PAGE *evict_page;		/* Eviction thread's location */

	WT_BTREE_STATS *stats;		/* Btree statistics */

#define	WT_BTREE_BULK		0x01	/* Bulk-load handle */
#define	WT_BTREE_EXCLUSIVE	0x02	/* Need exclusive access to handle */
#define	WT_BTREE_NO_LOCK	0x04	/* Do not lock the handle */
#define	WT_BTREE_OPEN		0x08	/* Handle is open */
#define	WT_BTREE_SALVAGE	0x10	/* Handle is for salvage */
#define	WT_BTREE_UPGRADE	0x20	/* Handle is for upgrade */
#define	WT_BTREE_VERIFY		0x40	/* Handle is for verify */
	uint32_t flags;
};

/*
 * In diagnostic mode we track the locations from which hazard references
 * were acquired.
 */
#ifdef HAVE_DIAGNOSTIC
#define	__wt_page_in(a, b, c)						\
	__wt_page_in_func(a, b, c, __FILE__, __LINE__)
#else
#define	__wt_page_in(a, b, c)						\
	__wt_page_in_func(a, b, c)
#endif

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
 * The minimum btree leaf and internal page sizes are 512B, the maximum 512MB.
 * (The maximum of 512MB is enforced by the software, it could be set as high
 * as 4GB.)
 */
#define	WT_BTREE_ALLOCATION_SIZE_MIN	512
#define	WT_BTREE_ALLOCATION_SIZE_MAX	(128 * WT_MEGABYTE)
#define	WT_BTREE_PAGE_SIZE_MAX		(512 * WT_MEGABYTE)

/*
 * Split page size calculation -- we don't want to repeatedly split every time
 * a new entry is added, so we split to a smaller-than-maximum page size.
 */
#define	WT_SPLIT_PAGE_SIZE(pagesize, allocsize, pct)			\
	WT_ALIGN(((uintmax_t)(pagesize) * (pct)) / 100, allocsize)

/*
 * Limit the maximum size of a single object to 4GB - 512B: in some places we
 * allocate memory to store objects plus associated data structures.  512B is
 * far more space than we ever need, but I'm not eager to debug any off-by-ones,
 * and storing a 4GB object in the file is flatly insane, anyway.
 *
 * Key and data item lengths are stored in 32-bit unsigned integers, meaning
 * the largest key or data item is 4GB (minus a few bytes).  Record numbers
 * are stored in 64-bit unsigned integers, meaning the largest record number
 * is "really, really big".
 */
#define	WT_BTREE_OBJECT_SIZE_MAX	(UINT32_MAX - 512)
