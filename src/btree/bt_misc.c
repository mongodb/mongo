/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_set_ff_and_sa_from_offset --
 *	Set first-free and space-available values from an address positioned
 *	one past the last used byte on the page.
 */
inline void
__wt_set_ff_and_sa_from_offset(WT_PAGE *page,
    void *p, uint8_t **first_freep, uint32_t *space_availp)
{
	*first_freep = (uint8_t *)p;
	*space_availp =
	    page->size - (uint32_t)((uint8_t *)p - (uint8_t *)page->dsk);
}

/*
 * __wt_page_write_gen_check --
 *	Confirm the page's write generation number is correct.
 */
inline int
__wt_page_write_gen_check(WT_PAGE *page, uint32_t write_gen)
{
	return (page->write_gen == write_gen ? 0 : WT_RESTART);
}

/*
 * __wt_key_item_next --
 *	Helper function for the WT_INDX_AND_KEY_FOREACH macro, move to the
 *	next key WT_ITEM on the page.
 */
inline WT_ITEM *
__wt_key_item_next(WT_PAGE *page, WT_ROW *rip, WT_ITEM *key_item)
{
	/* If it's a duplicate entry, we're pointing to the appropriate key. */
	if (WT_ROW_INDX_IS_DUPLICATE(page, rip))
		return (key_item);

	/* Move to the next key WT_ITEM on the page. */
	do {
		key_item = WT_ITEM_NEXT(key_item);
	} while (
	    WT_ITEM_TYPE(key_item) != WT_ITEM_KEY &&
	    WT_ITEM_TYPE(key_item) != WT_ITEM_KEY_OVFL);

	return (key_item);
}

/*
 * __wt_page_type_string --
 *	Return a string representing the page type.
 */
const char *
__wt_page_type_string(WT_PAGE_DISK *dsk)
{
	switch (dsk->type) {
	case WT_PAGE_INVALID:
		return ("invalid");
	case WT_PAGE_COL_FIX:
		return ("column-store fixed-length leaf");
	case WT_PAGE_COL_INT:
		return ("column-store internal");
	case WT_PAGE_COL_RLE:
		return ("column-store fixed-length run-length encoded leaf");
	case WT_PAGE_COL_VAR:
		return ("column-store variable-length leaf");
	case WT_PAGE_DUP_INT:
		return ("duplicate tree internal");
	case WT_PAGE_DUP_LEAF:
		return ("duplicate tree leaf");
	case WT_PAGE_OVFL:
		return ("overflow");
	case WT_PAGE_ROW_INT:
		return ("row-store internal");
	case WT_PAGE_ROW_LEAF:
		return ("row-store leaf");
	case WT_PAGE_FREELIST:
		return ("freelist");
	default:
		break;
	}
	return ("unknown");
}

/*
 * __wt_item_type_string --
 *	Return a string representing the item type.
 */
const char *
__wt_item_type_string(WT_ITEM *item)
{
	switch (WT_ITEM_TYPE(item)) {
	case WT_ITEM_DATA:
		return ("data");
	case WT_ITEM_DATA_DUP:
		return ("data-duplicate");
	case WT_ITEM_DATA_DUP_OVFL:
		return ("data-duplicate-overflow");
	case WT_ITEM_DATA_OVFL:
		return ("data-overflow");
	case WT_ITEM_DEL:
		return ("deleted");
	case WT_ITEM_KEY:
		return ("key");
	case WT_ITEM_KEY_DUP:
		return ("key-duplicate");
	case WT_ITEM_KEY_DUP_OVFL:
		return ("key-duplicate-overflow");
	case WT_ITEM_KEY_OVFL:
		return ("key-overflow");
	case WT_ITEM_OFF:
		return ("off-page");
	case WT_ITEM_OFF_RECORD:
		return ("off-page-records");
	default:
		break;
	}
	return ("unknown");
}
