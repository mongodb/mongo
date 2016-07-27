/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __err_cell_corrupt(WT_SESSION_IMPL *, uint32_t, const char *);
static int __err_cell_corrupt_or_eof(WT_SESSION_IMPL *, uint32_t, const char *);
static int __err_cell_type(
	WT_SESSION_IMPL *, uint32_t, const char *, uint8_t, uint8_t);
static int __verify_dsk_chunk(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, uint32_t);
static int __verify_dsk_col_fix(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *);
static int __verify_dsk_col_int(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *);
static int __verify_dsk_col_var(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *);
static int __verify_dsk_memsize(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *, WT_CELL *);
static int __verify_dsk_row(
	WT_SESSION_IMPL *, const char *, const WT_PAGE_HEADER *);

#define	WT_ERR_VRFY(session, ...) do {					\
	if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)))		\
		__wt_errx(session, __VA_ARGS__);			\
	goto err;							\
} while (0)

#define	WT_RET_VRFY(session, ...) do {					\
	if (!(F_ISSET(session, WT_SESSION_QUIET_CORRUPT_FILE)))		\
		__wt_errx(session, __VA_ARGS__);			\
	return (WT_ERROR);						\
} while (0)

/*
 * __wt_verify_dsk_image --
 *	Verify a single block as read from disk.
 */
int
__wt_verify_dsk_image(WT_SESSION_IMPL *session,
    const char *tag, const WT_PAGE_HEADER *dsk, size_t size, bool empty_page_ok)
{
	const uint8_t *p, *end;
	u_int i;
	uint8_t flags;

	/* Check the page type. */
	switch (dsk->type) {
	case WT_PAGE_BLOCK_MANAGER:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		break;
	case WT_PAGE_INVALID:
	default:
		WT_RET_VRFY(session,
		    "page at %s has an invalid type of %" PRIu32,
		    tag, dsk->type);
	}

	/* Check the page record number. */
	switch (dsk->type) {
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		if (dsk->recno != WT_RECNO_OOB)
			break;
		WT_RET_VRFY(session,
		    "%s page at %s has an invalid record number of %d",
		    __wt_page_type_string(dsk->type), tag, WT_RECNO_OOB);
	case WT_PAGE_BLOCK_MANAGER:
	case WT_PAGE_OVFL:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if (dsk->recno == WT_RECNO_OOB)
			break;
		WT_RET_VRFY(session,
		    "%s page at %s has a record number, which is illegal for "
		    "this page type",
		    __wt_page_type_string(dsk->type), tag);
	}

	/* Check the page flags. */
	flags = dsk->flags;
	if (LF_ISSET(WT_PAGE_COMPRESSED))
		LF_CLR(WT_PAGE_COMPRESSED);
	if (dsk->type == WT_PAGE_ROW_LEAF) {
		if (LF_ISSET(WT_PAGE_EMPTY_V_ALL) &&
		    LF_ISSET(WT_PAGE_EMPTY_V_NONE))
			WT_RET_VRFY(session,
			    "page at %s has invalid flags combination: 0x%"
			    PRIx8,
			    tag, dsk->flags);
		if (LF_ISSET(WT_PAGE_EMPTY_V_ALL))
			LF_CLR(WT_PAGE_EMPTY_V_ALL);
		if (LF_ISSET(WT_PAGE_EMPTY_V_NONE))
			LF_CLR(WT_PAGE_EMPTY_V_NONE);
	}
	if (LF_ISSET(WT_PAGE_ENCRYPTED))
		LF_CLR(WT_PAGE_ENCRYPTED);
	if (LF_ISSET(WT_PAGE_LAS_UPDATE))
		LF_CLR(WT_PAGE_LAS_UPDATE);
	if (flags != 0)
		WT_RET_VRFY(session,
		    "page at %s has invalid flags set: 0x%" PRIx8,
		    tag, flags);

	/* Unused bytes */
	for (p = dsk->unused, i = sizeof(dsk->unused); i > 0; --i)
		if (*p != '\0')
			WT_RET_VRFY(session,
			    "page at %s has non-zero unused page header bytes",
			    tag);

	/*
	 * Any bytes after the data chunk should be nul bytes; ignore if the
	 * size is 0, that allows easy checking of disk images where we don't
	 * have the size.
	 */
	if (size != 0) {
		p = (uint8_t *)dsk + dsk->mem_size;
		end = (uint8_t *)dsk + size;
		for (; p < end; ++p)
			if (*p != '\0')
				WT_RET_VRFY(session,
				    "%s page at %s has non-zero trailing bytes",
				    __wt_page_type_string(dsk->type), tag);
	}

	/* Check for empty pages, then verify the items on the page. */
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_VAR:
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		if (!empty_page_ok && dsk->u.entries == 0)
			WT_RET_VRFY(session, "%s page at %s has no entries",
			    __wt_page_type_string(dsk->type), tag);
		break;
	case WT_PAGE_BLOCK_MANAGER:
	case WT_PAGE_OVFL:
		if (dsk->u.datalen == 0)
			WT_RET_VRFY(session, "%s page at %s has no data",
			    __wt_page_type_string(dsk->type), tag);
		break;
	}
	switch (dsk->type) {
	case WT_PAGE_COL_INT:
		return (__verify_dsk_col_int(session, tag, dsk));
	case WT_PAGE_COL_FIX:
		return (__verify_dsk_col_fix(session, tag, dsk));
	case WT_PAGE_COL_VAR:
		return (__verify_dsk_col_var(session, tag, dsk));
	case WT_PAGE_ROW_INT:
	case WT_PAGE_ROW_LEAF:
		return (__verify_dsk_row(session, tag, dsk));
	case WT_PAGE_BLOCK_MANAGER:
	case WT_PAGE_OVFL:
		return (__verify_dsk_chunk(session, tag, dsk, dsk->u.datalen));
	WT_ILLEGAL_VALUE(session);
	}
	/* NOTREACHED */
}

/*
 * __wt_verify_dsk --
 *	Verify a single Btree page as read from disk.
 */
int
__wt_verify_dsk(WT_SESSION_IMPL *session, const char *tag, WT_ITEM *buf)
{
	return (
	    __wt_verify_dsk_image(session, tag, buf->data, buf->size, false));
}

/*
 * __verify_dsk_row --
 *	Walk a WT_PAGE_ROW_INT or WT_PAGE_ROW_LEAF disk page and verify it.
 */
static int
__verify_dsk_row(
    WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(current);
	WT_DECL_ITEM(last_ovfl);
	WT_DECL_ITEM(last_pfx);
	WT_DECL_ITEM(tmp1);
	WT_DECL_ITEM(tmp2);
	WT_DECL_RET;
	WT_ITEM *last;
	enum { FIRST, WAS_KEY, WAS_VALUE } last_cell_type;
	void *huffman;
	uint32_t cell_num, cell_type, i, key_cnt, prefix;
	uint8_t *end;
	int cmp;

	btree = S2BT(session);
	bm = btree->bm;
	unpack = &_unpack;
	huffman = dsk->type == WT_PAGE_ROW_INT ? NULL : btree->huffman_key;

	WT_ERR(__wt_scr_alloc(session, 0, &current));
	WT_ERR(__wt_scr_alloc(session, 0, &last_pfx));
	WT_ERR(__wt_scr_alloc(session, 0, &last_ovfl));
	WT_ERR(__wt_scr_alloc(session, 0, &tmp1));
	WT_ERR(__wt_scr_alloc(session, 0, &tmp2));
	last = last_ovfl;

	end = (uint8_t *)dsk + dsk->mem_size;

	last_cell_type = FIRST;
	cell_num = 0;
	key_cnt = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		++cell_num;

		/* Carefully unpack the cell. */
		if (__wt_cell_unpack_safe(cell, unpack, dsk, end) != 0) {
			ret = __err_cell_corrupt(session, cell_num, tag);
			goto err;
		}

		/* Check the raw and collapsed cell types. */
		WT_ERR(__err_cell_type(
		    session, cell_num, tag, unpack->raw, dsk->type));
		WT_ERR(__err_cell_type(
		    session, cell_num, tag, unpack->type, dsk->type));
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
			++key_cnt;
			switch (last_cell_type) {
			case FIRST:
			case WAS_VALUE:
				break;
			case WAS_KEY:
				if (dsk->type == WT_PAGE_ROW_LEAF)
					break;
				WT_ERR_VRFY(session,
				    "cell %" PRIu32 " on page at %s is the "
				    "first of two adjacent keys",
				    cell_num - 1, tag);
			}
			last_cell_type = WAS_KEY;
			break;
		case WT_CELL_ADDR_DEL:
		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
			switch (last_cell_type) {
			case FIRST:
				WT_ERR_VRFY(session,
				    "page at %s begins with a value", tag);
			case WAS_KEY:
				break;
			case WAS_VALUE:
				WT_ERR_VRFY(session,
				    "cell %" PRIu32 " on page at %s is the "
				    "first of two adjacent values",
				    cell_num - 1, tag);
			}
			last_cell_type = WAS_VALUE;
			break;
		}

		/* Check if any referenced item has an invalid address. */
		switch (cell_type) {
		case WT_CELL_ADDR_DEL:
		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
		case WT_CELL_KEY_OVFL:
		case WT_CELL_VALUE_OVFL:
			if ((ret = bm->addr_invalid(
			    bm, session, unpack->data, unpack->size)) == EINVAL)
				ret = __err_cell_corrupt_or_eof(
				    session, cell_num, tag);
			WT_ERR(ret);
			break;
		}

		/*
		 * Remaining checks are for key order and prefix compression.
		 * If this cell isn't a key, we're done, move to the next cell.
		 * If this cell is an overflow item, instantiate the key and
		 * compare it with the last key. Otherwise, we have to deal with
		 * prefix compression.
		 */
		switch (cell_type) {
		case WT_CELL_KEY:
			break;
		case WT_CELL_KEY_OVFL:
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, dsk->type, unpack, current));
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
		if (last_pfx->size == 0 && prefix != 0)
			WT_ERR_VRFY(session,
			    "the %" PRIu32 " key on page at %s is the first "
			    "non-overflow key on the page and has a non-zero "
			    "prefix compression value",
			    cell_num, tag);

		/* Confirm the prefix compression count is possible. */
		if (cell_num > 1 && prefix > last->size)
			WT_ERR_VRFY(session,
			    "key %" PRIu32 " on page at %s has a prefix "
			    "compression count of %" PRIu32 ", larger than "
			    "the length of the previous key, %" WT_SIZET_FMT,
			    cell_num, tag, prefix, last->size);

		/*
		 * If Huffman decoding required, unpack the cell to build the
		 * key, then resolve the prefix.  Else, we can do it faster
		 * internally because we don't have to shuffle memory around as
		 * much.
		 */
		if (huffman != NULL) {
			WT_ERR(__wt_dsk_cell_data_ref(
			    session, dsk->type, unpack, current));

			/*
			 * If there's a prefix, make sure there's enough buffer
			 * space, then shift the decoded data past the prefix
			 * and copy the prefix into place.  Take care with the
			 * pointers: current->data may be pointing inside the
			 * buffer.
			 */
			if (prefix != 0) {
				WT_ERR(__wt_buf_grow(
				    session, current, prefix + current->size));
				memmove((uint8_t *)current->mem + prefix,
				    current->data, current->size);
				memcpy(current->mem, last->data, prefix);
				current->data = current->mem;
				current->size += prefix;
			}
		} else {
			/*
			 * Get the cell's data/length and make sure we have
			 * enough buffer space.
			 */
			WT_ERR(__wt_buf_init(
			    session, current, prefix + unpack->size));

			/* Copy the prefix then the data into place. */
			if (prefix != 0)
				memcpy(current->mem, last->data, prefix);
			memcpy((uint8_t *)current->mem + prefix, unpack->data,
			    unpack->size);
			current->size = prefix + unpack->size;
		}

key_compare:	/*
		 * Compare the current key against the last key.
		 *
		 * Be careful about the 0th key on internal pages: we only store
		 * the first byte and custom collators may not be able to handle
		 * truncated keys.
		 */
		if ((dsk->type == WT_PAGE_ROW_INT && cell_num > 3) ||
		    (dsk->type != WT_PAGE_ROW_INT && cell_num > 1)) {
			WT_ERR(__wt_compare(
			    session, btree->collator, last, current, &cmp));
			if (cmp >= 0)
				WT_ERR_VRFY(session,
				    "the %" PRIu32 " and %" PRIu32 " keys on "
				    "page at %s are incorrectly sorted: %s, %s",
				    cell_num - 2, cell_num, tag,
				    __wt_buf_set_printable(session,
				    last->data, last->size, tmp1),
				    __wt_buf_set_printable(session,
				    current->data, current->size, tmp2));
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
		WT_ASSERT(session, last != current);
	}
	WT_ERR(__verify_dsk_memsize(session, tag, dsk, cell));

	/*
	 * On row-store internal pages, and on row-store leaf pages, where the
	 * "no empty values" flag is set, the key count should be equal to half
	 * the number of physical entries.  On row-store leaf pages where the
	 * "all empty values" flag is set, the key count should be equal to the
	 * number of physical entries.
	 */
	if (dsk->type == WT_PAGE_ROW_INT && key_cnt * 2 != dsk->u.entries)
		WT_ERR_VRFY(session,
		    "%s page at %s has a key count of %" PRIu32 " and a "
		    "physical entry count of %" PRIu32,
		    __wt_page_type_string(dsk->type),
		    tag, key_cnt, dsk->u.entries);
	if (dsk->type == WT_PAGE_ROW_LEAF &&
	    F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL) &&
	    key_cnt != dsk->u.entries)
		WT_ERR_VRFY(session,
		    "%s page at %s with the 'all empty values' flag set has a "
		    "key count of %" PRIu32 " and a physical entry count of %"
		    PRIu32,
		    __wt_page_type_string(dsk->type),
		    tag, key_cnt, dsk->u.entries);
	if (dsk->type == WT_PAGE_ROW_LEAF &&
	    F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE) &&
	    key_cnt * 2 != dsk->u.entries)
		WT_ERR_VRFY(session,
		    "%s page at %s with the 'no empty values' flag set has a "
		    "key count of %" PRIu32 " and a physical entry count of %"
		    PRIu32,
		    __wt_page_type_string(dsk->type),
		    tag, key_cnt, dsk->u.entries);

	if (0) {
err:		if (ret == 0)
			ret = WT_ERROR;
	}
	__wt_scr_free(session, &current);
	__wt_scr_free(session, &last_pfx);
	__wt_scr_free(session, &last_ovfl);
	__wt_scr_free(session, &tmp1);
	__wt_scr_free(session, &tmp2);
	return (ret);
}

/*
 * __verify_dsk_col_int --
 *	Walk a WT_PAGE_COL_INT disk page and verify it.
 */
static int
__verify_dsk_col_int(
    WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_RET;
	uint32_t cell_num, i;
	uint8_t *end;

	btree = S2BT(session);
	bm = btree->bm;
	unpack = &_unpack;
	end = (uint8_t *)dsk + dsk->mem_size;

	cell_num = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		++cell_num;

		/* Carefully unpack the cell. */
		if (__wt_cell_unpack_safe(cell, unpack, dsk, end) != 0)
			return (__err_cell_corrupt(session, cell_num, tag));

		/* Check the raw and collapsed cell types. */
		WT_RET(__err_cell_type(
		    session, cell_num, tag, unpack->raw, dsk->type));
		WT_RET(__err_cell_type(
		    session, cell_num, tag, unpack->type, dsk->type));

		/* Check if any referenced item is entirely in the file. */
		ret = bm->addr_invalid(bm, session, unpack->data, unpack->size);
		WT_RET_ERROR_OK(ret, EINVAL);
		if (ret == EINVAL)
			return (
			    __err_cell_corrupt_or_eof(session, cell_num, tag));
	}
	WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

	return (0);
}

/*
 * __verify_dsk_col_fix --
 *	Walk a WT_PAGE_COL_FIX disk page and verify it.
 */
static int
__verify_dsk_col_fix(
    WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk)
{
	WT_BTREE *btree;
	uint32_t datalen;

	btree = S2BT(session);

	datalen = __bitstr_size(btree->bitcnt * dsk->u.entries);
	return (__verify_dsk_chunk(session, tag, dsk, datalen));
}

/*
 * __verify_dsk_col_var --
 *	Walk a WT_PAGE_COL_VAR disk page and verify it.
 */
static int
__verify_dsk_col_var(
    WT_SESSION_IMPL *session, const char *tag, const WT_PAGE_HEADER *dsk)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_RET;
	size_t last_size;
	uint32_t cell_num, cell_type, i;
	bool last_deleted;
	const uint8_t *last_data;
	uint8_t *end;

	btree = S2BT(session);
	bm = btree->bm;
	unpack = &_unpack;
	end = (uint8_t *)dsk + dsk->mem_size;

	last_data = NULL;
	last_size = 0;
	last_deleted = false;

	cell_num = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		++cell_num;

		/* Carefully unpack the cell. */
		if (__wt_cell_unpack_safe(cell, unpack, dsk, end) != 0)
			return (__err_cell_corrupt(session, cell_num, tag));

		/* Check the raw and collapsed cell types. */
		WT_RET(__err_cell_type(
		    session, cell_num, tag, unpack->raw, dsk->type));
		WT_RET(__err_cell_type(
		    session, cell_num, tag, unpack->type, dsk->type));
		cell_type = unpack->type;

		/* Check if any referenced item is entirely in the file. */
		if (cell_type == WT_CELL_VALUE_OVFL) {
			ret = bm->addr_invalid(
			    bm, session, unpack->data, unpack->size);
			WT_RET_ERROR_OK(ret, EINVAL);
			if (ret == EINVAL)
				return (__err_cell_corrupt_or_eof(
				    session, cell_num, tag));
		}

		/*
		 * Compare the last two items and see if reconciliation missed
		 * a chance for RLE encoding.  We don't have to care about data
		 * encoding or anything else, a byte comparison is enough.
		 */
		if (last_deleted) {
			if (cell_type == WT_CELL_DEL)
				goto match_err;
		} else
			if (cell_type == WT_CELL_VALUE &&
			    last_data != NULL &&
			    last_size == unpack->size &&
			    memcmp(last_data, unpack->data, last_size) == 0)
match_err:			WT_RET_VRFY(session,
				    "data entries %" PRIu32 " and %" PRIu32
				    " on page at %s are identical and should "
				    "have been run-length encoded",
				    cell_num - 1, cell_num, tag);

		switch (cell_type) {
		case WT_CELL_DEL:
			last_deleted = true;
			last_data = NULL;
			break;
		case WT_CELL_VALUE_OVFL:
			last_deleted = false;
			last_data = NULL;
			break;
		case WT_CELL_VALUE:
			last_deleted = false;
			last_data = unpack->data;
			last_size = unpack->size;
			break;
		}
	}
	WT_RET(__verify_dsk_memsize(session, tag, dsk, cell));

	return (0);
}

/*
 * __verify_dsk_memsize --
 *	Verify the last cell on the page matches the page's memory size.
 */
static int
__verify_dsk_memsize(WT_SESSION_IMPL *session,
    const char *tag, const WT_PAGE_HEADER *dsk, WT_CELL *cell)
{
	size_t len;

	/*
	 * We use the fact that cells exactly fill a page to detect the case of
	 * a row-store leaf page where the last cell is a key (that is, there's
	 * no subsequent value cell).  Check for any page type containing cells.
	 */
	len = WT_PTRDIFF((uint8_t *)dsk + dsk->mem_size, cell);
	if (len == 0)
		return (0);
	WT_RET_VRFY(session,
	    "%s page at %s has %" WT_SIZET_FMT " unexpected bytes of data "
	    "after the last cell",
	    __wt_page_type_string(dsk->type), tag, len);
}

/*
 * __verify_dsk_chunk --
 *	Verify a Chunk O' Data on a Btree page.
 */
static int
__verify_dsk_chunk(WT_SESSION_IMPL *session,
    const char *tag, const WT_PAGE_HEADER *dsk, uint32_t datalen)
{
	WT_BTREE *btree;
	uint8_t *p, *end;

	btree = S2BT(session);
	end = (uint8_t *)dsk + dsk->mem_size;

	/*
	 * Fixed-length column-store and overflow pages are simple chunks of
	 * data. Verify the data doesn't overflow the end of the page.
	 */
	p = WT_PAGE_HEADER_BYTE(btree, dsk);
	if (p + datalen > end)
		WT_RET_VRFY(session,
		    "data on page at %s extends past the end of the page",
		    tag);

	/* Any bytes after the data chunk should be nul bytes. */
	for (p += datalen; p < end; ++p)
		if (*p != '\0')
			WT_RET_VRFY(session,
			    "%s page at %s has non-zero trailing bytes",
			    __wt_page_type_string(dsk->type), tag);

	return (0);
}

/*
 * __err_cell_corrupt --
 *	Generic corrupted cell, we couldn't read it.
 */
static int
__err_cell_corrupt(
    WT_SESSION_IMPL *session, uint32_t entry_num, const char *tag)
{
	WT_RET_VRFY(session,
	    "item %" PRIu32 " on page at %s is a corrupted cell",
	    entry_num, tag);
}

/*
 * __err_cell_corrupt_or_eof --
 *	Generic corrupted cell or item references non-existent file pages error.
 */
static int
__err_cell_corrupt_or_eof(
    WT_SESSION_IMPL *session, uint32_t entry_num, const char *tag)
{
	WT_RET_VRFY(session,
	    "item %" PRIu32 " on page at %s is a corrupted cell or references "
	    "non-existent file pages",
	    entry_num, tag);
}

/*
 * __err_cell_type --
 *	Generic illegal cell type for a particular page type error.
 */
static int
__err_cell_type(WT_SESSION_IMPL *session,
    uint32_t entry_num, const char *tag, uint8_t cell_type, uint8_t dsk_type)
{
	switch (cell_type) {
	case WT_CELL_ADDR_DEL:
	case WT_CELL_ADDR_INT:
	case WT_CELL_ADDR_LEAF:
	case WT_CELL_ADDR_LEAF_NO:
		if (dsk_type == WT_PAGE_COL_INT ||
		    dsk_type == WT_PAGE_ROW_INT)
			return (0);
		break;
	case WT_CELL_DEL:
		if (dsk_type == WT_PAGE_COL_VAR)
			return (0);
		break;
	case WT_CELL_KEY:
	case WT_CELL_KEY_OVFL:
	case WT_CELL_KEY_SHORT:
		if (dsk_type == WT_PAGE_ROW_INT ||
		    dsk_type == WT_PAGE_ROW_LEAF)
			return (0);
		break;
	case WT_CELL_KEY_PFX:
	case WT_CELL_KEY_SHORT_PFX:
		if (dsk_type == WT_PAGE_ROW_LEAF)
			return (0);
		break;
	case WT_CELL_KEY_OVFL_RM:
	case WT_CELL_VALUE_OVFL_RM:
		/*
		 * Removed overflow cells are in-memory only, it's an error to
		 * ever see one on a disk page.
		 */
		break;
	case WT_CELL_VALUE:
	case WT_CELL_VALUE_COPY:
	case WT_CELL_VALUE_OVFL:
	case WT_CELL_VALUE_SHORT:
		if (dsk_type == WT_PAGE_COL_VAR ||
		    dsk_type == WT_PAGE_ROW_LEAF)
			return (0);
		break;
	default:
		break;
	}

	WT_RET_VRFY(session,
	    "illegal cell and page type combination: cell %" PRIu32
	    " on page at %s is a %s cell on a %s page",
	    entry_num, tag,
	    __wt_cell_type_string(cell_type), __wt_page_type_string(dsk_type));
}
