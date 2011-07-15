/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#undef	STATIN
#define	STATIN static inline

STATIN void	__wt_cell_pack_fixed(WT_CELL *, u_int, uint32_t *);
STATIN void	__wt_cell_pack(WT_SESSION_IMPL *,
		    WT_CELL *, u_int, u_int, uint32_t, uint32_t *);
STATIN void	__wt_cell_unpack(
		    WT_SESSION_IMPL *, WT_CELL *, WT_CELL_UNPACK *);
STATIN int	__wt_cell_unpack_safe(
		    WT_SESSION_IMPL *, WT_CELL *, WT_CELL_UNPACK *, uint8_t *);

/*
 * WT_CELL --
 *	Variable-length cell type.
 *
 * Pages containing variable-length data (WT_PAGE_ROW_INT, WT_PAGE_ROW_LEAF,
 * and WT_PAGE_COL_VAR page types), have variable-length cells after the page
 * header.
 *
 * There are 4 basic cell types: keys and data (each of which has an overflow
 * form), deleted items and off-page references.  The cell is usually followed
 * by additional data, varying by type: a key or data cell is followed by a set
 * of bytes, a WT_OFF structure with addr/size pair follows overflow or off-page
 * cells.
 *
 * Deleted items are place-holders for column-store files, where entries cannot
 * be removed in order to preserve the record count.
 *
 * Here's cell use by page type:
 *
 * WT_PAGE_ROW_INT (row-store internal pages):
 *	Keys with offpage-reference items (a WT_CELL_KEY or WT_CELL_KEY_OVFL
 * cell followed by a WT_CELL_OFF cell).
 *
 * WT_PAGE_ROW_LEAF (row-store leaf pages):
 *	Keys with optional data items (a WT_CELL_KEY or WT_CELL_KEY_OVFL cell,
 *	optionally followed by a WT_CELL_DATA or WT_CELL_DATA_OVFL cell).
 *
 * Both WT_PAGE_ROW_INT and WT_PAGE_ROW_LEAF pages prefix compress keys, using
 * a single byte immediately following the cell.
 *
 * WT_PAGE_COL_VAR (Column-store leaf page storing variable-length cells):
 *	Data items (a WT_CELL_DATA or WT_CELL_DATA_OVFL cell), and deleted
 * items (a WT_CELL_DEL cell).
 */

/*
 * Cell descriptor byte macros.
 *
 * In the most general case, bits 1-5 of the descriptor byte specify the cell
 * type, the 6th bit is unused, and bits 7-8 specify how many bytes of data
 * length follow the cell (if data-length bytes follow the cell).
 *
 * Bits 1 and 2 are reserved for "short" key and data items.  If bit 1 (but not
 * bit 2) is set, it's a short data item, less than 128 bytes in length, and the
 * other 7 bits are the length.   If bit 2 is set (but not bit 1), it's a short
 * key, less than 64 bytes in length, and the other 6 bits are the length.
 *
 * The 0x03 bit combination is unused, but would require code changes.
 */
#define	WT_CELL_DATA_SHORT	0x001		/* Short data */
#define	WT_CELL_KEY_SHORT	0x002		/* Short key */
#define	WT_CELL_UNUSED_BIT3	0x004		/* Unused */
#define	WT_CELL_UNUSED_BIT4	0x008		/* Unused */
#define	WT_CELL_UNUSED_BIT5	0x010		/* Unused */

/*
 * Bits 6-8 are for other cell types -- there are 6 cell types and 8 possible
 * values.
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
	 * Maximum of 6 bytes:
	 *	  0: descriptor/type
	 *	  1: prefix compression
	 *	2-6: data-length
	 *           (variable-length encoding of a uint32_t can go to 5 bytes).
	 */
	uint8_t __chunk[7];
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

	WT_OFF	 off;			/* WT_OFF structure */

	const void *data;		/* Data */
	uint32_t    size;		/* Data size */

	uint32_t len;			/* Cell + data total length */
};

/*
 * WT_CELL_FOREACH --
 *	Walk the cells on a page.
 */
#define	WT_CELL_FOREACH(session, dsk, cell, unpack, i)			\
	for ((cell) = WT_PAGE_DISK_BYTE(dsk), (i) = (dsk)->u.entries;	\
	    (i) > 0;							\
	    (cell) = (WT_CELL *)((uint8_t *)cell + (unpack)->len), --(i))

/*
 * __wt_cell_pack_fixed --
 *	Write a WT_CELL's contents based on a fixed-size type.
 */
static inline void
__wt_cell_pack_fixed(WT_CELL *cell, u_int type, uint32_t *cell_lenp)
{
	cell->__chunk[0] = (u_int8_t)type;
	*cell_lenp = 1;				/* Cell byte */
}

/*
 * __wt_cell_pack --
 *	Set a WT_CELL's contents based on a type, prefix and data size.
 */
static inline void
__wt_cell_pack(WT_SESSION_IMPL *session,
    WT_CELL *cell, u_int type, u_int prefix, uint32_t size, uint32_t *cell_lenp)
{
	uint8_t byte, *p;

	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	WT_ASSERT(session, type == WT_CELL_DATA || type == WT_CELL_KEY);
	if (type == WT_CELL_DATA && size < 0x7f) {
		/*
		 * Bit 0 is the WT_CELL_DATA_SHORT flag; the other 7 bits are
		 * the size.
		 */
		byte = (uint8_t)size;
		cell->__chunk[0] = (byte << 1) | WT_CELL_DATA_SHORT;
		*cell_lenp = 1;			/* Cell byte */
		return;
	}
	if (size < 0x3f) {
		/*
		 * Bit 0 is 0, bit 1 is the WT_CELL_KEY_SHORT flag; the other
		 * 6 bits are the size.
		 */
		byte = (uint8_t)size;
		cell->__chunk[0] = (byte << 2) | WT_CELL_KEY_SHORT;
		cell->__chunk[1] = (uint8_t)prefix;
		*cell_lenp = 2;			/* Cell byte + prefix byte */
		return;
	}

	p = cell->__chunk;
	*p++ = (uint8_t)type;			/* Type */

	if (type == WT_CELL_KEY)		/* Prefix byte */
		*p++ = (uint8_t)prefix;

	/* Pack the data length: a cell is always big enough. */
	(void)__wt_vpack_uint(&p, 0, (uint64_t)size);

	*cell_lenp = WT_PTRDIFF32(p, cell);
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
__wt_cell_unpack(
    WT_SESSION_IMPL *session, WT_CELL *cell, WT_CELL_UNPACK *unpack)
{
	(void)__wt_cell_unpack_safe(session, cell, unpack, NULL);
}

/*
 * __wt_cell_unpack_safe --
 *	Unpack a WT_CELL into a structure during verification.
 */
static inline int
__wt_cell_unpack_safe(WT_SESSION_IMPL *session,
    WT_CELL *cell, WT_CELL_UNPACK *unpack, uint8_t *end)
{
	uint64_t v;
	const uint8_t *p;

	/*
	 * If our caller specifies an "1 past the end-of-buffer" reference, it's
	 * the verification code and we have to make sure we don't go past the
	 * end of the buffer when reading.  Don't complain on error here, our
	 * caller will take care of that.
	 *
	 * NOTE: WT_CELL_DATA_SHORT MUST BE CHECKED BEFORE WT_CELL_KEY_SHORT.
	 */
	if (cell->__chunk[0] & WT_CELL_DATA_SHORT) {
		unpack->type = WT_CELL_DATA;
		unpack->raw = WT_CELL_DATA_SHORT;
	} else if (cell->__chunk[0] & WT_CELL_KEY_SHORT) {
		unpack->type = WT_CELL_KEY;
		unpack->raw = WT_CELL_KEY_SHORT;
	} else
		unpack->type =
		    unpack->raw = cell->__chunk[0] & WT_CELL_TYPE_MASK;

	unpack->prefix = 0;
	unpack->ovfl = 0;
	p = (uint8_t *)cell + 1;			/* skip cell */

	/*
	 * Delete and off-page items have known sizes, there's no length bytes.
	 * Short key/data items have 6- or 7-bits of length in the descriptor
	 * byte and no length bytes.   Normal key/data items have length bytes.
	 */
	switch (unpack->raw) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
		unpack->ovfl = 1;
		/* FALLTHROUGH */
	case WT_CELL_OFF:
		if (end != NULL && p + sizeof(WT_OFF) > end)
			return (WT_ERROR);
		memcpy(&unpack->off, p, sizeof(WT_OFF));

		unpack->data = NULL;
		unpack->size = sizeof(WT_OFF);
		unpack->len = 1 + sizeof(WT_OFF);
		break;
	case WT_CELL_DEL:
		unpack->data = NULL;
		unpack->size = 0;
		unpack->len = 1;
		break;
	case WT_CELL_DATA_SHORT:
		unpack->data = p;
		unpack->size = cell->__chunk[0] >> 1;
		unpack->len = 1 + unpack->size;
		break;
	case WT_CELL_DATA:
		WT_RET_TEST(__wt_vunpack_uint(&p,
		    (end == NULL) ? 0 : (size_t)(end - p), &v), WT_ERROR);
		if (end != NULL && p + v > end)
			return (WT_ERROR);

		unpack->data = p;
		unpack->size = (uint32_t)v;
		unpack->len = WT_PTRDIFF32(p, cell) + (uint32_t)v;
		break;
	case WT_CELL_KEY_SHORT:
		unpack->prefix = cell->__chunk[1];
		++p;					/* skip prefix */

		unpack->data = p;
		unpack->size = cell->__chunk[0] >> 2;
		unpack->len = 2 + unpack->size;
		break;
	case WT_CELL_KEY:
		unpack->prefix = cell->__chunk[1];
		++p;					/* skip prefix */
		WT_RET_TEST(__wt_vunpack_uint(&p,
		    (end == NULL) ? 0 : (size_t)(end - p), &v), WT_ERROR);
		if (end != NULL && p + v > end)
			return (WT_ERROR);

		unpack->data = p;
		unpack->size = (uint32_t)v;
		unpack->len = WT_PTRDIFF32(p, cell) + (uint32_t)v;
		break;
	default:
		return (end == NULL ? __wt_file_format(session) : WT_ERROR);
	}
	return (0);
}
