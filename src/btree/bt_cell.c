/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "cell.i"

/*
 * __wt_cell_set --
 *	Set a WT_CELL's contents based on a type, prefix and data size.
 */
void
__wt_cell_set(WT_CELL *cell,
    u_int type, u_int prefix, uint32_t size, uint32_t *cell_lenp)
{
	uint32_t len;
	uint8_t byte, *p;

	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	switch (type) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_DEL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		cell->__chunk[0] = (uint8_t)type;
		*cell_lenp = 1;			/* Cell byte */
		return;
	case WT_CELL_DATA:
		if (size < 0x7f) {
			/*
			 * Bit 0 is the WT_CELL_DATA_SHORT flag; the other 7
			 * bits are the size.
			 */
			byte = (uint8_t)size;
			cell->__chunk[0] = (byte << 1) | WT_CELL_DATA_SHORT;
			*cell_lenp = 1;		/* Cell byte */
			return;
		}
		break;
	case WT_CELL_KEY:
	default:
		if (size < 0x3f) {
			/*
			 * Bit 0 is 0, bit 1 is the WT_CELL_KEY_SHORT flag;
			 * the other 6 bits are the size.
			 */
			byte = (uint8_t)size;
			cell->__chunk[0] = (byte << 2) | WT_CELL_KEY_SHORT;
			cell->__chunk[1] = (uint8_t)prefix;
			*cell_lenp = 2;		/* Cell byte + prefix byte */
			return;
		}
		break;
	}

	/*
	 * Key and data items that fit on the page, but are too big for the
	 * length to fit into the descriptor byte.
	 */
	if (size < 0xff) {
		cell->__chunk[0] = WT_CELL_1_BYTE | (uint8_t)type;
		len = 2;			/* Cell byte + 1 length byte */
	} else if (size < 0xffff) {
		cell->__chunk[0] = WT_CELL_2_BYTE | (uint8_t)type;
		len = 3;			/* Cell byte + 2 length bytes */
	} else if (size < 0xffffff) {
		cell->__chunk[0] = WT_CELL_3_BYTE | (uint8_t)type;
		len = 4;			/* Cell byte + 3 length bytes */
	} else {
		cell->__chunk[0] = WT_CELL_4_BYTE | (uint8_t)type;
		len = 5;			/* Cell byte + 4 length bytes */
	}

	p = &cell->__chunk[0];			/* Cell byte */
	if (type == WT_CELL_KEY) {
		*++p = (uint8_t)prefix;		/* Prefix byte */
		++len;
	}

	*cell_lenp = len;			/* Return the cell's length */

#ifdef WORDS_BIGENDIAN
	__wt_cell_set has not been written for a big-endian system.
#else
	/* Little endian copy of the size into the WT_CELL. */
	*++p = ((uint8_t *)&size)[0];
	*++p = ((uint8_t *)&size)[1];
	*++p = ((uint8_t *)&size)[2];
	*++p = ((uint8_t *)&size)[3];
#endif
}

/*
 * __wt_cell_data --
 *	Return a reference to the first byte of data for a cell.
 */
void *
__wt_cell_data(WT_CELL *cell)
{
	uint8_t *p;

	p = (uint8_t *)cell + 1;		/* Step past cell byte */

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
		++p;				/* Step past prefix byte */
		/* FALLTHROUGH */
	case WT_CELL_DATA:
	default:				/* N bytes more */
		switch (WT_CELL_BYTES(cell)) {
		case WT_CELL_1_BYTE:
			return (p + 1);		/* Step past 1 length byte */
		case WT_CELL_2_BYTE:
			return (p + 2);		/* Step past 2 length bytes */
		case WT_CELL_3_BYTE:
			return (p + 3);		/* Step past 3 length bytes */
		case WT_CELL_4_BYTE:
		default:
			return (p + 4);		/* Step past 4 length bytes */
		}
		/* NOTREACHED */
	}
	/* NOTREACHED */
}

/*
 * __wt_cell_datalen --
 *	Return the number of data bytes referenced by a WT_CELL.
 */
uint32_t
__wt_cell_datalen(WT_CELL *cell)
{
	uint32_t _v;
	uint8_t *p, *v;

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

	/* Copy out the length bytes, and return the value. */
	_v = 0;
	v = (uint8_t *)&_v;

#ifdef WORDS_BIGENDIAN
	__wt_cell_datalen has not been written for a big-endian system.
#else
	/* Little endian copy of the size from the WT_CELL. */
	switch (WT_CELL_BYTES(cell)) {
	case WT_CELL_4_BYTE:
		*v++ = *p++;
		/* FALLTHROUGH */
	case WT_CELL_3_BYTE:
		*v++ = *p++;
		/* FALLTHROUGH */
	case WT_CELL_2_BYTE:
		*v++ = *p++;
		/* FALLTHROUGH */
	case WT_CELL_1_BYTE:
		*v = *p;
		break;
	}
#endif
	return (_v);
}

/*
 * __wt_cell_len --
 *	Return the total bytes taken up by a WT_CELL on page, including the
 * trailing data.
 */
uint32_t
__wt_cell_len(WT_CELL *cell)
{
	uint32_t len;

	len = 1;				/* Step past cell */
	/*
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of length in the
	 * descriptor byte and no length bytes.
	 */
	switch (__wt_cell_type_raw(cell)) {
	case WT_CELL_DATA_SHORT:		/* Cell + data */
		return (1 + __wt_cell_datalen(cell));
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_OFF:
		return (1 + sizeof(WT_OFF));	/* Cell + WT_OFF */
	case WT_CELL_DEL:
		return (1);			/* Cell */
	case WT_CELL_KEY_SHORT:			/* Cell + prefix + data */
		return (2 + __wt_cell_datalen(cell));
	case WT_CELL_KEY:
		++len;				/* Step past prefix byte */
		/* FALLTHROUGH */
	case WT_CELL_DATA:
	default:
		/* Calculate the length bytes and the data length. */
		switch (WT_CELL_BYTES(cell)) {
		case WT_CELL_1_BYTE:
			return (len + 1 + __wt_cell_datalen(cell));
		case WT_CELL_2_BYTE:
			return (len + 2 + __wt_cell_datalen(cell));
		case WT_CELL_3_BYTE:
			return (len + 3 + __wt_cell_datalen(cell));
		case WT_CELL_4_BYTE:
		default:
			return (len + 4 + __wt_cell_datalen(cell));
		}
		/* NOTREACHED */
	}
	/* NOTREACHED */
}

/*
 * __wt_cell_copy --
 *	Copy an on-page cell into a return buffer, processing as needed.
 */
int
__wt_cell_copy(SESSION *session, WT_CELL *cell, WT_BUF *retb)
{
	BTREE *btree;
	WT_OFF ovfl;
	uint32_t size;
	const void *p;
	void *huffman;

	btree = session->btree;

	/* Get the cell's data. */
	switch (__wt_cell_type(cell)) {
	case WT_CELL_DATA:
	case WT_CELL_KEY:
		__wt_cell_data_and_len(cell, &p, &size);
		WT_RET(__wt_buf_set(session, retb, p, size));
		break;
	case WT_CELL_DATA_OVFL:
	case WT_CELL_KEY_OVFL:
		__wt_cell_off(cell, &ovfl);
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
