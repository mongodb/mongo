/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_err_delfmt(SESSION *, uint32_t, uint32_t);
static int __wt_err_eof(SESSION *, uint32_t, uint32_t);
static int __wt_err_eop(SESSION *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_fix(
    SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_int(
    SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_rle(
    SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_cell(
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

	/*
	 * FUTURE:
	 * Check the LSN against the existing log files.
	 */

	/* Ignore the checksum -- it verified when we first read the page. */

	if (dsk->size != size) {
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
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		WT_RET(__wt_verify_dsk_cell(session, dsk, addr, size));
		break;
	case WT_PAGE_COL_INT:
		WT_RET(__wt_verify_dsk_col_int(session, dsk, addr, size));
		break;
	case WT_PAGE_COL_FIX:
		WT_RET(__wt_verify_dsk_col_fix(session, dsk, addr, size));
		break;
	case WT_PAGE_COL_RLE:
		WT_RET(__wt_verify_dsk_col_rle(session, dsk, addr, size));
		break;
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
		WT_RET(__wt_verify_dsk_chunk(session, dsk, addr, size));
		break;
	WT_ILLEGAL_FORMAT(session);
	}

	return (0);
}

/*
 * __wt_verify_dsk_cell --
 *	Walk a disk page of WT_CELLs, and verify them.
 */
static int
__wt_verify_dsk_cell(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	enum { IS_FIRST, WAS_KEY, WAS_DATA } last_item_type;
	struct {
		WT_ITEM	 item;			/* WT_ITEM to compare */
		WT_BUF	*scratch;		/* scratch buffer */
	} *current, *last, *tmp, _a, _b;
	BTREE *btree;
	WT_CELL *cell;
	WT_OVFL *ovfl;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;
	off_t file_size;
	uint8_t *end;
	void *huffman;
	uint32_t i, cell_num, cell_len, cell_type;
	int (*func)(BTREE *, const WT_ITEM *, const WT_ITEM *), ret;

	btree = session->btree;
	func = btree->btree_compare;
	huffman = btree->huffman_key;
	ret = 0;

	WT_CLEAR(_a);
	WT_CLEAR(_b);
	current = &_a;
	WT_ERR(__wt_scr_alloc(session, 0, &current->scratch));
	last = &_b;
	WT_ERR(__wt_scr_alloc(session, 0, &last->scratch));

	file_size = btree->fh->file_size;
	end = (uint8_t *)dsk + size;

	last_item_type = IS_FIRST;
	cell_num = 0;
	WT_CELL_FOREACH(dsk, cell, i) {
		++cell_num;

		/* Check if this item is entirely on the page. */
		if ((uint8_t *)cell + sizeof(WT_CELL) > end)
			goto eop;

		cell_type = WT_CELL_TYPE(cell);
		cell_len = WT_CELL_LEN(cell);

		/* Check the cell's type. */
		switch (cell_type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			if (dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto cell_vs_page;
			break;
		case WT_CELL_DATA:
		case WT_CELL_DATA_OVFL:
			if (dsk->type != WT_PAGE_COL_VAR &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto cell_vs_page;
			break;
		case WT_CELL_DEL:
			/* Deleted items only appear on column-store pages. */
			if (dsk->type != WT_PAGE_COL_VAR)
				goto cell_vs_page;
			break;
		case WT_CELL_OFF:
			if (dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto cell_vs_page;
			break;
		case WT_CELL_OFF_RECORD:
			if (dsk->type != WT_PAGE_ROW_LEAF) {
cell_vs_page:			__wt_errx(session,
				    "illegal cell and page type combination "
				    "(cell %lu on page at addr %lu is a %s "
				    "cell on a %s page)",
				    (u_long)cell_num, (u_long)addr,
				    __wt_cell_type_string(cell),
				    __wt_page_type_string(dsk->type));
				return (WT_ERROR);
			}
			break;
		default:
			__wt_errx(session,
			    "cell %lu on page at addr %lu has an illegal type "
			    "of %lu",
			    (u_long)cell_num, (u_long)addr, (u_long)cell_type);
			return (WT_ERROR);
		}

		/*
		 * Only row-store leaf pages require cell type ordering checks,
		 * other page types don't have ordering relationships between
		 * their WT_CELL entries, and validating the correct types above
		 * is sufficient.
		 *
		 * For row-store leaf pages, check for:
		 *	two values in a row,
		 *	a value as the first cell on a page.
		 */
		if (dsk->type == WT_PAGE_ROW_LEAF)
			switch (cell_type) {
			case WT_CELL_KEY:
			case WT_CELL_KEY_OVFL:
				last_item_type = WAS_KEY;
				break;
			case WT_CELL_DATA:
			case WT_CELL_DATA_OVFL:
			case WT_CELL_DEL:
				switch (last_item_type) {
				case IS_FIRST:
					__wt_errx(session,
					    "page at addr %lu begins with a "
					    "value",
					    (u_long)addr);
					return (WT_ERROR);
				case WAS_DATA:
					__wt_errx(session,
					    "cell %lu on page at addr %lu is "
					    "the first of two adjacent values",
					    (u_long)cell_num - 1, (u_long)addr);
					return (WT_ERROR);
				case WAS_KEY:
					last_item_type = WAS_DATA;
					break;
				}
				break;
			}

		/* Check the cell's length. */
		switch (cell_type) {
		case WT_CELL_KEY:
		case WT_CELL_DATA:
			/* The length is variable, we can't check it. */
			break;
		case WT_CELL_KEY_OVFL:
		case WT_CELL_DATA_OVFL:
			if (cell_len != sizeof(WT_OVFL))
				goto cell_len;
			break;
		case WT_CELL_DEL:
			if (cell_len != 0)
				goto cell_len;
			break;
		case WT_CELL_OFF:
			if (cell_len != sizeof(WT_OFF))
				goto cell_len;
			break;
		case WT_CELL_OFF_RECORD:
			if (cell_len != sizeof(WT_OFF_RECORD)) {
cell_len:			__wt_errx(session,
				    "cell %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)cell_num, (u_long)addr);
				return (WT_ERROR);
			}
			break;
		default:
			break;
		}

		/* Check if the cell is entirely on the page. */
		if ((uint8_t *)WT_CELL_NEXT(cell) > end)
			goto eop;

		/* Check if the referenced item is entirely in the file. */
		switch (cell_type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_DATA_OVFL:
			ovfl = WT_CELL_BYTE_OVFL(cell);
			if (WT_ADDR_TO_OFF(btree, ovfl->addr) +
			    (off_t)WT_HDR_BYTES_TO_ALLOC(btree, ovfl->size) >
			    file_size)
				goto eof;
			break;
		case WT_CELL_OFF:
			off = WT_CELL_BYTE_OFF(cell);
			if (WT_ADDR_TO_OFF(btree, off->addr) +
			    (off_t)off->size > file_size)
				goto eof;
			break;
		case WT_CELL_OFF_RECORD:
			off_record = WT_CELL_BYTE_OFF_RECORD(cell);
			if (WT_ADDR_TO_OFF(btree, off_record->addr) +
			    (off_t)off_record->size > file_size)
				goto eof;
			break;
		}

		/*
		 * The only remaining task is to check key ordering: row-store
		 * internal and leaf page keys must be ordered, variable-length
		 * column store pages have no such requirement.
		 */
		if (dsk->type == WT_PAGE_COL_VAR)
			continue;

		/* Build the keys and compare them, skipping values. */
		switch (cell_type) {
		case WT_CELL_KEY:
			if (huffman == NULL) {
				current->item.data = WT_CELL_BYTE(cell);
				current->item.size = WT_CELL_LEN(cell);
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
eop:		ret = __wt_err_eop(session, cell_num, addr);
	}

err:	if (_a.scratch != NULL)
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
	p = (uint8_t *)dsk + (WT_PAGE_DISK_SIZE + dsk->u.datalen);
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
