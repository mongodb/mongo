/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_dbt_copy(DB *, DBT *, DBT *, u_int8_t *, u_int32_t);

/*
 * __wt_bt_dbt_return --
 *	Copy a WT_PAGE/WT_INDX pair into a key/data pair for return to the
 *	application.
 */
int
__wt_bt_dbt_return(DB *db, DBT *key, DBT *data, WT_PAGE *page, WT_INDX *ip)
{
	IDB *idb;
	DBT *local_dbt;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	int ret;

	idb = db->idb;

	/*
	 * Handle the key.
	 *
	 * If the key is an overflow, it may not have been instantiated yet.
	 */
	if (key != NULL) {
		if (ip->data == NULL &&
		    (ret = __wt_bt_ovfl_copy_to_indx(db, page, ip)) != 0)
			return (ret);
		if ((ret = __wt_bt_dbt_copy(
		    db, key, &idb->key, ip->data, ip->size)) != 0)
			return (ret);
	}

	if (data == NULL)
		return (0);

	/*
	 * Handle the data item.
	 */
	item = ip->ditem;
	switch (page->hdr->type) {
	case WT_PAGE_LEAF:
		if (WT_ITEM_TYPE(item) == WT_ITEM_DATA)
			return (__wt_bt_dbt_copy(db, data, &idb->data,
			    WT_ITEM_BYTE(item), (u_int32_t)WT_ITEM_LEN(item)));
		/* It's an overflow item. */
		break;
	case WT_PAGE_DUP_LEAF:
		if (WT_ITEM_TYPE(item) == WT_ITEM_DUP)
			return (__wt_bt_dbt_copy(
			    db, data, &idb->data, ip->data, ip->size));
		/* It's an overflow item. */
		break;
	WT_DEFAULT_FORMAT(db);
	}

	/*
	 * Handle overflow data items.
	 *
	 * The overflow copy routines always attempt to resize the buffer if
	 * necessary, which isn't correct in the case of user-memory.  Check
	 * before calling them.
	 */
	ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(ip->ditem);
	if (F_ISSET(data, WT_DBT_ALLOC | WT_DBT_APPMEM)) {
		if (F_ISSET(data, WT_DBT_APPMEM) && data->data_len < ovfl->len)
			return (WT_TOOSMALL);
		return (__wt_bt_ovfl_copy_to_dbt(db, ovfl, data));
	}

	local_dbt = &idb->data;
	if ((ret = __wt_bt_ovfl_copy_to_dbt(db, ovfl, local_dbt)) != 0)
		return (ret);
	data->data = local_dbt->data;
	data->size = local_dbt->size;
	return (0);
}

/*
 * __wt_bt_dbt_copy --
 *	Do the actual allocation and copy for a returned DBT.
 */
static int
__wt_bt_dbt_copy(DB *db, DBT *dbt, DBT *local_dbt, u_int8_t *p, u_int32_t size)
{
	ENV *env;
	int ret;

	env = db->env;

	/*
	 * By default, we use memory in the DB handle to return keys; if
	 * the DB handle is being used by multiple threads, however, we
	 * can't do that, we have to use memory in the DBT itself.  Look
	 * for the appropriate flags.
	 */
	if (F_ISSET(dbt, WT_DBT_ALLOC)) {
		if (dbt->data_len < size) {
			if ((ret = __wt_realloc(
			    env, dbt->data_len, size, &dbt->data)) != 0)
				return (ret);
			dbt->data_len = size;
		}
	} else if (F_ISSET(dbt, WT_DBT_APPMEM)) {
		if (dbt->data_len < size)
			return (WT_TOOSMALL);
	} else {
		if (local_dbt->data_len < size) {
			if ((ret = __wt_realloc(
			    env, size, size + 40, &local_dbt->data)) != 0)
				return (ret);
			local_dbt->data_len = size + 40;
		}
		dbt->data = local_dbt->data;
	}

	memcpy(dbt->data, p, size);
	dbt->size = size;
	return (0);
}
