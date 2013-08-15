/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_page_type_string --
 *	Return a string representing the page type.
 */
const char *
__wt_page_type_string(u_int type)
{
	switch (type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_BLOCK_MANAGER:
		return ("block manager");
	case WT_PAGE_COL_FIX:
		return ("column-store fixed-length leaf");
	case WT_PAGE_COL_INT:
		return ("column-store internal");
	case WT_PAGE_COL_VAR:
		return ("column-store variable-length leaf");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_ROW_INT:
		return ("row-store internal");
	case WT_PAGE_ROW_LEAF:
		return ("row-store leaf");
	default:
		return ("unknown");
	}
	/* NOTREACHED */
}

/*
 * __wt_cell_type_string --
 *	Return a string representing the cell type.
 */
const char *
__wt_cell_type_string(uint8_t type)
{
	switch (type) {
	case WT_CELL_ADDR:
		return ("addr");
	case WT_CELL_ADDR_DEL:
		return ("addr/del");
	case WT_CELL_ADDR_LNO:
		return ("addr/lno");
	case WT_CELL_DEL:
		return ("deleted");
	case WT_CELL_KEY:
		return ("key");
	case WT_CELL_KEY_PFX:
		return ("key/pfx");
	case WT_CELL_KEY_OVFL:
		return ("key/ovfl");
	case WT_CELL_KEY_SHORT:
		return ("key/short");
	case WT_CELL_KEY_SHORT_PFX:
		return ("key/short,pfx");
	case WT_CELL_VALUE:
		return ("value");
	case WT_CELL_VALUE_COPY:
		return ("value/copy");
	case WT_CELL_VALUE_OVFL:
		return ("value/ovfl");
	case WT_CELL_VALUE_OVFL_RM:
		return ("value/ovfl,rm");
	case WT_CELL_VALUE_SHORT:
		return ("value/short");
	default:
		return ("unknown");
	}
	/* NOTREACHED */
}

/*
 * __wt_page_addr_string --
 *	Figure out a page's "address" and load a buffer with a printable,
 * nul-terminated representation of that address.
 */
const char *
__wt_page_addr_string(WT_SESSION_IMPL *session, WT_ITEM *buf, WT_PAGE *page)
{
	uint32_t size;
	const uint8_t *addr;

	if (WT_PAGE_IS_ROOT(page)) {
		buf->data = "[Root]";
		buf->size = WT_STORE_SIZE(strlen("[Root]"));
		return (buf->data);
	}

	__wt_get_addr(page->parent, page->ref, &addr, &size);

	return (__wt_addr_string(session, buf, addr, size));
}

/*
 * __wt_addr_string --
 *	Load a buffer with a printable, nul-terminated representation of an
 * address.
 */
const char *
__wt_addr_string(
    WT_SESSION_IMPL *session, WT_ITEM *buf, const uint8_t *addr, uint32_t size)
{
	WT_BM *bm;

	bm = S2BT(session)->bm;

	if (addr == NULL) {
		buf->data = "[NoAddr]";
		buf->size = WT_STORE_SIZE(strlen("[NoAddr]"));
	} else if (bm->addr_string(bm, session, buf, addr, size) != 0) {
		buf->data = "[Error]";
		buf->size = WT_STORE_SIZE(strlen("[Error]"));
	}
	return (buf->data);
}
