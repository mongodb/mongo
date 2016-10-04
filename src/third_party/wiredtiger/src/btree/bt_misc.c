/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	case WT_CELL_ADDR_DEL:
		return ("addr/del");
	case WT_CELL_ADDR_INT:
		return ("addr/int");
	case WT_CELL_ADDR_LEAF:
		return ("addr/leaf");
	case WT_CELL_ADDR_LEAF_NO:
		return ("addr/leaf-no");
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
	case WT_CELL_KEY_OVFL_RM:
		return ("key/ovfl,rm");
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
__wt_page_addr_string(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *buf)
{
	size_t addr_size;
	const uint8_t *addr;

	if (__wt_ref_is_root(ref)) {
		buf->data = "[Root]";
		buf->size = strlen("[Root]");
		return (buf->data);
	}

	__wt_ref_info(ref, &addr, &addr_size, NULL);
	return (__wt_addr_string(session, addr, addr_size, buf));
}

/*
 * __wt_addr_string --
 *	Load a buffer with a printable, nul-terminated representation of an
 * address.
 */
const char *
__wt_addr_string(WT_SESSION_IMPL *session,
    const uint8_t *addr, size_t addr_size, WT_ITEM *buf)
{
	WT_BM *bm;
	WT_BTREE *btree;

	btree = S2BT_SAFE(session);

	if (addr == NULL) {
		buf->data = "[NoAddr]";
		buf->size = strlen("[NoAddr]");
	} else if (btree == NULL || (bm = btree->bm) == NULL ||
	    bm->addr_string(bm, session, buf, addr, addr_size) != 0) {
		buf->data = "[Error]";
		buf->size = strlen("[Error]");
	}
	return (buf->data);
}
