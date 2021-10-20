/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * WT_CELL --
 *	Variable-length cell type.
 *
 * Pages containing variable-length keys or values data (the WT_PAGE_ROW_INT,
 * WT_PAGE_ROW_LEAF, WT_PAGE_COL_INT and WT_PAGE_COL_VAR page types), have
 * cells after the page header.
 *
 * There are 4 basic cell types: keys and data (each of which has an overflow
 * form), deleted cells and off-page references.  The cell is usually followed
 * by additional data, varying by type: a key or data cell is followed by a set
 * of bytes, an address cookie follows overflow or off-page cells.
 *
 * Deleted cells are place-holders for column-store files, where entries cannot
 * be removed in order to preserve the record count.
 *
 * Here's the cell use by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal page):
 *	Keys and offpage-reference pairs (a WT_CELL_KEY or WT_CELL_KEY_OVFL
 * cell followed by a WT_CELL_ADDR_XXX cell).
 *
 * WT_PAGE_ROW_LEAF (row-store leaf page):
 *	Keys with optional data cells (a WT_CELL_KEY or WT_CELL_KEY_OVFL cell,
 *	normally followed by a WT_CELL_{VALUE,VALUE_COPY,VALUE_OVFL} cell).
 *
 *	WT_PAGE_ROW_LEAF pages optionally prefix-compress keys, using a single
 *	byte count immediately following the cell.
 *
 * WT_PAGE_COL_INT (Column-store internal page):
 *	Off-page references (a WT_CELL_ADDR_XXX cell).
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length cells):
 *	Data cells (a WT_CELL_{VALUE,VALUE_COPY,VALUE_OVFL} cell), or deleted
 * cells (a WT_CELL_DEL cell).
 *
 * Each cell starts with a descriptor byte:
 *
 * Bits 1 and 2 are reserved for "short" key and value cells (that is, a cell
 * carrying data less than 64B, where we can store the data length in the cell
 * descriptor byte):
 *	0x00	Not a short key/data cell
 *	0x01	Short key cell
 *	0x10	Short key cell, with a following prefix-compression byte
 *	0x11	Short value cell
 * In these cases, the other 6 bits of the descriptor byte are the data length.
 *
 * Bit 3 marks an 8B packed, uint64_t value following the cell description byte.
 * (A run-length counter or a record number for variable-length column store.)
 *
 * Bit 4 is unused.
 *
 * Bits 5-8 are cell "types".
 */
#define WT_CELL_KEY_SHORT 0x01     /* Short key */
#define WT_CELL_KEY_SHORT_PFX 0x02 /* Short key with prefix byte */
#define WT_CELL_VALUE_SHORT 0x03   /* Short data */
#define WT_CELL_SHORT_TYPE(v) ((v)&0x03U)

#define WT_CELL_SHORT_MAX 63  /* Maximum short key/value */
#define WT_CELL_SHORT_SHIFT 2 /* Shift for short key/value */

#define WT_CELL_64V 0x04 /* Associated value */

/*
 * We could use bit 4 as a single bit (similar to bit 3), or as a type bit in a backward compatible
 * way by adding bit 4 to the type mask and adding new types that incorporate it.
 */
#define WT_CELL_UNUSED_BIT4 0x08 /* Unused */

/*
 * WT_CELL_ADDR_INT is an internal block location, WT_CELL_ADDR_LEAF is a leaf
 * block location, and WT_CELL_ADDR_LEAF_NO is a leaf block location where the
 * page has no overflow items.  (The goal is to speed up truncation as we don't
 * have to read pages without overflow items in order to delete them.  Note,
 * WT_CELL_ADDR_LEAF_NO is not guaranteed to be set on every page without
 * overflow items, the only guarantee is that if set, the page has no overflow
 * items.)
 *
 * WT_CELL_VALUE_COPY is a reference to a previous cell on the page, supporting
 * value dictionaries: if the two values are the same, we only store them once
 * and have the second and subsequent use reference the original.
 */
#define WT_CELL_ADDR_DEL (0)            /* Address: deleted */
#define WT_CELL_ADDR_INT (1 << 4)       /* Address: internal  */
#define WT_CELL_ADDR_LEAF (2 << 4)      /* Address: leaf */
#define WT_CELL_ADDR_LEAF_NO (3 << 4)   /* Address: leaf no overflow */
#define WT_CELL_DEL (4 << 4)            /* Deleted value */
#define WT_CELL_KEY (5 << 4)            /* Key */
#define WT_CELL_KEY_OVFL (6 << 4)       /* Overflow key */
#define WT_CELL_KEY_OVFL_RM (12 << 4)   /* Overflow key (removed) */
#define WT_CELL_KEY_PFX (7 << 4)        /* Key with prefix byte */
#define WT_CELL_VALUE (8 << 4)          /* Value */
#define WT_CELL_VALUE_COPY (9 << 4)     /* Value copy */
#define WT_CELL_VALUE_OVFL (10 << 4)    /* Overflow value */
#define WT_CELL_VALUE_OVFL_RM (11 << 4) /* Overflow value (removed) */

#define WT_CELL_TYPE_MASK (0x0fU << 4) /* Maximum 16 cell types */
#define WT_CELL_TYPE(v) ((v)&WT_CELL_TYPE_MASK)

/*
 * When we aren't able to create a short key or value (and, in the case of a value, there's no
 * associated RLE), the key or value is at least 64B, else we'd have been able to store it as a
 * short cell. Decrement/Increment the size before storing it, in the hopes that relatively small
 * key/value sizes will pack into a single byte instead of two bytes.
 */
#define WT_CELL_SIZE_ADJUST 64

/*
 * WT_CELL --
 *	Variable-length, on-page cell header.
 */
struct __wt_cell {
    /*
     * Maximum of 16 bytes:
     * 1: cell descriptor byte
     * 1: prefix compression count
     * 9: associated 64-bit value	(uint64_t encoding, max 9 bytes)
     * 5: data length		(uint32_t encoding, max 5 bytes)
     *
     * This calculation is pessimistic: the prefix compression count and
     * 64V value overlap, the 64V value and data length are optional.
     */
    uint8_t __chunk[1 + 1 + WT_INTPACK64_MAXSIZE + WT_INTPACK32_MAXSIZE];
};

/*
 * WT_CELL_UNPACK --
 *	Unpacked cell.
 */
struct __wt_cell_unpack {
    WT_CELL *cell; /* Cell's disk image address */

    uint64_t v; /* RLE count or recno */

    /*
     * !!!
     * The size and __len fields are reasonably type size_t; don't change
     * the type, performance drops significantly if they're type size_t.
     */
    const void *data; /* Data */
    uint32_t size;    /* Data size */

    uint32_t __len; /* Cell + data length (usually) */

    uint8_t prefix; /* Cell prefix length */

    uint8_t raw;  /* Raw cell type (include "shorts") */
    uint8_t type; /* Cell type */

    uint8_t ovfl; /* boolean: cell is an overflow */
};
