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
	 * also return the relationship of the search key to the found key and
	 * a write-generation for the leaf page.
	 */
	WT_PAGE	  *page;		/* Current page */
	uint32_t   slot;		/* WT_COL/WT_ROW 0-based slot */

	WT_INSERT_HEAD   *ins_head;	/* Insert chain head */
	WT_INSERT	 *ins;		/* Insert list, skiplist stack */
	WT_INSERT	**ins_stack[WT_SKIP_MAXDEPTH];

	uint64_t recno;			/* Record number */
	uint32_t write_gen;		/* Saved leaf page's write generation */

	/*
	 * The search function sets compare to:
	 *	-1 if the found key is less than the specified key
	 *	 0 if the found key matches the specified key
	 *	+1 if the found key is larger than the specified key
	 */
	int	compare;

	/*
	 * We can't walk an insert list in reverse order, it's only linked in a
	 * forward, sorted order.  Maintain a count of the current entry we're
	 * on.  For each iteration, we return one entry earlier in the list.
	 */
	uint32_t ins_entry_cnt;		/* 1-based insert list entry count */

	/*
	 * It's relatively expensive to calculate the last record on a variable-
	 * length column-store page because of the repeat values.  Calculate it
	 * once per page and cache it.  This value doesn't include the skiplist
	 * of appended entries on the last page.
	 */
	uint64_t last_standard_recno;

	/*
	 * Variable-length column-store items are run-length encoded, and
	 * optionally Huffman encoded.   To avoid repeatedly decompressing the
	 * item, we decompress it once into the value buffer.  The vslot field
	 * is used to determine if we're returning information from the same
	 * slot as the last iteration on the cursor; we're never going to have
	 * UINT32_MAX slots on a page, so use that as our out-of-band value.
	 */
	WT_BUF	 value;			/* Variable-length return value */
#define	WT_CBT_VSLOT_OOB	UINT32_MAX
	uint32_t vslot;			/* Variable-length value slot */

	/*
	 * Fixed-length column-store items are a single byte, and it's simpler
	 * and cheaper to allocate the space for it now than keep checking to
	 * see if we need to grow the buffer.
	 */
	uint8_t v;			/* Fixed-length return value */

#define	WT_CBT_ITERATE_APPEND	0x01	/* Col-store: iterating append list */
#define	WT_CBT_ITERATE_NEXT	0x02	/* Next iteration configuration */
#define	WT_CBT_ITERATE_PREV	0x04	/* Prev iteration configuration */
#define	WT_CBT_SEARCH_SMALLEST	0x08	/* Row-store: small-key insert list */
	uint8_t flags;
};

struct __wt_cursor_bulk {
	WT_CURSOR_BTREE cbt;

	uint64_t recno;				/* Total record number */
	uint32_t ipp;				/* Items per page */
	uint32_t ins_cnt;			/* Inserts on the list */
	uint8_t	 page_type;			/* Page type */

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
	WT_CURSOR_BTREE cbt;

	WT_TABLE *table;
	const char *key_plan, *value_plan;
	WT_CURSOR **cg_cursors;
};

struct __wt_cursor_stat {
	WT_CURSOR iface;

	WT_STATS *stats_first;		/* First stats references */
	WT_STATS *stats;		/* Current stats reference */

	WT_BUF pvalue;			/* Current stats (pretty). */

	void (*clear_func)(WT_STATS *);	/* Function to clear stats. */
	WT_BTREE *btree;		/* Pinned btree handle. */
};

struct __wt_cursor_table {
	WT_CURSOR iface;

	WT_TABLE *table;
	const char *plan;
	WT_CURSOR **cg_cursors;
	WT_CURSOR **idx_cursors;
};

#define	WT_CURSOR_NEEDKEY(cursor)	do {				\
	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))			\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 1));		\
} while (0)

#define	WT_CURSOR_NEEDVALUE(cursor)	do {				\
	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))			\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 0));		\
} while (0)

