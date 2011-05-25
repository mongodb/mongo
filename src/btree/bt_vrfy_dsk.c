/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "cell.i"

static int __wt_err_cell_vs_page(
	SESSION *, uint32_t, uint32_t, WT_CELL *, WT_PAGE_DISK *);
static int __wt_err_delfmt(SESSION *, uint32_t, uint32_t);
static int __wt_err_eof(SESSION *, uint32_t, uint32_t);
static int __wt_err_eop(SESSION *, uint32_t, uint32_t);
static int __wt_verify_cell(
	SESSION *, WT_CELL *, uint32_t, uint32_t, uint8_t *);
static int __wt_verify_dsk_row(SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_fix(
	SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_int(
	SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_rle(
	SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_var(
	SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);

/*
 * __wt_verify_dsk_page --
 *	Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk_page(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	/* Check the page type. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_INVALID:
	default:
		__wt_errx(session,
		    "page at addr %lu has an invalid type of %lu",
		    (u_long)addr, (u_long)dsk->type);
		return (WT_ERROR);
	}

	/* Check the page record number. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_RLE:
	case WT_PAGE_COL_VAR:
		if (dsk->recno == 0) {
			__wt_errx(session,
			    "%s page at addr %lu has a record number of zero",
			    __wt_page_type_string(dsk->type), (u_long)addr);
			return (WT_ERROR);
		}
		break;
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if (dsk->recno != 0) {
			__wt_errx(session,
			    "%s page at addr %lu has a non-zero record number",
			    __wt_page_type_string(dsk->type), (u_long)addr);
			return (WT_ERROR);
		}
		break;
	}

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	/* Ignore the checksum -- it verified when we first read the page. */

	/* The in-memory and on-disk page sizes are currently the same. */
	if (dsk->size != size || dsk->memsize != size) {
		__wt_errx(session,
		    "page at addr %lu has an incorrect size", (u_long)addr);
		return (WT_ERROR);
	}

	if (dsk->unused[0] != '\0' ||
	    dsk->unused[1] != '\0' || dsk->unused[2] != '\0') {
		__wt_errx(session,
		    "page at addr %lu has non-zero unused header fields",
		    (u_long)addr);
		return (WT_ERROR);
	}

	/* Verify the items on the page. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		return (__wt_verify_dsk_col_int(session, dsk, addr, size));
	case WT_PAGE_COL_FIX:
		return (__wt_verify_dsk_col_fix(session, dsk, addr, size));
	case WT_PAGE_COL_RLE:
		return (__wt_verify_dsk_col_rle(session, dsk, addr, size));
	case WT_PAGE_COL_VAR:
		return (__wt_verify_dsk_col_var(session, dsk, addr, size));
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		return (__wt_verify_dsk_row(session, dsk, addr, size));
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
		return (__wt_verify_dsk_chunk(session, dsk, addr, size));
	WT_ILLEGAL_FORMAT(session);
	}
	/* NOTREACHED */
}

/*
 * __wt_verify_dsk_row --
 *	Walk a WT_PAGE_ROW_INT or WT_PAGE_ROW_LEAF disk page and verify it.
 */
static int
__wt_verify_dsk_row(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
	struct {
		WT_ITEM	 item;			/* WT_ITEM to compare */
		WT_BUF	*scratch;		/* scratch buffer */
	} *current, *last, *tmp, _a, _b;
	BTREE *btree;
	WT_CELL *cell;
	WT_OFF off;
	off_t file_size;
	void *huffman;
	uint32_t cell_num, cell_type, i, prefix;
	uint8_t *end;
	int pfx_chk, ret;
	int (*func)(BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	func = btree->btree_compare;
	huffman = btree->huffman_key;
	pfx_chk = 1;
	ret = 0;

	WT_CLEAR(_a);
	WT_CLEAR(_b);
	current = &_a;
	WT_ERR(__wt_scr_alloc(session, 0, &current->scratch));
	last = &_b;
	WT_ERR(__wt_scr_alloc(session, 0, &last->scratch));

	file_size = btree->fh->file_size;
	end = (uint8_t *)dsk + size;

	last_cell_type = FIRST;
	cell_num = 0;
	WT_CELL_FOREACH(dsk, cell, i) {
		++cell_num;

		/* Check the cell itself. */
		WT_RET(__wt_verify_cell(session, cell, cell_num, addr, end));

		/* Check the cell type. */
		cell_type = __wt_cell_type(cell);
		switch (cell_type) {
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			break;
		case WT_CELL_OFF:
			if (dsk->type == WT_PAGE_ROW_INT)
				break;
			/* FALLTHROUGH */
		default:
			return (__wt_err_cell_vs_page(
			    session, cell_num, addr, cell, dsk));
		}

		/*
		 * Check ordering relationships between the WT_CELL entries.
		 * For row-store internal pages, check for:
		 *	two values in a row,
		 *	two keys in a row,
		 *	a value as the first cell on a page.
		 * For row-store leaf pages, check for:
		 *	two values in a row,
		 *	a value as the first cell on a page.
		 */
		switch (cell_type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			switch (last_cell_type) {
			case FIRST:
			case WAS_VALUE:
				break;
			case WAS_KEY:
				if (dsk->type == WT_PAGE_ROW_LEAF)
					break;
				__wt_errx(session,
				    "cell %lu on page at addr %lu is the first "
				    "of two adjacent keys",
				    (u_long)cell_num - 1, (u_long)addr);
				goto err;
			}
			last_cell_type = WAS_KEY;
			break;
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
		case WT_CELL_OFF:
			switch (last_cell_type) {
			case FIRST:
				__wt_errx(session,
				    "page at addr %lu begins with a value",
				    (u_long)addr);
				goto err;
			case WAS_KEY:
				break;
			case WAS_VALUE:
				__wt_errx(session,
				    "cell %lu on page at addr %lu is the first "
				    "of two adjacent values",
				    (u_long)cell_num - 1, (u_long)addr);
				goto err;
			}
			last_cell_type = WAS_VALUE;
			break;
		}

		/* Check if any referenced item is entirely in the file. */
		switch (cell_type) {
		case WT_CELL_DATA_OVFL:
		case WT_CELL_KEY_OVFL:
		case WT_CELL_OFF:
			__wt_cell_off(cell, &off);
			if (WT_ADDR_TO_OFF(btree,
			    off.addr) + off.size > file_size)
				goto eof;
			break;
		}

		/*
		 * Check key order and prefix compression.
		 *
		 * Build the key.
		 */
		switch (cell_type) {
		case WT_CELL_KEY:
			if (huffman == NULL) {
				__wt_cell_data_and_len(cell,
				    &current->item.data, &current->item.size);
				break;
			}
			/* FALLTHROUGH */
		case WT_CELL_KEY_OVFL:
			WT_RET(
			    __wt_cell_process(session, cell, current->scratch));
			current->item = *(WT_ITEM *)current->scratch;
			break;
		default:
			continue;
		}

		if (cell_type == WT_CELL_KEY) {
			prefix = __wt_cell_prefix(cell);
			/*
			 * Confirm the first non-overflow key on a page has a
			 * zero prefix compression count.
			 */
			if (pfx_chk) {
				pfx_chk = 0;
				if (prefix != 0) {
					__wt_errx(session,
					    "the %lu key on page at addr %lu "
					    "is the first non-overflow key on "
					    "the page and has a non-zero "
					    "prefix compression value",
					    (u_long)cell_num, (u_long)addr);
					goto err;
				}
			}

			/*
			 * Confirm the last and current keys match up to the
			 * current key's prefix compression count.
			 */
			if (prefix > last->item.size) {
				__wt_errx(session,
				    "the %lu key on page at addr %lu has a "
				    "prefix compression count of %lu larger "
				    "than the length of the previous key, %lu",
				    (u_long)cell_num, (u_long)addr,
				    (u_long)prefix, (u_long)last->item.size);
				goto err;
			}
			if (prefix != 0 && memcmp(
			    last->item.data, current->item.data, prefix) != 0) {
				__wt_errx(session,
				    "the %lu key on page at addr %lu is not "
				    "identical to the previous key up to its "
				    "prefix compression count of %lu",
				    (u_long)cell_num,
				    (u_long)addr, (u_long)prefix);
				goto err;
			}
		}

		/* Compare the current key against the last key. */
		if (cell_num > 1 &&
		    func(btree, &last->item, &current->item) >= 0) {
			__wt_errx(session,
			    "the %lu and %lu keys on page at addr %lu are "
			    "incorrectly sorted",
			    (u_long)cell_num - 2,
			    (u_long)cell_num, (u_long)addr);
			ret = WT_ERROR;
			goto err;
		}
		tmp = last;
		last = current;
		current = tmp;
	}

	if (0) {
eof:		ret = __wt_err_eof(session, cell_num, addr);
	}

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}
	if (_a.scratch != NULL)
		__wt_scr_release(&_a.scratch);
	if (_b.scratch != NULL)
		__wt_scr_release(&_b.scratch);
	return (ret);
}

/*
 * __wt_verify_dsk_col_int --
 *	Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__wt_verify_dsk_col_int(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
	WT_OFF_RECORD *off_record;
	uint8_t *end;
	uint32_t i, entry_num;

	btree = session->btree;
	end = (uint8_t *)dsk + size;

	entry_num = 0;
	WT_OFF_FOREACH(dsk, off_record, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if ((uint8_t *)off_record + sizeof(WT_OFF_RECORD) > end)
			return (__wt_err_eop(session, entry_num, addr));

		/* Check if the reference is past the end-of-file. */
		if (WT_ADDR_TO_OFF(btree, off_record->addr) +
		    (off_t)off_record->size > btree->fh->file_size)
			return (__wt_err_eof(session, entry_num, addr));
	}

	return (0);
}

/*
 * __wt_verify_dsk_col_fix --
 *	Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__wt_verify_dsk_col_fix(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
	u_int len;
	uint32_t i, j, entry_num;
	uint8_t *data, *end, *p;

	btree = session->btree;
	len = btree->fixed_len;
	end = (uint8_t *)dsk + size;

	entry_num = 0;
	WT_FIX_FOREACH(btree, dsk, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_err_eop(session, entry_num, addr));

		/* Deleted items are entirely nul bytes. */
		p = data;
		if (WT_FIX_DELETE_ISSET(data)) {
			if (*p != WT_FIX_DELETE_BYTE)
				goto delfmt;
			for (j = 1; j < btree->fixed_len; ++j)
				if (*++p != '\0')
					goto delfmt;
		}
	}

	return (0);

delfmt:	return (__wt_err_delfmt(session, entry_num, addr));
}

/*
 * __wt_verify_dsk_col_rle --
 *	Walk a WT_PAGE_COL_RLE disk page and verify it.
 */
static int
__wt_verify_dsk_col_rle(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
	u_int len;
	uint32_t i, j, entry_num;
	uint8_t *data, *end, *last_data, *p;

	btree = session->btree;
	end = (uint8_t *)dsk + size;

	last_data = NULL;
	len = btree->fixed_len + WT_SIZEOF32(uint16_t);

	entry_num = 0;
	WT_RLE_REPEAT_FOREACH(btree, dsk, data, i) {
		++entry_num;

		/* Check if this entry is entirely on the page. */
		if (data + len > end)
			return (__wt_err_eop(session, entry_num, addr));

		/* Count must be non-zero. */
		if (WT_RLE_REPEAT_COUNT(data) == 0) {
			__wt_errx(session,
			    "fixed-length entry %lu on page at addr "
			    "%lu has a repeat count of 0",
			    (u_long)entry_num, (u_long)addr);
			return (WT_ERROR);
		}

		/* Deleted items are entirely nul bytes. */
		p = WT_RLE_REPEAT_DATA(data);
		if (WT_FIX_DELETE_ISSET(p)) {
			if (*p != WT_FIX_DELETE_BYTE)
				goto delfmt;
			for (j = 1; j < btree->fixed_len; ++j)
				if (*++p != '\0')
					goto delfmt;
		}

		/*
		 * If the previous data is the same as this data, we
		 * missed an opportunity for compression -- complain.
		 */
		if (last_data != NULL &&
		    memcmp(WT_RLE_REPEAT_DATA(last_data),
		    WT_RLE_REPEAT_DATA(data), btree->fixed_len) == 0 &&
		    WT_RLE_REPEAT_COUNT(last_data) < UINT16_MAX) {
			__wt_errx(session,
			    "fixed-length entries %lu and %lu on page "
			    "at addr %lu are identical and should have "
			    "been compressed",
			    (u_long)entry_num,
			    (u_long)entry_num - 1, (u_long)addr);
			return (WT_ERROR);
		}
		last_data = data;
	}

	return (0);

delfmt:	return (__wt_err_delfmt(session, entry_num, addr));
}

/*
 * __wt_verify_dsk_col_var --
 *	Walk a WT_PAGE_COL_VAR disk page and verify it.
 */
static int
__wt_verify_dsk_col_var(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;
	WT_CELL *cell;
	WT_OFF off;
	off_t file_size;
	uint32_t cell_num, cell_type, i;
	uint8_t *end;

	btree = session->btree;
	file_size = btree->fh->file_size;
	end = (uint8_t *)dsk + size;

	cell_num = 0;
	WT_CELL_FOREACH(dsk, cell, i) {
		++cell_num;

		/* Check the cell itself. */
		WT_RET(__wt_verify_cell(session, cell, cell_num, addr, end));

		/* Check the cell type. */
		cell_type = __wt_cell_type_raw(cell);
		switch (cell_type) {
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
		case WT_CELL_DATA_SHORT:
		case WT_CELL_DEL:
			break;
		default:
			return (__wt_err_cell_vs_page(
			    session, cell_num, addr, cell, dsk));
		}

		/* Check if any referenced item is entirely in the file. */
		if (cell_type == WT_CELL_DATA_OVFL) {
			__wt_cell_off(cell, &off);
			if (WT_ADDR_TO_OFF(btree,
			    off.addr) + off.size > file_size)
				return (__wt_err_eof(session, cell_num, addr));
		}
	}
	return (0);
}

/*
 * __wt_verify_dsk_chunk --
 *	Verify the WT_PAGE_FREELIST and WT_PAGE_OVFL disk pages.
 */
int
__wt_verify_dsk_chunk(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	uint32_t len;
	uint8_t *p;

	/*
	 * Overflow and freelist pages are roughly identical, both are simply
	 * chunks of data.   This routine should also be used for any chunks
	 * of data we store in the file in the future.
	 */
	if (dsk->u.datalen == 0) {
		__wt_errx(session,
		    "%s page at addr %lu has no data",
		    __wt_page_type_string(dsk->type), (u_long)addr);
		return (WT_ERROR);
	}

	/* Any data after the data chunk should be nul bytes. */
	p = (uint8_t *)WT_PAGE_DISK_BYTE(dsk) + dsk->u.datalen;
	len = size - (WT_PAGE_DISK_SIZE + dsk->u.datalen);
	for (; len > 0; ++p, --len)
		if (*p != '\0') {
			__wt_errx(session,
			    "%s page at addr %lu has non-zero trailing bytes",
			    __wt_page_type_string(dsk->type), (u_long)addr);
			return (WT_ERROR);
		}

	return (0);
}

/*
 * __wt_verify_cell --
 *	Check to see if a cell is safe.
 */
static int
__wt_verify_cell(SESSION *session,
    WT_CELL *cell, uint32_t cell_num, uint32_t addr, uint8_t *end)
{
	uint8_t *p;

	/*
	 * Check if this cell is on the page, and once we know the cell
	 * is safe, check if the cell's data is entirely on the page.
	 *
	 * Delete and off-page items have known sizes, we don't store length
	 * bytes.  Short key/data items have 6- or 7-bits of size in the
	 * descriptor byte and no length bytes.  In both cases, the data is
	 * after the single byte WT_CELL.
	 */
	p = (uint8_t *)cell;
	
	switch (__wt_cell_type_raw(cell)) {
	case WT_CELL_DATA_OVFL:
	case WT_CELL_DATA_SHORT:
	case WT_CELL_DEL:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_KEY_SHORT:
	case WT_CELL_OFF:
		p += 1;
		break;
	case WT_CELL_DATA:
	case WT_CELL_KEY:
		switch (WT_CELL_BYTES(cell)) {
			case WT_CELL_1_BYTE:
				p += 2;
				break;
			case WT_CELL_2_BYTE:
				p += 3;
				break;
			case WT_CELL_3_BYTE:
				p += 4;
				break;
			case WT_CELL_4_BYTE:
			default:
				p += 5;
				break;
			}
		break;
	default:
		/*
		 * Don't worry about illegal types -- our caller will check,
		 * based on its page type.
		 */
		break;
	}
	if (p > end || (uint8_t *)(cell) + __wt_cell_len(cell) > end)
		return (__wt_err_eop(session, cell_num, addr));
	return (0);
}

/*
 * __wt_err_cell_vs_page --
 *	Generic illegal cell type for a particular page type error.
 */
static int
__wt_err_cell_vs_page(SESSION *session,
    uint32_t entry_num, uint32_t addr, WT_CELL *cell, WT_PAGE_DISK *dsk)
{
	__wt_errx(session,
	    "illegal cell and page type combination cell %lu on page at addr "
	    "%lu is a %s cell on a %s page",
	    (u_long)entry_num, (u_long)addr,
	    __wt_cell_type_string(cell),
	    __wt_page_type_string(dsk->type));
	return (WT_ERROR);
}

/*
 * __wt_err_eop --
 *	Generic item extends past the end-of-page error.
 */
static int
__wt_err_eop(SESSION *session, uint32_t entry_num, uint32_t addr)
{
	__wt_errx(session,
	    "item %lu on page at addr %lu extends past the end of the page",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_err_eof --
 *	Generic item references non-existent file pages error.
 */
static int
__wt_err_eof(SESSION *session, uint32_t entry_num, uint32_t addr)
{
	__wt_errx(session,
	    "off-page item %lu on page at addr %lu references non-existent "
	    "file pages",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}

/*
 * __wt_err_delfmt --
 *	WT_PAGE_COL_FIX and WT_PAGE_COL_RLE error where a deleted item has
 *	non-nul bytes.
 */
static int
__wt_err_delfmt(SESSION *session, uint32_t entry_num, uint32_t addr)
{
	__wt_errx(session,
	    "deleted fixed-length entry %lu on page at addr %lu has non-nul "
	    "bytes",
	    (u_long)entry_num, (u_long)addr);
	return (WT_ERROR);
}
