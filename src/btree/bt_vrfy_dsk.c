/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __err_cell_corrupted(WT_SESSION_IMPL *, uint32_t, uint32_t, int);
static int __err_cell_type(
	WT_SESSION_IMPL *, uint32_t, uint32_t, uint8_t, WT_PAGE_DISK *, int);
static int __err_eof(WT_SESSION_IMPL *, uint32_t, uint32_t, int);
static int __err_eop(WT_SESSION_IMPL *, uint32_t, uint32_t, int);
static int __verify_dsk_chunk(
	WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, uint32_t, uint32_t, int);
static int __verify_dsk_col_fix(
	WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, uint32_t, int);
static int __verify_dsk_col_int(
	WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, uint32_t, int);
static int __verify_dsk_col_var(
	WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, uint32_t, int);
static int __verify_dsk_row(
	WT_SESSION_IMPL *, WT_PAGE_DISK *, uint32_t, uint32_t, int);

#define	WT_VRFY_ERR(session, quiet, ...) do {				\
	if (!(quiet))							\
		WT_FAILURE(session, __VA_ARGS__);			\
} while (0)

/*
 * __wt_verify_dsk --
 *	Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size, int quiet)
{
	/* Check the page type. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_INVALID:
	default:
		WT_VRFY_ERR(session, quiet,
		    "page at addr %" PRIu32 " has an invalid type of %" PRIu32,
		    addr, dsk->type);
		return (WT_ERROR);
	}

	/* Check the page record number. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		if (dsk->recno != 0)
			break;
		WT_VRFY_ERR(session, quiet,
		    "%s page at addr %" PRIu32 " has a record number of zero",
		    __wt_page_type_string(dsk->type), addr);
		return (WT_ERROR);
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if (dsk->recno == 0)
			break;
		WT_VRFY_ERR(session, quiet,
		    "%s page at addr %" PRIu32 " has a non-zero record number",
		    __wt_page_type_string(dsk->type), addr);
		return (WT_ERROR);
	}

	/*
	 * Ignore the LSN.
	 *
	 * Ignore the checksum -- it verified when we first read the page.
	 *
	 * Ignore the disk sizes.
	 */

	/* Unused bytes */
	if (dsk->unused[0] != '\0' ||
	    dsk->unused[1] != '\0' || dsk->unused[2] != '\0') {
		WT_VRFY_ERR(session, quiet,
		    "page at addr %" PRIu32 " has non-zero unused header "
		    "fields",
		    addr);
		return (WT_ERROR);
	}

	/* Verify the items on the page. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		return (__verify_dsk_col_int(session, dsk, addr, size, quiet));
	case WT_PAGE_COL_FIX:
		return (__verify_dsk_col_fix(session, dsk, addr, size, quiet));
	case WT_PAGE_COL_VAR:
		return (__verify_dsk_col_var(session, dsk, addr, size, quiet));
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		return (__verify_dsk_row(session, dsk, addr, size, quiet));
	case WT_PAGE_FREELIST:
	case WT_PAGE_OVFL:
		return (__verify_dsk_chunk(
		    session, dsk, addr, dsk->u.datalen, size, quiet));
	WT_ILLEGAL_FORMAT(session);
	}
	/* NOTREACHED */
}

/*
 * __verify_dsk_row --
 *	Walk a WT_PAGE_ROW_INT or WT_PAGE_ROW_LEAF disk page and verify it.
 */
static int
__verify_dsk_row(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size, int quiet)
{
	WT_BTREE *btree;
	WT_BUF *current, *last, *last_pfx, *last_ovfl;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
	off_t file_size;
	void *huffman;
	uint32_t cell_num, cell_type, i, prefix;
	uint8_t *end;
	int ret;
	int (*func)(WT_BTREE *, const WT_ITEM *, const WT_ITEM *);

	btree = session->btree;
	func = btree->btree_compare;
	huffman = btree->huffman_key;
	unpack = &_unpack;
	ret = 0;

	current = last_pfx = last_ovfl = NULL;
	WT_ERR(__wt_scr_alloc(session, 0, &current));
	WT_ERR(__wt_scr_alloc(session, 0, &last_pfx));
	WT_ERR(__wt_scr_alloc(session, 0, &last_ovfl));
	last = last_ovfl;

	file_size = btree->fh->file_size;
	end = (uint8_t *)dsk + size;

	last_cell_type = FIRST;
	cell_num = 0;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		++cell_num;

		/* Carefully unpack the cell. */
		if (__wt_cell_unpack_safe(cell, unpack, end) != 0) {
			ret = __err_cell_corrupted(
			    session, cell_num, addr, quiet);
			goto err;
		}

		/* Check the cell type. */
		cell_type = unpack->raw;
		switch (cell_type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
		case WT_CELL_KEY_SHORT:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
		case WT_CELL_VALUE_SHORT:
			break;
		case WT_CELL_OFF:
			if (dsk->type == WT_PAGE_ROW_INT)
				break;
			/* FALLTHROUGH */
		default:
			return (__err_cell_type(
			    session, cell_num, addr, unpack->type, dsk, quiet));
		}

		/* Collapse the short key/data types. */
		cell_type = unpack->type;

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
				WT_VRFY_ERR(session, quiet,
				    "cell %" PRIu32 " on page at addr %" PRIu32
				    " is the first of two adjacent keys",
				    cell_num - 1, addr);
				goto err;
			}
			last_cell_type = WAS_KEY;
			break;
		case WT_CELL_OFF:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
			switch (last_cell_type) {
			case FIRST:
				WT_VRFY_ERR(session, quiet,
				    "page at addr %" PRIu32 " begins with a "
				    "value", addr);
				goto err;
			case WAS_KEY:
				break;
			case WAS_VALUE:
				WT_VRFY_ERR(session, quiet,
				    "cell %" PRIu32 " on page at addr %" PRIu32
				    " is the first of two adjacent values",
				    cell_num - 1, addr);
				goto err;
			}
			last_cell_type = WAS_VALUE;
			break;
		}

		/* Check if any referenced item is entirely in the file.
		 */
		switch (cell_type) {
		case WT_CELL_KEY_OVFL:
		case WT_CELL_OFF:
		case WT_CELL_VALUE_OVFL:
			if (WT_ADDR_TO_OFF(btree,
			    unpack->off.addr) + unpack->off.size > file_size)
				goto eof;
			break;
		}

		/*
		 * Remaining checks are for key order and prefix compression.
		 * If this cell isn't a key, we're done, move to the next cell.
		 * If this cell is an overflow item, instantiate the key and
		 * compare it with the last key.   Otherwise, we have to deal
		 * with prefix compression.
		 */
		switch (cell_type) {
		case WT_CELL_KEY:
			break;
		case WT_CELL_KEY_OVFL:
			WT_ERR(__wt_cell_unpack_copy(session, unpack, current));
			goto key_compare;
		default:
			/* Not a key -- continue with the next cell. */
			continue;
		}

		/*
		 * Prefix compression checks.
		 *
		 * Confirm the first non-overflow key on a page has a zero
		 * prefix compression count.
		 */
		prefix = unpack->prefix;
		if (last_pfx->size == 0 && prefix != 0) {
			WT_VRFY_ERR(session, quiet,
			    "the %" PRIu32 " key on page at addr %" PRIu32
			    " is the first non-overflow key on the page and "
			    "has a non-zero prefix compression value",
			    cell_num, addr);
			goto err;
		}

		/* Confirm the prefix compression count is possible. */
		if (last->size != 0 && prefix > last->size) {
			WT_VRFY_ERR(session, quiet,
			    "key %" PRIu32 " on page at addr %" PRIu32
			    " has a prefix compression count of %" PRIu32
			    ", larger than the length of the previous key, %"
			    PRIu32,
			    cell_num, addr, prefix, last->size);
			goto err;
		}

		/*
		 * If Huffman decoding required, use the heavy-weight call to
		 * __wt_cell_unpack_copy() to build the key, up to the prefix.
		 * Else, we can do it faster internally because we don't have
		 * to shuffle memory around as much.
		 */
		if (huffman == NULL) {
			/*
			 * Get the cell's data/length and make sure we have
			 * enough buffer space.
			 */
			WT_ERR(__wt_buf_grow(
			    session, current, prefix + unpack->size));

			/* Copy the prefix then the data into place. */
			if (prefix != 0)
				memcpy((void *)
				    current->data, last->data, prefix);
			memcpy((uint8_t *)
			    current->data + prefix, unpack->data, unpack->size);
			current->size = prefix + unpack->size;
		} else {
			WT_ERR(__wt_cell_unpack_copy(session, unpack, current));

			/*
			 * If there's a prefix, make sure there's enough buffer
			 * space, then shift the decoded data past the prefix
			 * and copy the prefix into place.
			 */
			if (prefix != 0) {
				WT_ERR(__wt_buf_grow(
				    session, current, prefix + current->size));
				memmove((uint8_t *)current->data +
				    prefix, current->data, current->size);
				memcpy(
				    (void *)current->data, last->data, prefix);
				current->size += prefix;
			}
		}

key_compare:	/* Compare the current key against the last key. */
		if (last->size != 0 &&
		     func(btree, (WT_ITEM *)last, (WT_ITEM *)current) >= 0) {
			WT_VRFY_ERR(session, quiet,
			    "the %" PRIu32 " and %" PRIu32 " keys on page at "
			    "addr %" PRIu32 " are incorrectly sorted",
			    cell_num - 2, cell_num, addr);
			goto err;
		}

		/*
		 * Swap the buffers: last always references the last key entry,
		 * last_pfx and last_ovfl reference the last prefix-compressed
		 * and last overflow key entries.  Current gets pointed to the
		 * buffer we're not using this time around, which is where the
		 * next key goes.
		 */
		last = current;
		if (cell_type == WT_CELL_KEY) {
			current = last_pfx;
			last_pfx = last;
		} else {
			current = last_ovfl;
			last_ovfl = last;
		}
	}

	if (0) {
eof:		ret = __err_eof(session, cell_num, addr, quiet);
	}

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}
	__wt_scr_free(&current);
	__wt_scr_free(&last_pfx);
	__wt_scr_free(&last_ovfl);
	return (ret);
}

/*
 * __verify_dsk_col_int --
 *	Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__verify_dsk_col_int(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size, int quiet)
{
	WT_BTREE *btree;
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
			return (__err_eop(session, entry_num, addr, quiet));

		/* Check if the reference is past the end-of-file.
		 */
		if (WT_ADDR_TO_OFF(btree, off_record->addr) +
		    (off_t)off_record->size > btree->fh->file_size)
			return (__err_eof(session, entry_num, addr, quiet));
	}

	return (0);
}

/*
 * __verify_dsk_col_fix --
 *	Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__verify_dsk_col_fix(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size, int quiet)
{
	WT_BTREE *btree;
	uint32_t datalen;

	btree = session->btree;

	datalen = __bitstr_size(btree->bitcnt * dsk->u.entries);
	return (__verify_dsk_chunk(session, dsk, addr, datalen, size, quiet));
}

/*
 * __verify_dsk_col_var --
 *	Walk a WT_PAGE_COL_VAR disk page and verify it.
 */
static int
__verify_dsk_col_var(WT_SESSION_IMPL *session,
    WT_PAGE_DISK *dsk, uint32_t addr, uint32_t size, int quiet)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	off_t file_size;
	uint32_t cell_num, cell_type, i, last_size;
	int last_deleted, ret;
	const uint8_t *last_data;
	uint8_t *end;

	btree = session->btree;
	unpack = &_unpack;
	file_size = btree->fh->file_size;
	end = (uint8_t *)dsk + size;
	ret = 0;

	last_data = NULL;
	last_size = 0;
	last_deleted = 0;

	cell_num = 0;
	WT_CELL_FOREACH(dsk, cell, unpack, i) {
		++cell_num;

		/* Carefully unpack the cell. */
		if (__wt_cell_unpack_safe(cell, unpack, end) != 0)
			return (__err_cell_corrupted(
			    session, cell_num, addr, quiet));

		/* Check the cell type. */
		cell_type = unpack->raw;
		switch (cell_type) {
		case WT_CELL_DEL:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
		case WT_CELL_VALUE_SHORT:
			break;
		default:
			return (__err_cell_type(
			    session, cell_num, addr, unpack->raw, dsk, quiet));
		}

		/* Check if any referenced item is entirely in the file.
		 */
		if (cell_type == WT_CELL_VALUE_OVFL) {
			if (WT_ADDR_TO_OFF(btree,
			    unpack->off.addr) + unpack->off.size > file_size)
				return (__err_eof(
				    session, cell_num, addr, quiet));
		}

		/*
		 * Compare the last two items and see if reconciliation missed
		 * a chance for RLE encoding.  We don't have to care about data
		 * encoding or anything else, a byte comparison is enough.
		 */
		if (last_deleted == 1) {
			if (cell_type == WT_CELL_DEL)
				goto match_err;
		} else
			if ((cell_type == WT_CELL_VALUE ||
			    cell_type == WT_CELL_VALUE_SHORT) &&
			    last_data != NULL &&
			    last_size == unpack->size &&
			    memcmp(last_data, unpack->data, last_size) == 0) {
match_err:			ret = WT_ERROR;
				WT_VRFY_ERR(session, quiet,
				    "data entries %" PRIu32 " and %" PRIu32
				    " on page at addr %" PRIu32 " are "
				    "identical and should have been "
				    "run-length encoded",
				    cell_num - 1, cell_num, addr);
				break;
			}

		switch (cell_type) {
		case WT_CELL_DEL:
			last_deleted = 1;
			last_data = NULL;
			break;
		case WT_CELL_VALUE_OVFL:
			last_deleted = 0;
			last_data = NULL;
			break;
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_SHORT:
			last_deleted = 0;
			last_data = unpack->data;
			last_size = unpack->size;
			break;
		}
	}

	return (ret);
}

/*
 * __verify_dsk_chunk --
 *	Verify a Chunk O' Data on a Btree page.
 */
static int
__verify_dsk_chunk(WT_SESSION_IMPL *session, WT_PAGE_DISK *dsk,
    uint32_t addr, uint32_t data_len, uint32_t size, int quiet)
{
	uint8_t *p, *end;

	end = (uint8_t *)dsk + size;

	/*
	 * Fixed-length column-store, overflow and freelist pages are simple
	 * chunks of data.
	 */
	if (data_len == 0) {
		WT_VRFY_ERR(session, quiet,
		    "%s page at addr %" PRIu32 " has no data",
		    __wt_page_type_string(dsk->type), addr);
		return (WT_ERROR);
	}

	/* Verify the data doesn't overflow the end of the page. */
	p = (uint8_t *)WT_PAGE_DISK_BYTE(dsk);
	if (p + data_len > end) {
		WT_VRFY_ERR(session, quiet,
		    "data on page at addr %" PRIu32 " extends past the "
		    "end of the page", addr);
		return (WT_ERROR);
	}

	/* Any bytes after the data chunk should be nul bytes. */
	for (p += data_len; p < end; ++p)
		if (*p != '\0') {
			WT_VRFY_ERR(session, quiet,
			    "%s page at addr %"
			    PRIu32 " has non-zero trailing bytes",
			    __wt_page_type_string(dsk->type), addr);
			return (WT_ERROR);
		}

	return (0);
}

/*
 * __err_cell_corrupted --
 *	Generic corrupted cell, we couldn't read it.
 */
static int
__err_cell_corrupted(
    WT_SESSION_IMPL *session, uint32_t entry_num, uint32_t addr, int quiet)
{
	WT_VRFY_ERR(session, quiet,
	    "item %" PRIu32
	    " on page at addr %" PRIu32 " is a corrupted cell",
	    entry_num, addr);
	return (WT_ERROR);
}

/*
 * __err_cell_type --
 *	Generic illegal cell type for a particular page type error.
 */
static int
__err_cell_type(WT_SESSION_IMPL *session, uint32_t entry_num,
    uint32_t addr, uint8_t cell_type, WT_PAGE_DISK *dsk, int quiet)
{
	WT_VRFY_ERR(session, quiet,
	    "illegal cell and page type combination cell %" PRIu32
	    " on page at addr %" PRIu32 " is a %s cell on a %s page",
	    entry_num, addr,
	    __wt_cell_type_string(cell_type), __wt_page_type_string(dsk->type));
	return (WT_ERROR);
}

/*
 * __err_eop --
 *	Generic item extends past the end-of-page error.
 */
static int
__err_eop(WT_SESSION_IMPL *session,
    uint32_t entry_num, uint32_t addr, int quiet)
{
	WT_VRFY_ERR(session, quiet,
	    "item %" PRIu32
	    " on page at addr %" PRIu32 " extends past the end of the page",
	    entry_num, addr);
	return (WT_ERROR);
}

/*
 * __err_eof --
 *	Generic item references non-existent file pages error.
 */
static int
__err_eof(WT_SESSION_IMPL *session,
    uint32_t entry_num, uint32_t addr, int quiet)
{
	WT_VRFY_ERR(session, quiet,
	    "off-page item %" PRIu32
	    " on page at addr %" PRIu32 " references non-existent file pages",
	    entry_num, addr);
	return (WT_ERROR);
}
