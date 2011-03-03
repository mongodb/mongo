/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"
#include "bt_inline.c"

static int __wt_err_delfmt(SESSION *, uint32_t, uint32_t);
static int __wt_err_eof(SESSION *, uint32_t, uint32_t);
static int __wt_err_eop(SESSION *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_fix(SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_int(SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_col_rle(SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);
static int __wt_verify_dsk_item(SESSION *, WT_PAGE_DISK *, uint32_t, uint32_t);

/*
 * __wt_verify_dsk_page --
 *	Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk_page(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	BTREE *btree;

	btree = session->btree;

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

	if (dsk->unused[0] != '\0' || dsk->unused[1] != '\0') {
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
		WT_RET(__wt_verify_dsk_item(session, dsk, addr, size));
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
	WT_ILLEGAL_FORMAT(btree);
	}

	return (0);
}

/*
 * __wt_verify_dsk_item --
 *	Walk a disk page of WT_ITEMs, and verify them.
 */
static int
__wt_verify_dsk_item(
    SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
{
	enum { IS_FIRST, WAS_KEY, WAS_DATA } last_item_type;
	struct {
		WT_ROW   rip;			/* Reference to on-page data */
		WT_DATAITEM	*dbt;			/* WT_DATAITEM to compare */
		WT_SCRATCH	*scratch;		/* scratch buffer */
	} *current, *last, *tmp, _a, _b;
	BTREE *btree;
	WT_ITEM *item;
	WT_OVFL *ovfl;
	WT_OFF *off;
	WT_OFF_RECORD *off_record;
	WT_ROW *rip;
	off_t file_size;
	uint8_t *end;
	void *huffman;
	uint32_t i, item_num, item_len, item_type;
	int (*func)(BTREE *, const WT_DATAITEM *, const WT_DATAITEM *), ret;

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
	item_num = 0;
	WT_ITEM_FOREACH(dsk, item, i) {
		++item_num;

		/* Check if this item is entirely on the page. */
		if ((uint8_t *)item + sizeof(WT_ITEM) > end)
			goto eop;

		item_type = WT_ITEM_TYPE(item);
		item_len = WT_ITEM_LEN(item);

		/* Check the item's type. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_KEY_OVFL:
			if (dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DATA:
		case WT_ITEM_DATA_OVFL:
			if (dsk->type != WT_PAGE_COL_VAR &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_DEL:
			/* Deleted items only appear on column-store pages. */
			if (dsk->type != WT_PAGE_COL_VAR)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF:
			if (dsk->type != WT_PAGE_ROW_INT &&
			    dsk->type != WT_PAGE_ROW_LEAF)
				goto item_vs_page;
			break;
		case WT_ITEM_OFF_RECORD:
			if (dsk->type != WT_PAGE_ROW_LEAF) {
item_vs_page:			__wt_errx(session,
				    "illegal item and page type combination "
				    "(item %lu on page at addr %lu is a %s "
				    "item on a %s page)",
				    (u_long)item_num, (u_long)addr,
				    __wt_item_type_string(item),
				    __wt_page_type_string(dsk));
				return (WT_ERROR);
			}
			break;
		default:
			__wt_errx(session,
			    "item %lu on page at addr %lu has an illegal type "
			    "of %lu",
			    (u_long)item_num, (u_long)addr, (u_long)item_type);
			return (WT_ERROR);
		}

		/*
		 * Only row-store leaf pages require item type ordering checks,
		 * other page types don't have ordering relationships between
		 * their WT_ITEM entries, and validating the correct types above
		 * is sufficient.
		 *
		 * For row-store leaf pages, check for:
		 *	two data items in a row,
		 *	a data item as the first item on a page.
		 */
		if (dsk->type == WT_PAGE_ROW_LEAF)
			switch (item_type) {
			case WT_ITEM_KEY:
			case WT_ITEM_KEY_OVFL:
				last_item_type = WAS_KEY;
				break;
			case WT_ITEM_DATA:
			case WT_ITEM_DATA_OVFL:
			case WT_ITEM_DEL:
				switch (last_item_type) {
				case IS_FIRST:
					__wt_errx(session,
					    "page at addr %lu begins with a "
					    "data item",
					    (u_long)addr);
					return (WT_ERROR);
				case WAS_DATA:
					__wt_errx(session,
					    "item %lu on page at addr %lu is "
					    "the first of two adjacent data "
					    "items",
					    (u_long)item_num - 1, (u_long)addr);
					return (WT_ERROR);
				case WAS_KEY:
					last_item_type = WAS_DATA;
					break;
				}
				break;
			}

		/* Check the item's length. */
		switch (item_type) {
		case WT_ITEM_KEY:
		case WT_ITEM_DATA:
			/* The length is variable, we can't check it. */
			break;
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
			if (item_len != sizeof(WT_OVFL))
				goto item_len;
			break;
		case WT_ITEM_DEL:
			if (item_len != 0)
				goto item_len;
			break;
		case WT_ITEM_OFF:
			if (item_len != sizeof(WT_OFF))
				goto item_len;
			break;
		case WT_ITEM_OFF_RECORD:
			if (item_len != sizeof(WT_OFF_RECORD)) {
item_len:			__wt_errx(session,
				    "item %lu on page at addr %lu has an "
				    "incorrect length",
				    (u_long)item_num, (u_long)addr);
				return (WT_ERROR);
			}
			break;
		default:
			break;
		}

		/* Check if the item is entirely on the page. */
		if ((uint8_t *)WT_ITEM_NEXT(item) > end)
			goto eop;

		/* Check if the referenced item is entirely in the file. */
		switch (item_type) {
		case WT_ITEM_KEY_OVFL:
		case WT_ITEM_DATA_OVFL:
			ovfl = WT_ITEM_BYTE_OVFL(item);
			if (WT_ADDR_TO_OFF(btree, ovfl->addr) +
			    WT_HDR_BYTES_TO_ALLOC(btree, ovfl->size) > file_size)
				goto eof;
			break;
		case WT_ITEM_OFF:
			off = WT_ITEM_BYTE_OFF(item);
			if (WT_ADDR_TO_OFF(btree,
			    off->addr) + off->size > file_size)
				goto eof;
			break;
		case WT_ITEM_OFF_RECORD:
			off_record = WT_ITEM_BYTE_OFF_RECORD(item);
			if (WT_ADDR_TO_OFF(btree,
			    off_record->addr) + off_record->size > file_size)
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

		/*
		 * Skip data items.
		 * Otherwise build the keys and compare them.
		 */
		rip = &current->rip;
		switch (item_type) {
		case WT_ITEM_KEY:
			if (huffman == NULL) {
				__wt_key_set(rip,
				    WT_ITEM_BYTE(item), WT_ITEM_LEN(item));
				break;
			}
			/* FALLTHROUGH */
		case WT_ITEM_KEY_OVFL:
			__wt_key_set_process(rip, item);
			break;
		default:
			continue;
		}

		if (__wt_key_process(rip)) {
			WT_RET(
			    __wt_item_process(session, rip->key, current->scratch));
			current->dbt = &current->scratch->item;
		} else
			current->dbt = (WT_DATAITEM *)rip;

		/* Compare the current key against the last key. */
		if (last->dbt != NULL &&
		    func(btree, last->dbt, current->dbt) >= 0) {
			__wt_errx(session,
			    "the %lu and %lu keys on page at addr %lu are "
			    "incorrectly sorted",
			    (u_long)item_num - 2,
			    (u_long)item_num, (u_long)addr);
			ret = WT_ERROR;
			goto err;
		}
		tmp = last;
		last = current;
		current = tmp;
	}

	if (0) {
eof:		ret = __wt_err_eof(session, item_num, addr);
	}
	if (0) {
eop:		ret = __wt_err_eop(session, item_num, addr);
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
__wt_verify_dsk_col_int(SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
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
		if (WT_ADDR_TO_OFF(btree,
		    off_record->addr) + off_record->size > btree->fh->file_size)
			return (__wt_err_eof(session, entry_num, addr));
	}

	return (0);
}

/*
 * __wt_verify_dsk_col_fix --
 *	Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__wt_verify_dsk_col_fix(SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
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
__wt_verify_dsk_col_rle(SESSION *session, WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size)
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
	BTREE *btree;
	uint32_t len;
	uint8_t *p;

	btree = session->btree;

	/*
	 * Overflow and freelist pages are roughly identical, both are simply
	 * chunks of data.   This routine should also be used for any chunks
	 * of data we store in the file in the future.
	 */
	if (dsk->u.datalen == 0) {
		__wt_errx(session,
		    "%s page at addr %lu has no data",
		    __wt_page_type_string(dsk), (u_long)addr);
		return (WT_ERROR);
	}

	/* Any data after the data chunk should be nul bytes. */
	p = (uint8_t *)dsk + (WT_PAGE_DISK_SIZE + dsk->u.datalen);
	len = size - (WT_PAGE_DISK_SIZE + dsk->u.datalen);
	for (; len > 0; ++p, --len)
		if (*p != '\0') {
			__wt_errx(session,
			    "%s page at addr %lu has non-zero trailing bytes",
			    __wt_page_type_string(dsk), (u_long)addr);
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
