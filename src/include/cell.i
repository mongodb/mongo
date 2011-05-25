/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * WT_CELL --
 *	Variable-length cell type.
 *
 * Pages containing variable-length data (WT_PAGE_ROW_LEAF, WT_PAGE_ROW_INT
 * and WT_PAGE_COL_VAR page types), have variable-length cells after the page
 * header.
 *
 * There are 2 basic cell types: keys and data, each of which has an overflow
 * form.  The cell is usually followed by additional data, varying by type: a
 * key or data cell is followed by a set of bytes; a WT_OFF structure follows
 * an overflow form.
 *
 * There are 3 additional cell types: (1) a deleted type (a place-holder for
 * deleted cells where the cell cannot be removed, for example, a column-store
 * cell that must remain to preserve the record count); (2) a subtree reference
 * for keys that reference subtrees with no associated record count (a row-store
 * internal page has a key/reference pair for the tree containing all key/data
 * pairs greater than the key); (3) a subtree reference for keys that reference
 * subtrees with an associated record count (a column-store internal page has
 * a reference for the tree containing all records greater than the specified
 * record).
 *
 * Here's the cell type usage by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 *	Variable-length keys with offpage-reference pairs (a WT_CELL_KEY or
 *	WT_CELL_KEY_OVFL cell, followed by a WT_CELL_OFF cell).
 *
 * WT_PAGE_ROW_LEAF (row-store leaf pages):
 *	Variable-length key and data pairs (a WT_CELL_KEY or WT_CELL_KEY_OVFL
 *	cell, followed by a WT_CELL_DATA or WT_CELL_DATA_OVFL cell).
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length cells):
 *	Variable-length data cells (a WT_CELL_DATA or WT_CELL_DATA_OVFL cell,
 *	and for deleted cells, a WT_CELL_DEL.
 *
 * A cell consists of 1 descriptor byte, optionally followed by: 1 byte which
 * specifies key prefix compression, 1-4 bytes which specify a data length,
 * and bytes of data.
 *
 * A cell is never more than 6 bytes in length, and the WT_CELL structure is
 * just an array of 6 unsigned bytes.  (The WT_CELL structure's __chunk field
 * should not be directly read or written, use the macros and in-line functions
 * to manipulate it.)
 */

/*
 * Cell descriptor byte macros.
 *
 * In the most general case, bits 1-5 of the descriptor byte specify the cell
 * type, the 6th bit is unused, and bits 7-8 specify how many bytes of data
 * length follow the cell (if data-length bytes follow the cell).
 *
 * Bits 1 and 2 are reserved for "short" key and data items.  If bit 1 is set,
 * it's a short data item, less than 128 bytes in length, and the other 7 bits
 * are the length.   If bit 2 is set (but not bit 1), it's a short key, less
 * than 64 bytes in length, and the other 6 bits are the length.  (We test for
 * short data items first, otherwise this trick wouldn't work.)  The 0x03 bit
 * combination is unused, but would require code changes.
 */
#define	WT_CELL_DATA_SHORT	0x01		/* Short data */
#define	WT_CELL_KEY_SHORT	0x02		/* Short key */

/*
 * Bits 3-5 are for other cell types -- there are 7 cell types and 8 possible
 * values, the bit combination (7 << 2) remains unused.
 */
#define	WT_CELL_DATA		(0 << 2)	/* Data */
#define	WT_CELL_DATA_OVFL	(1 << 2)	/* Data: overflow */
#define	WT_CELL_DEL		(2 << 2)	/* Deleted */
#define	WT_CELL_KEY		(3 << 2)	/* Key */
#define	WT_CELL_KEY_OVFL	(4 << 2)	/* Key: overflow */
#define	WT_CELL_OFF		(5 << 2)	/* Off-page ref */
#define	WT_CELL_OFF_RECORD	(6 << 2)	/* Off-page ref with records */

/*
 * WT_CELL_{1,2,3,4}_BYTE --
 *	  Bits 7-8 of the descriptor byte specify how many bytes of data
 * length follow, if any.
 */
#define	WT_CELL_1_BYTE		(0 << 6)	/* 1 byte of length follows */
#define	WT_CELL_2_BYTE		(1 << 6)	/* 2 bytes of length follow */
#define	WT_CELL_3_BYTE		(2 << 6)	/* 3 ... */
#define	WT_CELL_4_BYTE		(3 << 6)	/* 4 ... */
#define	WT_CELL_BYTES(cell)						\
	(((WT_CELL *)(cell))->__chunk[0] & (3 << 6))

/* WT_CELL_FOREACH is a loop that walks the cells on a page */
#define	WT_CELL_FOREACH(dsk, cell, i)					\
	for ((cell) = WT_PAGE_DISK_BYTE(dsk),				\
	    (i) = (dsk)->u.entries;					\
	    (i) > 0; (cell) = __wt_cell_next(cell), --(i))

/*
 * __wt_cell_set_fixed --
 *	Set a WT_CELL's contents based on a fixed-size type.
 */
static inline void
__wt_cell_set_fixed(WT_CELL *cell, u_int type, uint32_t *cell_lenp)
{
	cell->__chunk[0] = (u_int8_t)type;
	*cell_lenp = 1;				/* Cell byte */
}

/*
 * __wt_cell_type_raw --
 *	Return the cell's type.
 */
static inline u_int
__wt_cell_type_raw(WT_CELL *cell)
{
	if (cell->__chunk[0] & WT_CELL_DATA_SHORT)
		return (WT_CELL_DATA_SHORT);
	if (cell->__chunk[0] & WT_CELL_KEY_SHORT)
		return (WT_CELL_KEY_SHORT);
	return (cell->__chunk[0] & (7 << 2));
}

/*
 * __wt_cell_type --
 *	Return a cell's type, mapping the short types to the normal, on-page
 * types.
 */
static inline u_int
__wt_cell_type(WT_CELL *cell)
{
	if (cell->__chunk[0] & WT_CELL_DATA_SHORT)
		return (WT_CELL_DATA);
	if (cell->__chunk[0] & WT_CELL_KEY_SHORT)
		return (WT_CELL_KEY);
	return (cell->__chunk[0] & (7 << 2));
}

/*
 * __wt_cell_prefix --
 *	Return a cell's prefix-compression value.
 */
static inline u_int
__wt_cell_prefix(WT_CELL *cell)
{
	return (cell->__chunk[1]);
}

/*
 * __wt_cell_data_and_len --
 *	Fill in both the first byte of data for a cell as well as the length.
 */
static inline void
__wt_cell_data_and_len(WT_CELL *cell, void *p, uint32_t *sizep)
{
	*(void **)p = __wt_cell_data(cell);
	*sizep = __wt_cell_datalen(cell);
}

/*
 * __wt_cell_off --
 *	Copy out a WT_CELL that references a WT_OFF structure.
 */
static inline void
__wt_cell_off(WT_CELL *cell, WT_OFF *off)
{
	/* Version for systems that support unaligned access. */
	*off = *(WT_OFF *)__wt_cell_data(cell);
}

/*
 * __wt_cell_off_record --
 *	Copy out a WT_CELL that references a WT_OFF_RECORD structure.
 */
static inline void
__wt_cell_off_record(WT_CELL *cell, WT_OFF_RECORD *off_record)
{
	/* Version for systems that support unaligned access. */
	*off_record = *(WT_OFF_RECORD *)__wt_cell_data(cell);
}

/*
 * __wt_cell_next --
 *	Return a pointer to the next WT_CELL on the page.
 */
static inline void *
__wt_cell_next(WT_CELL *cell)
{
	return ((u_int8_t *)cell + __wt_cell_len(cell));
}

/*
 * __wt_key_cell_next --
 *	Return a pointer to the next key WT_CELL on the page.
 */
static inline WT_CELL *
__wt_key_cell_next(WT_CELL *key_cell)
{
	/*
	 * Row-store leaf pages may have a single data cell between each key, or
	 * keys may be adjacent (when the data cell is empty).  Move to the next
	 * key.
	 */
	key_cell = __wt_cell_next(key_cell);
	if (__wt_cell_type(key_cell) != WT_CELL_KEY &&
	    __wt_cell_type(key_cell) != WT_CELL_KEY_OVFL)
		key_cell = __wt_cell_next(key_cell);
	return (key_cell);
}
