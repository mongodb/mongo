/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

struct __wt_cursor_backup {
	WT_CURSOR iface;

	size_t next;			/* Cursor position */

	size_t list_allocated;		/* List of files */
	size_t list_next;
	char **list;
};

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

	WT_INSERT_HEAD	*ins_head;	/* Insert chain head */
	WT_INSERT	*ins;		/* Current insert node */
					/* Search stack */
	WT_INSERT	**ins_stack[WT_SKIP_MAXDEPTH];

	uint64_t recno;			/* Record number */
	uint32_t write_gen;		/* Saved leaf page's write generation */

	/*
	 * The search function sets compare to:
	 *	< 1 if the found key is less than the specified key
	 *	  0 if the found key matches the specified key
	 *	> 1 if the found key is larger than the specified key
	 */
	int	compare;

	/*
	 * It's relatively expensive to calculate the last record on a variable-
	 * length column-store page because of the repeat values.  Calculate it
	 * once per page and cache it.  This value doesn't include the skiplist
	 * of appended entries on the last page.
	 */
	uint64_t last_standard_recno;

	/*
	 * Variable-length column-store values are run-length encoded and may
	 * be overflow values or Huffman encoded.   To avoid repeatedly reading
	 * overflow values or decompressing encoded values, process it once and
	 * store the result in a temporary buffer.  The cip_saved field is used
	 * to determine if we've switched columns since our last cursor call.
	 */
	WT_COL *cip_saved;		/* Last iteration reference */

	/*
	 * We don't instantiate prefix-compressed keys on pages where there's no
	 * Huffman encoding because we don't want to waste memory if only moving
	 * a cursor through the page, and it's faster to build keys while moving
	 * through the page than to roll-forward from a previously instantiated
	 * key (we don't instantiate all of the keys, just the ones at binary
	 * search points).  We can't use the application's WT_CURSOR key field
	 * as a copy of the last-returned key because it may have been altered
	 * by the API layer, for example, dump cursors.  Instead we store the
	 * last-returned key in a temporary buffer.  The rip_saved field is used
	 * to determine if the key in the temporary buffer has the prefix needed
	 * for building the current key.
	 */
	WT_ROW *rip_saved;		/* Last-returned key reference */

	/*
	 * A temporary buffer with two uses: caching RLE values for column-store
	 * files, and caching the last-returned keys for row-store files.
	 */
	WT_ITEM tmp;

	/*
	 * Fixed-length column-store items are a single byte, and it's simpler
	 * and cheaper to allocate the space for it now than keep checking to
	 * see if we need to grow the buffer.
	 */
	uint8_t v;			/* Fixed-length return value */

#define	WT_CBT_ITERATE_APPEND	0x01	/* Col-store: iterating append list */
#define	WT_CBT_ITERATE_NEXT	0x02	/* Next iteration configuration */
#define	WT_CBT_ITERATE_PREV	0x04	/* Prev iteration configuration */
#define	WT_CBT_MAX_RECORD	0x08	/* Col-store: past end-of-table */
#define	WT_CBT_SEARCH_SMALLEST	0x10	/* Row-store: small-key insert list */
	uint8_t flags;
};

struct __wt_cursor_bulk {
	WT_CURSOR_BTREE cbt;

	WT_PAGE *leaf;				/* The leaf page */

	/*
	 * Variable-length column store compares values during bulk load as
	 * part of RLE compression, row-store compares keys during bulk load
	 * to avoid corruption.
	 */
	WT_ITEM cmp;				/* Comparison buffer */

	/*
	 * Variable-length column-store RLE counter (also overloaded to mean
	 * the first time through the bulk-load insert routine, when set to 0).
	 */
	uint64_t rle;

	/*
	 * Fixed-length column-store current entry in memory chunk count, and
	 * the maximum number of records per chunk.
	 */
	uint32_t entry;				/* Entry count */
	uint32_t nrecs;				/* Max records per chunk */
};

struct __wt_cursor_config {
	WT_CURSOR iface;
};

struct __wt_cursor_dump {
	WT_CURSOR iface;

	WT_CURSOR *child;
};

struct __wt_cursor_index {
	WT_CURSOR_BTREE cbt;

	WT_TABLE *table;
	const char *key_plan, *value_plan;
	WT_CURSOR **cg_cursors;
};

struct __wt_cursor_stat {
	WT_CURSOR iface;

	WT_STATS *stats_first;		/* First stats reference */
	int	  stats_count;		/* Count of stats elements */

	int	 notpositioned;		/* Cursor not positioned */

	int	 key;			/* Current stats key */
	uint64_t v;			/* Current stats value */
	WT_ITEM   pv;			/* Current stats value (string) */

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

#define	WT_CURSOR_RECNO(cursor)	(strcmp((cursor)->key_format, "r") == 0)

#define	WT_CURSOR_NEEDKEY(cursor)	do {				\
	if (!F_ISSET(cursor, WT_CURSTD_KEY_SET))			\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 1));		\
} while (0)

#define	WT_CURSOR_NEEDVALUE(cursor)	do {				\
	if (!F_ISSET(cursor, WT_CURSTD_VALUE_SET))			\
		WT_ERR(__wt_cursor_kv_not_set(cursor, 0));		\
} while (0)
