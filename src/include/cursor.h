/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_cursor_btree {
	WT_CURSOR iface;

	WT_BTREE *btree;			/* Enclosing btree */

	/*
	 * WT_WALK is the stack of pages to the current cursor location, a
	 * page.
	 */
	WT_WALK	  walk;				/* Current walk */
	WT_PAGE	 *page;				/* Current page */

	/* The following fields give us a location within a single page. */
	WT_COL	  *cip;				/* Col-store page slot */
	WT_ROW	  *rip;				/* Row-store page slot */
	uint32_t  nslots;			/* Counting page slots */

	/*
	 * Insert lists override the slot, that is, if the insert list is
	 * set, then we're walking an insert list, and ignoring the slot.
	 */
	WT_INSERT_HEAD *ins_head;		/* Insert chain head */
	WT_INSERT *ins;				/* Insert chain */
	/*
	 * We can't walk an insert list in reverse order.  We count the insert
	 * list entries, then repeatedly walk the list in the forward direction,
	 * each time returning one entry earlier in the list.
	 */
	uint32_t   ins_prev_cnt;		/* Insert chain counting back */

	/*
	 * The following fields are cached information when returning items
	 * from the page.
	 */
	uint64_t recno;				/* Cursor record number */

	/*
	 * Column-store variable length items are optionally run-length encoded.
	 * WT_CURSOR_BTREE->rle is item return count, decremented to 0.
	 *
	 * Column-store variable length items are optionally Huffman encoded.
	 * WT_CURSOR_BTREE->value is a buffer that contains a copy of the item
	 * we're returning, so we don't repeatedly decode them if the RLE count
	 * is non-zero.  The buffer is NULL if the item was deleted.
	 */
	uint64_t rle_return_cnt;		/* RLE count */
	WT_BUF  value;				/* Cursor value copy */
};

struct __wt_cursor_bulk {
	WT_CURSOR_BTREE cbt;

	uint8_t	 page_type;			/* Page type */
	uint64_t recno;				/* Total record number */
	uint32_t ipp;				/* Items per page */

	/*
	 * K/V pairs for row-store leaf pages, and V objects for column-store
	 * leaf pages, are stored in singly-linked lists (the lists are never
	 * searched, only walked at reconciliation, so it's not so bad).
	 */
	WT_INSERT  *ins_base;			/* Base insert link */
	WT_INSERT **insp;			/* Next insert link */
	WT_UPDATE  *upd_base;			/* Base update link */
	WT_UPDATE **updp;			/* Next update link */
	uint8_t	   *bitf;			/* Bit field */
	uint32_t   ins_cnt;			/* Inserts on the list */

	/*
	 * Bulk load dynamically allocates an array of leaf-page references;
	 * when the bulk load finishes, we build an internal page for those
	 * references.
	 */
	WT_ROW_REF *rref;			/* List of row leaf pages */
	WT_COL_REF *cref;			/* List of column leaf pages */
	uint32_t ref_next;			/* Next leaf page slot */
	uint32_t ref_entries;			/* Total leaf page slots */
	uint32_t ref_allocated;			/* Bytes allocated */
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

	WT_STATS *stats_first;			/* First stats references */
	WT_STATS *stats;			/* Current stats reference */

	WT_BUF pvalue;				/* Current stats (pretty). */

	void (*clear_func)(WT_STATS *);		/* Function to clear stats. */
};

struct __wt_cursor_table {
	WT_CURSOR iface;

	WT_TABLE *table;
	const char *plan;
	WT_CURSOR **cg_cursors;
	WT_CURSOR **idx_cursors;
};
