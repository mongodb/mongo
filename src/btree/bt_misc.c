/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

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
__wt_item_type_string(WT_CELL *item)
{
	switch (WT_CELL_TYPE(item)) {
	case WT_CELL_DATA:
		return ("data");
	case WT_CELL_DATA_OVFL:
		return ("data-overflow");
	case WT_CELL_DEL:
		return ("deleted");
	case WT_CELL_KEY:
		return ("key");
	case WT_CELL_KEY_OVFL:
		return ("key-overflow");
	case WT_CELL_OFF:
		return ("off-page");
	case WT_CELL_OFF_RECORD:
		return ("off-page-records");
	default:
		break;
	}
	return ("unknown");
}
