/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#undef	STATIN
#define	STATIN static inline

STATIN uint32_t	__wt_cell_pack_data(WT_CELL *, uint32_t, uint32_t);
STATIN uint32_t	__wt_cell_pack_key(WT_CELL *, uint8_t, uint32_t);
STATIN uint32_t	__wt_cell_pack_type(WT_CELL *, uint8_t, uint32_t);
STATIN void	__wt_cell_unpack(WT_CELL *, WT_CELL_UNPACK *);
STATIN int	__wt_cell_unpack_safe(WT_CELL *, WT_CELL_UNPACK *, uint8_t *);

/*
 * WT_CELL --
 *	Variable-length cell type.
 *
 * Pages containing variable-length data (WT_PAGE_ROW_INT, WT_PAGE_ROW_LEAF,
 * and WT_PAGE_COL_VAR page types), have cells after the page header.
 *
 * There are 4 basic cell types: keys and data (each of which has an overflow
 * form), deleted cells and off-page references.  The cell is usually followed
 * by additional data, varying by type: a key or data cell is followed by a set
 * of bytes, a WT_OFF structure with addr/size pair follows overflow or off-page
 * cells.
 *
 * Deleted cells are place-holders for column-store files, where entries cannot
 * be removed in order to preserve the record count.
 *
 * Here's cell use by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 *	Keys with offpage-reference pairs (a WT_CELL_KEY or WT_CELL_KEY_OVFL
 * cell followed by a WT_CELL_OFF cell).
 *
 * WT_PAGE_ROW_LEAF (row-store leaf pages):
 *	Keys with optional data cells (a WT_CELL_KEY or WT_CELL_KEY_OVFL cell,
 *	optionally followed by a WT_CELL_DATA or WT_CELL_DATA_OVFL cell).
 *
 * Both WT_PAGE_ROW_INT and WT_PAGE_ROW_LEAF pages prefix compress keys, using
 * a single byte immediately following the cell.
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length cells):
 *	Data cells (a WT_CELL_DATA or WT_CELL_DATA_OVFL cell), and deleted
 * cells (a WT_CELL_DEL cell).
 *
 * Cell descriptor byte:
 *
 * Bits 1 and 2 are reserved for "short" key and data cells.  If bit 1 (but not
 * bit 2) is set, it's a short data item, less than 128 bytes in length, and the
 * other 7 bits are the length.   If bit 2 is set (but not bit 1), it's a short
 * key, less than 64 bytes in length, and the other 6 bits are the length.  The
 * 0x03 bit combination (setting both 0x01 and 0x02) is unused, but will require
 * code changes.
 *
 * Bit 3 marks run-length encoded variable-length column store data: immediately
 * after the cell description byte, there's a uint32_t repeat count.
 */
#define	WT_CELL_DATA_SHORT	0x001		/* Short data */
#define	WT_CELL_KEY_SHORT	0x002		/* Short key */
#define	WT_CELL_RLE		0x004		/* Run-length encoding */
#define	WT_CELL_UNUSED_BIT4	0x008		/* Unused */
#define	WT_CELL_UNUSED_BIT5	0x010		/* Unused */

/*
 * Bits 6-8 are for other cell types (there are currently 6 cell types).
 */
#define	WT_CELL_DATA		(0 << 5)	/* Data */
#define	WT_CELL_DATA_OVFL	(1 << 5)	/* Data: overflow */
#define	WT_CELL_DEL		(2 << 5)	/* Deleted */
#define	WT_CELL_KEY		(3 << 5)	/* Key */
#define	WT_CELL_KEY_OVFL	(4 << 5)	/* Key: overflow */
#define	WT_CELL_OFF		(5 << 5)	/* Off-page ref */
#define	WT_CELL_UNUSED_TYPE6	(6 << 5)	/* Unused */
#define	WT_CELL_UNUSED_TYPE7	(7 << 5)	/* Unused */
#define	WT_CELL_TYPE_MASK	(7 << 5)

/*
 * WT_CELL --
 *	Variable-length, on-page cell header.
 */
struct __wt_cell {
	/*
	 * Maximum of 12 bytes:
	 *	   0: type, RLE flag
	 *	   1: prefix compression count
	 *	 2-6: optional RLE count	(uint32_t encoding, max 5 bytes)
	 *	7-11: optional data length	(uint32_t encoding, max 5 bytes)
	 */
	uint8_t __chunk[12];
};

/*
 * WT_CELL_UNPACK --
 *	Unpacked cell.
 */
struct __wt_cell_unpack {
	uint8_t  raw;			/* Raw cell type (include "shorts") */
	uint8_t  type;			/* Cell type */
	uint8_t  prefix;		/* Cell prefix */
	uint8_t	 ovfl;			/* Cell is an overflow */

	uint32_t rle;			/* RLE count */

	WT_OFF	 off;			/* WT_OFF structure */

	const void *data;		/* Data */
	uint32_t    size;		/* Data size */

	uint32_t    len;		/* Cell + data total length */
};

/*
 * WT_CELL_FOREACH --
 *	Walk the cells on a page.
 */
#define	WT_CELL_FOREACH(dsk, cell, unpack, i)				\
	for ((cell) = WT_PAGE_DISK_BYTE(dsk), (i) = (dsk)->u.entries;	\
	    (i) > 0;							\
	    (cell) = (WT_CELL *)((uint8_t *)cell + (unpack)->len), --(i))

/*
 * __wt_cell_pack_key --
 *	Set a key's WT_CELL contents.
 */
static inline uint32_t
__wt_cell_pack_key(WT_CELL *cell, uint8_t prefix, uint32_t size)
{
	uint8_t byte, *p;

	/*
	 * Short keys have 6-bits of length in the descriptor byte and no length
	 * bytes.
	 *
	 * Bit 0 is 0, bit 1 is the WT_CELL_KEY_SHORT flag; the other 6 bits are
	 * the size.
	 */
	if (size <= 0x3f) {
		byte = (uint8_t)size;
		cell->__chunk[0] = (byte << 2) | WT_CELL_KEY_SHORT;
		cell->__chunk[1] = prefix;
		return (2);
	}

	cell->__chunk[0] = WT_CELL_KEY;		/* Type */
	cell->__chunk[1] = prefix;		/* Prefix */

	p = cell->__chunk + 2;			/* Length */
	(void)__wt_vpack_uint(&p, 0, (uint64_t)size);

	return (WT_PTRDIFF32(p, cell));
}

/*
 * __wt_cell_pack_data --
 *	Set a data item's WT_CELL contents.
 */
static inline uint32_t
__wt_cell_pack_data(WT_CELL *cell, uint32_t rle, uint32_t size)
{
	uint8_t byte, *p;

	/*
	 * Short data cells have 7-bits of length in the descriptor byte and no
	 * length bytes.
	 *
	 * Bit 0 is the WT_CELL_DATA_SHORT flag; the other 7 bits are the size.
	 */
	if (rle == 0 && size <= 0x7f) {
		byte = (uint8_t)size;
		cell->__chunk[0] = (byte << 1) | WT_CELL_DATA_SHORT;
		return (1);
	}

	p = cell->__chunk + 1;
	if (rle == 0)				/* Type + RLE */
		cell->__chunk[0] = WT_CELL_DATA;
	else {
		cell->__chunk[0] = WT_CELL_DATA | WT_CELL_RLE;
		(void)__wt_vpack_uint(&p, 0, (uint64_t)rle);
	}
						/* Length */
	(void)__wt_vpack_uint(&p, 0, (uint64_t)size);

	return (WT_PTRDIFF32(p, cell));
}

/*
 * __wt_cell_pack_type --
 *	Write a WT_CELL's contents based on a type with optional RLE count but
 * fixed-size data (that is, no need for data length bytes).
 */
static inline uint32_t
__wt_cell_pack_type(WT_CELL *cell, uint8_t type, uint32_t rle)
{
	uint8_t *p;

	if (rle == 0) {				/* Type + RLE */
		cell->__chunk[0] = type;
		return (1);
	}

	cell->__chunk[0] = type | WT_CELL_RLE;

	p = cell->__chunk + 1;
	(void)__wt_vpack_uint(&p, 0, (uint64_t)rle);

	return (WT_PTRDIFF32(p, cell));
}

/*
 * __wt_cell_type --
 *	Return the cell's type (collapsing "short" types).
 */
static inline u_int
__wt_cell_type(WT_CELL *cell)
{
	/*
	 * NOTE: WT_CELL_DATA_SHORT MUST BE CHECKED BEFORE WT_CELL_KEY_SHORT.
	 */
	if (cell->__chunk[0] & WT_CELL_DATA_SHORT)
		return (WT_CELL_DATA);
	if (cell->__chunk[0] & WT_CELL_KEY_SHORT)
		return (WT_CELL_KEY);
	return (cell->__chunk[0] & WT_CELL_TYPE_MASK);
}

/*
 * __wt_cell_unpack --
 *	Unpack a WT_CELL into a structure.
 */
static inline void
__wt_cell_unpack(WT_CELL *cell, WT_CELL_UNPACK *unpack)
{
	(void)__wt_cell_unpack_safe(cell, unpack, NULL);
}

/*
 * __wt_cell_unpack_safe --
 *	Unpack a WT_CELL into a structure during verification.
 */
static inline int
__wt_cell_unpack_safe(WT_CELL *cell, WT_CELL_UNPACK *unpack, uint8_t *end)
{
	uint64_t v;
	const uint8_t *p;

	memset(unpack, 0, sizeof(*unpack));

	/*
	 * The verification code specifies an end argument, a pointer to 1 past
	 * the end-of-page.  In that case, make sure we don't go past the end
	 * of the page when reading.  If an error occurs, we simply return the
	 * error code, the verification code takes care of complaining (and, in
	 * the case of salvage, it won't complain at all, it's OK to fail).
	 */
#undef	CHK
#define	CHK(p, len) do {						\
	if (end != NULL && (((uint8_t *)p) + (len)) > end)		\
		return (WT_ERROR);					\
} while (0)

	/*
	 * Check the cell description byte, then get the cell type.
	 *
	 * NOTE: WT_CELL_DATA_SHORT MUST BE CHECKED BEFORE WT_CELL_KEY_SHORT.
	 */
	CHK(cell, 0);
	if (cell->__chunk[0] & WT_CELL_DATA_SHORT) {
		unpack->type = WT_CELL_DATA;
		unpack->raw = WT_CELL_DATA_SHORT;
	} else if (cell->__chunk[0] & WT_CELL_KEY_SHORT) {
		unpack->type = WT_CELL_KEY;
		unpack->raw = WT_CELL_KEY_SHORT;
	} else
		unpack->type =
		    unpack->raw = cell->__chunk[0] & WT_CELL_TYPE_MASK;

	/*
	 * Handle cells with neither an RLE count or data length: short key/data
	 * cells have 6- or 7-bits of data length in the descriptor byte and no
	 * RLE count or length bytes.   Off-page cells have known sizes, and no
	 * RLE count or length bytes.
	 */
	switch (unpack->raw) {
	case WT_CELL_DATA_SHORT:
		/*
		 * Not reading any more memory, no further checks until the
		 * final check of the complete cell and its associated data.
		 */
		unpack->data = cell->__chunk + 1;
		unpack->size = cell->__chunk[0] >> 1;
		unpack->len = 1 + unpack->size;
		goto done;
	case WT_CELL_KEY_SHORT:
		/*
		 * Check the prefix byte that follows the cell descriptor byte.
		 */
		CHK(cell, 1);
		unpack->prefix = cell->__chunk[1];

		unpack->data = cell->__chunk + 2;
		unpack->size = cell->__chunk[0] >> 2;
		unpack->len = 2 + unpack->size;
		goto done;
	case WT_CELL_OFF:
		/*
		 * Check the WT_OFF that follows the cell descriptor byte.
		 */
		CHK(cell, 1 + sizeof(WT_OFF));		/* check WT_OFF */
		memcpy(&unpack->off, cell->__chunk + 1, sizeof(WT_OFF));
		unpack->len = 1 + sizeof(WT_OFF);
		goto done;
	}

	/*
	 * The rest of the cell types optionally have an RLE count and/or a
	 * data length.
	 */
	p = (uint8_t *)cell + 1;			/* skip cell */

	if (unpack->raw == WT_CELL_KEY) {
		/*
		 * Check the prefix byte that follows the cell descriptor byte.
		 */
		++p;					/* skip prefix */
		CHK(p, 0);
		unpack->prefix = cell->__chunk[1];
	}

	if (cell->__chunk[0] & WT_CELL_RLE) {		/* skip RLE */
		WT_RET(__wt_vunpack_uint(
		    &p, end == NULL ? 0 : (size_t)(end - p), &v));
		unpack->rle = v;
	}

	/*
	 * Overflow and deleted cells have known sizes, and no length bytes.
	 * Key/data cells have data length bytes.
	 */
	switch (unpack->raw) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
		CHK(p, sizeof(WT_OFF));			/* check WT_OFF */
		unpack->ovfl = 1;
		memcpy(&unpack->off, p, sizeof(WT_OFF));
		unpack->len = WT_PTRDIFF32(p, cell) + sizeof(WT_OFF);
		break;
	case WT_CELL_DEL:
		unpack->len = WT_PTRDIFF32(p, cell);
		break;
	case WT_CELL_DATA:
	case WT_CELL_KEY:
		WT_RET(__wt_vunpack_uint(
		    &p, end == NULL ? 0 : (size_t)(end - p), &v));
		unpack->data = p;
		unpack->size = (uint32_t)v;
		unpack->len = WT_PTRDIFF32(p, cell) + (uint32_t)v;
		break;
	default:
		return (WT_ERROR);			/* Unknown cell type. */
	}

	/*
	 * Check the original cell against the full cell length (this is a
	 * diagnostic as well, we may be copying the cell from the page and
	 * we need the right length.
	 */
done:	CHK(cell, unpack->len);
	return (0);
}
