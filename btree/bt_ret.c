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
 * __wt_dbt_return --
 *	Copy a WT_PAGE/WT_INDX pair into a DBT for return to the application.
 */
int
__wt_dbt_return(DB *db, DBT *data, WT_PAGE *page, WT_INDX *indx)
{
	WT_ITEM *item;
	u_int8_t *p;
	u_int32_t size;
	int ret;

	if (page->hdr->type == WT_PAGE_LEAF) {
		if (indx->addr == WT_ADDR_INVALID) {
			item = indx->ditem;
			p = WT_ITEM_BYTE(item);
			size = item->len;
		} else
			return (__wt_db_ovfl_item_copy(db,
			    (WT_ITEM_OVFL *)WT_ITEM_BYTE(indx->ditem), data));
	} else {
		if (indx->addr == WT_ADDR_INVALID) {
			p = indx->data;
			size = indx->size;
		} else
			return (__wt_db_ovfl_item_copy(db,
			    (WT_ITEM_OVFL *)WT_ITEM_BYTE(indx->ditem), data));
	}

	if (data->alloc_size < size) {
		if ((ret =
		    __wt_realloc(db->ienv, size + 40, &data->data)) != 0)
			return (ret);
		data->alloc_size = size + 40;
	}

	data->size = size;
	memcpy(data->data, p, size);
	return (0);
}
