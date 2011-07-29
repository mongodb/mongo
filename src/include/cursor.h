/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

struct __wt_cursor_btree {
	WT_CURSOR iface;

	WT_BTREE *btree;			/* Enclosing btree */
	WT_WALK	  walk;				/* Current walk */
	WT_PAGE	 *page;				/* Current page */
	uint32_t  nitems;			/* Total page slots */

	WT_COL	  *cip;				/* Col-store page slot ref */
	WT_ROW	  *rip;				/* Row-store page slot ref */
	WT_INSERT *ins;				/* Insert chain */

	uint64_t recno;				/* Cursor record number */
	/*
	 * Column-store variable length items are optionally run-length encoded.
	 * WT_CURSOR_BTREE->rle is the count of items to return, decremented to
	 * 0.
	 */
	uint64_t rle;				/* RLE count */
	/*
	 * Column-store variable length items are optionally Huffman encoded.
	 * WT_CURSOR_BTREE->value is a buffer that contains a copy of the item
	 * we're returning, so we don't repeatedly decode them.  The buffer is
	 * NULL if the item was deleted.
	 */
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

	const char *stat_name;			/* Current stat name. */
	uint64_t stat_value;			/* Current stat value. */
	const char *stat_desc;			/* Current stat description. */

	void (*clear_func)(WT_STATS *);		/* Function to clear stats. */
};

struct __wt_cursor_table {
	WT_CURSOR iface;

	WT_TABLE *table;
	const char *plan;
	WT_CURSOR **cg_cursors;
	WT_CURSOR **idx_cursors;
};
