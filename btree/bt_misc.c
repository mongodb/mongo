/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

/*
 * __wt_bt_build_verify --
 *	Verify the Btree build itself.
 */
int
__wt_bt_build_verify(void)
{
	/*
	 * The compiler had better not have padded our structures -- make
	 * sure the page header structure is exactly what we expect.
	 */
	if (sizeof(WT_PAGE_HDR) != WT_HDR_SIZE ||
	    sizeof(WT_PAGE_DESC) != WT_DESC_SIZE) {
		fprintf(stderr,
		    "WiredTiger build failed, the header structures are not "
		    "the correct size");
		return (WT_ERROR);
	}
	if (WT_ALIGN(sizeof(WT_PAGE_HDR), sizeof(u_int32_t)) != WT_HDR_SIZE) {
		fprintf(stderr,
		    "Build verification failed, the WT_PAGE_HDR structure"
		    " isn't aligned correctly");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_bt_data_copy_to_dbt --
 *	Copy a data/length pair into allocated memory in a DBT.
 */
int
__wt_bt_data_copy_to_dbt(DB *db, u_int8_t *data, size_t len, DBT *copy)
{
	int ret;

	if (copy->data == NULL || copy->alloc_size < len) {
		if ((ret = __wt_realloc(db->ienv, len, &copy->data)) != 0)
			return (ret);
		copy->alloc_size = len;
	}
	memcpy(copy->data, data, copy->size = len);

	return (0);
}

/*
 * __wt_bt_first_offp_addr --
 *	In a couple of places in the code, we're trying to walk down the
 *	internal pages from the root, and we need to get the address off
 *	the first WT_ITEM_OFFP on the page.
 */
void
__wt_bt_first_offp_addr(WT_PAGE *page, u_int32_t *addrp)
{
	WT_ITEM *item;
	WT_ITEM_OFFP *offp;

	item = (WT_ITEM *)WT_PAGE_BYTE(page);
	item = WT_ITEM_NEXT(item);
	offp = (WT_ITEM_OFFP *)WT_ITEM_BYTE(item);
	*addrp = offp->addr;
}

/*
 * __wt_bt_hdr_type --
 *	Return a string representing the page type.
 */
const char *
__wt_bt_hdr_type(u_int32_t type)
{
	switch (type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_INT:
		return ("primary internal");
	case WT_PAGE_LEAF:
		return ("primary leaf");
	case WT_PAGE_DUP_INT:
		return ("duplicate internal");
	case WT_PAGE_DUP_LEAF:
		return ("duplicate leaf");
	default:
		break;
	}
	return ("unknown");
}

/*
 * __wt_bt_item_type --
 *	Return a string representing the item type.
 */
const char *
__wt_bt_item_type(u_int32_t type)
{
	switch (type) {
	case WT_ITEM_KEY:
		return ("key");
	case WT_ITEM_DATA:
		return ("data");
	case WT_ITEM_DUP:
		return ("duplicate");
	case WT_ITEM_KEY_OVFL:
		return ("key-overflow");
	case WT_ITEM_DATA_OVFL:
		return ("data-overflow");
	case WT_ITEM_DUP_OVFL:
		return ("duplicate-overflow");
	case WT_ITEM_OFFPAGE:
		return ("offpage");
	default:
		break;
	}
	return ("unknown");
}
