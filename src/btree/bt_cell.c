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
 * __wt_cell_data --
 *	Return a reference to the first byte of data for a cell.
 */
void *
__wt_cell_data(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	uint64_t v;
	const uint8_t *p;

	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	switch (__wt_cell_type_raw(cell)) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_DATA_SHORT:
	case WT_CELL_DEL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		return ((uint8_t *)cell + 1);	/* Cell byte */
	case WT_CELL_KEY_SHORT:
		return ((uint8_t *)cell + 2);	/* Cell + prefix byte */
	case WT_CELL_KEY:
		p = (uint8_t *)cell + 2;	/* Cell + prefix */
		(void)__wt_vunpack_uint(
		    session, &p, sizeof(cell->__chunk) - 2, &v);
		return ((void *)p);
	case WT_CELL_DATA:
	default:				/* Impossible */
		p = (uint8_t *)cell + 1;	/* Cell */
		(void)__wt_vunpack_uint(
		    session, &p, sizeof(cell->__chunk) - 1, &v);
		return ((void *)p);
	}
	/* NOTREACHED */
}

/*
 * __wt_cell_datalen --
 *	Return the number of data bytes referenced by a WT_CELL.
 */
uint32_t
__wt_cell_datalen(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	uint64_t v;
	const uint8_t *p;

	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	switch (__wt_cell_type_raw(cell)) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		return (sizeof(WT_OFF));
	case WT_CELL_DATA_SHORT:
		return (cell->__chunk[0] >> 1);
	case WT_CELL_DEL:
		return (0);
	case WT_CELL_KEY_SHORT:
		return (cell->__chunk[0] >> 2);
	case WT_CELL_KEY:
		p = (uint8_t *)cell + 2;	/* Step past cell + prefix */
		break;
	case WT_CELL_DATA:
	default:
		p = (uint8_t *)cell + 1;	/* Step past cell byte */
		break;
	}

	(void)__wt_vunpack_uint(session, &p, sizeof(cell->__chunk) - 1, &v);
	return ((uint32_t)v);
}

/*
 * __wt_cell_len --
 *	Return the total bytes taken up by a WT_CELL on page, including the
 * trailing data.
 */
uint32_t
__wt_cell_len(WT_SESSION_IMPL *session, WT_CELL *cell)
{
	uint64_t v;
	const uint8_t *p;

	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	switch (__wt_cell_type_raw(cell)) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_DATA_SHORT:
	case WT_CELL_DEL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:			/* Cell + data */
		return (1 + __wt_cell_datalen(session, cell));
	case WT_CELL_KEY_SHORT:			/* Cell + prefix + data */
		return (2 + __wt_cell_datalen(session, cell));
	case WT_CELL_KEY:
		p = &cell->__chunk[2];		/* Cell + prefix */
		break;
	case WT_CELL_DATA:
	default:				/* Impossible */
		p = &cell->__chunk[1];		/* Cell */
		break;
	}

	(void)__wt_vunpack_uint(session, &p, sizeof(cell->__chunk) - 1, &v);
	return ((uint32_t)(WT_PTRDIFF32(p, cell->__chunk) + v));
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
