/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cell_set --
 *	Set a WT_CELL's contents based on a type, prefix and data size.
 */
void
__wt_cell_set(WT_SESSION_IMPL *session,
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
		*cell_lenp = 1;		/* Cell byte */
		return;
	} else if (size < 0x3f) {
		/*
		 * Bit 0 is 0, bit 1 is the WT_CELL_KEY_SHORT flag; the other
		 * 6 bits are the size.
		 */
		byte = (uint8_t)size;
		cell->__chunk[0] = (byte << 2) | WT_CELL_KEY_SHORT;
		cell->__chunk[1] = (uint8_t)prefix;
		*cell_lenp = 2;		/* Cell byte + prefix byte */
		return;
	}

	p = cell->__chunk;
	*p++ = (uint8_t)type;			/* Type */

	if (type == WT_CELL_KEY)		/* Prefix byte */
		*p++ = (uint8_t)prefix;

	/* Pack the data length. */
	(void)__wt_vpack_uint(
	    session, &p, sizeof(cell->__chunk) - 1, (uint64_t)size);

						/* Return the cell's length */
	*cell_lenp = WT_PTRDIFF32(p, cell->__chunk);
}

/*
 * __wt_cell_copy --
 *	Copy an on-page cell into a return buffer, processing as needed.
 */
int
__wt_cell_copy(WT_SESSION_IMPL *session, WT_CELL *cell, WT_BUF *retb)
{
	WT_BTREE *btree;
	WT_OFF ovfl;
	uint32_t size;
	const void *p;
	void *huffman;

	btree = session->btree;

	/* Get the cell's data. */
	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_KEY:
		__wt_cell_data_and_len(session, cell, &p, &size);
		WT_RET(__wt_buf_set(session, retb, p, size));
		break;
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
		__wt_cell_off(session, cell, &ovfl);
		WT_RET(__wt_ovfl_in(session, &ovfl, retb));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	/* Select a Huffman encoding function. */
	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_DATA_OVFL:
		if ((huffman = btree->huffman_value) == NULL)
			return (0);
		break;
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
	default:
		if ((huffman = btree->huffman_key) == NULL)
			return (0);
		break;
	}

	return (__wt_huffman_decode(
	    session, huffman, retb->data, retb->size, retb));
}
