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
 * __wt_bt_dbt_return --
 *	Copy a WT_PAGE/WT_INDX pair into a DBT for return to the application.
 */
int
__wt_bt_dbt_return(DB *db, DBT *data, WT_PAGE *page, WT_INDX *indx)
{
	ENV *env;
	WT_ITEM *item;
	u_int8_t *p;
	u_int32_t size;
	int ret;

	env = db->env;

	switch (page->hdr->type) {
	case WT_PAGE_LEAF:
		item = indx->ditem;
		if (item->type == WT_ITEM_DATA) {
			p = WT_ITEM_BYTE(item);
			size = item->len;
			break;
		}
		return (__wt_bt_ovfl_copy_to_dbt(db,
		    (WT_ITEM_OVFL *)WT_ITEM_BYTE(indx->ditem), data));
	case WT_PAGE_DUP_LEAF:
		if (indx->addr == WT_ADDR_INVALID) {
			p = indx->data;
			size = indx->size;
			break;
		}
		return (__wt_bt_ovfl_copy_to_dbt(db,
		    (WT_ITEM_OVFL *)WT_ITEM_BYTE(indx->ditem), data));
	}

	if (data->alloc_size < size) {
		if ((ret = __wt_realloc(env, size + 40, &data->data)) != 0)
			return (ret);
		data->alloc_size = size + 40;
	}

	data->size = size;
	memcpy(data->data, p, size);
	return (0);
}
