/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

/*
 * __wt_cell_space_req --
 *	Return the WT_CELL bytes required for a length of data.
 */
static inline uint32_t
__wt_cell_space_req(uint32_t size)
{
	/*
	 * A WT_CELL requires 1B for the type/byte-count value, plus N
	 * bytes of byte-count.
	 */
	return
	    (size < 0xff ? 2 :
	    (size < 0xffff ? 3 :
	    (size < 0xffffff ? 4 : 5)));
}

/*
 * __wt_cell_set --
 *	Set a WT_CELL's contents based on a data size and type.
 */
static inline void
__wt_cell_set(WT_CELL *cell, u_int type, uint32_t size)
{
	uint8_t *v;

	if (size < 0xff)
		cell->__cell_chunk[0] = WT_CELL_1_BYTE | (u_int8_t)type;
	else if (size < 0xffff)
		cell->__cell_chunk[0] = WT_CELL_2_BYTE | (u_int8_t)type;
	else if (size < 0xffffff)
		cell->__cell_chunk[0] = WT_CELL_3_BYTE | (u_int8_t)type;
	else
		cell->__cell_chunk[0] = WT_CELL_4_BYTE | (u_int8_t)type;

#ifdef WORDS_BIGENDIAN
	__wt_cell_set has not been written for a big-endian system.
#else
	/* Little endian copy of the size into the WT_CELL. */
	v = (uint8_t *)&size;
	cell->__cell_chunk[1] = v[0];
	cell->__cell_chunk[2] = v[1];
	cell->__cell_chunk[3] = v[2];
	cell->__cell_chunk[4] = v[3];
#endif
}

/*
 * __wt_cell_data --
 *	Return the first byte of data for a cell.
 */
static inline void *
__wt_cell_data(const WT_CELL *cell)
{
	switch (WT_CELL_BYTES(cell)) {
	case WT_CELL_1_BYTE:
		return ((u_int8_t *)cell + 2);
	case WT_CELL_2_BYTE:
		return ((u_int8_t *)cell + 3);
	case WT_CELL_3_BYTE:
		return ((u_int8_t *)cell + 4);
	case WT_CELL_4_BYTE:
	default:
		return ((u_int8_t *)cell + 5);
	}
}

/*
 * __wt_cell_datalen --
 *	Return the number of data bytes referenced by a WT_CELL.
 */
static inline u_int32_t
__wt_cell_datalen(const WT_CELL *cell)
{
	uint32_t _v;
	uint8_t *p, *v;

	v = (uint8_t *)&_v;
	_v = 0;

	p = (uint8_t *)cell + 1;

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
 * __wt_cell_data_and_len --
 *	Fill in both the first byte of data for a cell as well as the length.
 */
static inline void
__wt_cell_data_and_len(const WT_CELL *cell, void *p, uint32_t *sizep)
{
	*(void **)p = __wt_cell_data(cell);
	*sizep = __wt_cell_datalen(cell);
}

/*
 * __wt_cell_len --
 *	Return the total bytes taken up by a WT_CELL on page, including the
 * trailing data.
 */
static inline uint32_t
__wt_cell_len(const WT_CELL *cell)
{
	uint32_t i;

	switch (WT_CELL_BYTES(cell)) {
	case WT_CELL_1_BYTE:
		i = 2;
		break;
	case WT_CELL_2_BYTE:
		i = 3;
		break;
	case WT_CELL_3_BYTE:
		i = 4;
		break;
	case WT_CELL_4_BYTE:
	default:
		i = 5;
		break;
	}
	return (i + __wt_cell_datalen(cell));
}

/*
 * __wt_cell_next --
 *	Return a pointer to the next WT_CELL on the page.
 */
static inline void *
__wt_cell_next(const WT_CELL *cell)
{
	return ((u_int8_t *)cell + __wt_cell_len(cell));
}

/*
 * __wt_cell_ovfl --
 *	Copy out a WT_CELL that references a WT_OVFL structure.
 */
static inline void
__wt_cell_ovfl(const WT_CELL *cell, WT_OVFL *ovfl)
{
	/* Version for systems that support unaligned access. */
	*ovfl = *(WT_OVFL *)__wt_cell_data(cell);
}

/*
 * __wt_cell_off --
 *	Copy out a WT_CELL that references a WT_OFF structure.
 */
static inline void
__wt_cell_off(const WT_CELL *cell, WT_OFF *off)
{
	/* Version for systems that support unaligned access. */
	*off = *(WT_OFF *)__wt_cell_data(cell);
}

/*
 * __wt_cell_off_record --
 *	Copy out a WT_CELL that references a WT_OFF_RECORD structure.
 */
static inline void
__wt_cell_off_record(const WT_CELL *cell, WT_OFF_RECORD *off_record)
{
	/* Version for systems that support unaligned access. */
	*off_record = *(WT_OFF_RECORD *)__wt_cell_data(cell);
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
	if (WT_CELL_TYPE(key_cell) != WT_CELL_KEY &&
	    WT_CELL_TYPE(key_cell) != WT_CELL_KEY_OVFL)
		key_cell = __wt_cell_next(key_cell);
	return (key_cell);
}
