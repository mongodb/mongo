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
	DBT *idata;
	ENV *env;
	IDB *idb;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	u_int8_t *p;
	u_int32_t size;
	int ret;

	idb = db->idb;
	env = db->env;

	switch (page->hdr->type) {
	case WT_PAGE_LEAF:
		item = indx->ditem;
		if (WT_ITEM_TYPE(item) == WT_ITEM_DATA) {
			p = WT_ITEM_BYTE(item);
			size = WT_ITEM_LEN(item);
			break;
		}
		goto ovfl;
	case WT_PAGE_DUP_LEAF:
		if (indx->addr == WT_ADDR_INVALID) {
			p = indx->data;
			size = indx->size;
			break;
		}
		goto ovfl;
	WT_DEFAULT_FORMAT(db);
	}

	/*
	 * By default, we use memory in the DB handle to return keys; if
	 * the DB handle is being used by multiple threads, however, we
	 * can't do that, we have to use memory in the DBT itself.  Look
	 * for the appropriate flags.
	 */
	if (F_ISSET(data, WT_DBT_ALLOC)) {
		if (data->data_len < size) {
			if ((ret = __wt_realloc(env, size, &data->data)) != 0)
				return (ret);
			data->size = size;
		}
	} else if (F_ISSET(data, WT_DBT_APPMEM)) {
		if (data->data_len < size)
			return (WT_TOOSMALL);
	} else {
		idata = &idb->data;
		if (idata->data_len < size) {
			if ((ret =
			    __wt_realloc(env, size + 40, &idata->data)) != 0)
				return (ret);
			idata->data_len = size + 40;
		}
		data->data = idata->data;
	}

	data->size = size;
	memcpy(data->data, p, size);
	return (0);

ovfl:	/*
	 * The overflow copy routines always attempt to resize the buffer if
	 * necessary, which isn't correct in the case of user-memory.  Check
	 * before calling them.
	 */
	ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(indx->ditem);
	if (F_ISSET(data, WT_DBT_ALLOC | WT_DBT_APPMEM)) {
		if (F_ISSET(data, WT_DBT_APPMEM) && data->data_len < ovfl->len)
			return (WT_TOOSMALL);
		return (__wt_bt_ovfl_copy_to_dbt(db, ovfl, data));
	}

	idata = &idb->data;
	if ((ret = __wt_bt_ovfl_copy_to_dbt(db, ovfl, idata)) != 0)
		return (ret);
	data->data = idata->data;
	data->size = idata->size;
	return (0);
}
