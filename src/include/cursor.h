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
	 * page, an insert head, insert list and a skiplist stack (the stack of
	 * skiplist entries leading to the insert point).  The search functions
	 * also return if an exact match was found and a write-generation for
	 * the leaf page.
	 */
	WT_PAGE	  *page;		/* Current page */
	uint32_t   slot;		/* WT_COL/WT_ROW 0-based slot */

	WT_INSERT_HEAD   *ins_head;	/* Insert chain head */
	WT_INSERT	 *ins;		/* Insert list, skiplist stack */
	WT_INSERT	**ins_stack[WT_SKIP_MAXDEPTH];

	uint64_t recno;			/* Record number */
	int	 match;			/* If search found an exact match */
	uint32_t write_gen;		/* Saved leaf page's write generation */

	/*
	 * The following fields are maintained by cursor iteration functions.
	 *
	 * We can't walk an insert list in reverse order, it's only linked in a
	 * forward, sorted order.   We don't care for column-store files, the
	 * record number gives us a "key" for lookup; for row-store files, we
	 * maintain a count of the current entry we're on.  For each iteration,
	 * we return one entry earlier in the list.
	 */
	uint32_t ins_entry_cnt;		/* 1-based insert list entry count */

	/*
	 * Variable-length column-store items are run-length encoded, and
	 * optionally Huffman encoded.   To avoid repeatedly decompressing the
	 * item, we decompress it once into the value buffer.  The vslot field
	 * is used to determine if we're returning information from the same
	 * slot as the last iteration on the cursor.
	 */
	WT_BUF	 value;			/* Variable-length return value */
	uint32_t vslot;			/* Variable-length value slot */

	/*
	 * Fixed-length column-store items are a single byte, and it's simpler
	 * and cheaper to allocate the space for it now.
	 */
	uint8_t v;			/* Fixed-length return value */

#define	WT_CBT_SEARCH_SET	0x01	/* Search has set a page */
	uint8_t flags;
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
	WT_ROW_REF *rref;			/* List of row leaf pages */
	WT_COL_REF *cref;			/* List of column leaf pages */
	uint32_t ref_next;			/* Next leaf page slot */
	uint32_t ref_entries;			/* Total leaf page slots */
	size_t   ref_allocated;			/* Bytes allocated */
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
