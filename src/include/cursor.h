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
	 * WT_WALK is the stack of pages to the current cursor location, a
	 * page.
	 */
	WT_WALK	  walk;			/* Current walk */
	WT_PAGE	 *page;			/* Current page */

	/*
	 * The following fields give us a cursor location within a single page
	 * for traversal.  Our primary location is the WT_COL/WT_ROW slot, and
	 * we also track the number of slots to return remaining on this page.
	 */
	WT_COL	  *cip;			/* Col-store page slot */
	WT_ROW	  *rip;			/* Row-store page slot */
	uint32_t   nslots;		/* slots to return remaining */

	/*
	 * Insert lists override the slot: If the insert list is set for a row-
	 * store page, we're either walking an insert list of elements appearing
	 * before all the slots on the page, or between two slots on the page.
	 * If the insert list is set for a column-store page, it's replacement
	 * items for variable-length, RLE encoded entries, checked before we
	 * return an element.
	 *
	 * Further, we can't walk an insert list in reverse order.  Count the
	 * insert list entries, then repeatedly walk the list in the forward
	 * direction, each time returning one entry earlier in the list.
	 */
	WT_INSERT_HEAD  *ins_head;	/* Insert chain head */
	WT_INSERT	*ins;		/* Insert chain */
	uint32_t	 ins_prev_cnt;	/* Insert chain counting back */

	/*
	 * The following fields are cached information used for cursors as they
	 * return items from the page.
	 *
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
