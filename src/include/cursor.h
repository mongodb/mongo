/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_cursor_btree {
	WT_CURSOR iface;

	WT_BTREE *btree;		/* Enclosing btree */

	/*
	 * The following fields are set by the search functions as a precursor
	 * to page modification: we have a page, a WT_COL/WT_ROW slot on the
	 * page, an insert head and list and a skiplist stack (the stack of
	 * skiplist entries leading to the insert point).  The search functions
	 * also return if an exact match was found and a write-generation for
	 * the leaf page.
	 */
	WT_PAGE	  *page;		/* Current page */
	WT_COL	  *cip;			/* Col-store page slot */
	WT_ROW	  *rip;			/* Row-store page slot */
	uint32_t   slot;		/* WT_COL/WT_ROW 0-based slot */

	WT_INSERT_HEAD  *ins_head;	/* Insert chain head */
	WT_INSERT	*ins;		/* Insert list, skiplist stack */
	WT_INSERT	**ins_stack[WT_SKIP_MAXDEPTH];

	int	 match;			/* If search found an exact match */
	uint32_t write_gen;		/* Saved leaf page's write generation */

	/*
	 * If an application is doing a cursor walk, the following fields are
	 * additional information as to the cursor's location in the tree or
	 * are cached information used for cursors as they return items from
	 * the page.  These fields are not set by the search functions -- when
	 * the first cursor next/prev function is called, they are filled in,
	 * using the initial information returned by the search function.
	 */
	uint32_t nslots;		/* remaining page slots to return */

	/*
	 * We can't walk an insert list in reverse order, so we have to maintain
	 * a count of the current entry we're on.  For each iteration, we return
	  one entry earlier in the list.
	 */
	uint32_t ins_entry_cnt;		/* 1-based insert list entry count */

	/*
	 * The record number is calculated from the initial record number on the
	 * page, and offset for the slot at which we start.   Don't repeat that
	 * calculation, set it once and increment/decrement during traversal.
	 */
	uint64_t recno;			/* Cursor record number */

	/*
	 * Variable length column-store items are run-length encoded; the
	 * rel_return_count field is the item return count, set to the cell's
	 * initial unpacked value then decremented to 0.
	 *
	 * Variable length column-store items are optionally Huffman encoded;
	 * the value field is a buffer containing a copy of the item we're
	 * returning, so we don't repeatedly decode it if the RLE count is
	 * non-zero.
	 */
	uint64_t rle_return_cnt;	/* RLE count */
	WT_BUF  value;			/* Cursor value copy */

#define	WT_CBT_RET_INSERT	0x01	/* Return the current insert list */
#define	WT_CBT_RET_SLOT		0x02	/* Return the current slot */
#define	WT_CBT_SEARCH_SET	0x04	/* Search has set a page */
	uint32_t flags;
};

struct __wt_cursor_bulk {
	WT_CURSOR_BTREE cbt;

	uint8_t	 page_type;		/* Page type */
	uint64_t recno;			/* Total record number */
	uint32_t ipp;			/* Items per page */

	/*
	 * K/V pairs for row-store leaf pages, and V objects for column-store
	 * leaf pages, are stored in singly-linked lists (the lists are never
	 * searched, only walked at reconciliation, so it's not so bad).
	 */
	WT_INSERT  *ins_base;		/* Base insert link */
	WT_INSERT **insp;		/* Next insert link */
	WT_UPDATE  *upd_base;		/* Base update link */
	WT_UPDATE **updp;		/* Next update link */
	uint8_t	   *bitf;		/* Bit field */
	uint32_t   ins_cnt;		/* Inserts on the list */

	/*
	 * Bulk load dynamically allocates an array of leaf-page references;
	 * when the bulk load finishes, we build an internal page for those
	 * references.
	 */
	WT_ROW_REF *rref;		/* List of row leaf pages */
	WT_COL_REF *cref;		/* List of column leaf pages */
	uint32_t ref_next;		/* Next leaf page slot */
	uint32_t ref_entries;		/* Total leaf page slots */
	uint32_t ref_allocated;		/* Bytes allocated */
};

struct __wt_cursor_config {
	WT_CURSOR iface;
};

struct __wt_cursor_index {
	WT_CURSOR iface;

	WT_TABLE *table;
	const char *plan;
	WT_CURSOR **cg_cursors;
};

struct __wt_cursor_stat {
	WT_CURSOR iface;

	WT_STATS *stats_first;		/* First stats references */
	WT_STATS *stats;		/* Current stats reference */

	WT_BUF pvalue;			/* Current stats (pretty). */

	void (*clear_func)(WT_STATS *);	/* Function to clear stats. */
};

struct __wt_cursor_table {
	WT_CURSOR iface;

	WT_TABLE *table;
	const char *plan;
	WT_CURSOR **cg_cursors;
	WT_CURSOR **idx_cursors;
};
