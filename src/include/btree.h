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
 * WT_WALK_ENTRY --
 *	Node for walking one level of a tree.
 */
struct __wt_walk_entry {
	WT_PAGE	*page;		/* Page being traversed */
	uint32_t indx;		/* Page's next child slot to return */

	int	 child;		/* If all children have been returned */
	int	 visited;	/* If the page itself has been returned */

	WT_PAGE *hazard;	/* Last page returned -- has hazard reference */
};

/*
 * WT_WALK --
 *	Structure describing a position during a walk through a tree.
 */
struct __wt_walk {
	WT_WALK_ENTRY *tree;

	size_t   tree_len;	/* Tree stack in bytes */
	u_int	 tree_slot;	/* Current tree stack slot */

	uint32_t flags;		/* Flags specified for the walk */
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
	const char *value_format;	/* Value format */
	const char *key_plan;		/* Key projection plan for indices */
	const char *value_plan;		/* Value projection plan for indices */

					/* Row-store comparison function */
	int (*btree_compare)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	uint32_t key_gap;		/* Row-store prefix key gap */

	uint32_t intlitemsize;		/* Maximum item size for overflow */
	uint32_t leafitemsize;

	uint32_t allocsize;		/* Allocation size */
	uint32_t intlmin;		/* Min/max internal page size */
	uint32_t intlmax;
	uint32_t leafmin;		/* Min/max leaf page size */
	uint32_t leafmax;

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

	WT_BUF   key_srch;		/* Search key buffer */

	WT_WALK  evict_walk;		/* Eviction thread's walk state */
	WT_PAGE *evict_page;		/* Eviction thread's page */

	WT_PAGE *last_page;		/* Col-store append, last page */
	uint64_t last_recno;		/* Col-store append, last recno */

	void	*reconcile;		/* Reconciliation structure */

	WT_BTREE_STATS *stats;		/* Btree statistics */

#define	WT_BTREE_BULK		0x01	/* Handle is for bulk load. */
#define	WT_BTREE_EXCLUSIVE	0x02	/* Need exclusive access to handle */
#define	WT_BTREE_NO_EVICTION	0x04	/* Ignored by the eviction thread */
#define	WT_BTREE_NO_LOCK	0x08	/* Do not lock the handle. */
#define	WT_BTREE_SALVAGE	0x10	/* Handle is for salvage */
#define	WT_BTREE_VERIFY		0x20	/* Handle is for verify */
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
