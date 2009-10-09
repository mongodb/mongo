/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_dbt_copyout(WT_TOC *, DBT *, DBT *, u_int8_t *, u_int32_t);

/*
 * __wt_bt_dbt_return --
 *	Copy a WT_PAGE/WT_INDX pair into a key/data pair for return to the
 *	application.
 */
int
__wt_bt_dbt_return(WT_TOC *toc,
    DBT *key, DBT *data, WT_PAGE *page, WT_INDX *ip, int key_return)
{
	DB *db;
	DBT local_key, local_data;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	int (*callback)(DB *, DBT *, DBT *);

	db = toc->db;

	/*
	 * If the data DBT has been configured for a callback return, we don't
	 * have to build or copy anything except overflow key/data items.  We
	 * don't want to overwrite the user's DBTs, however, so use local ones.
	 */
	if ((callback = data->callback) != NULL) {
		if (key_return) {
			key = &local_key;
			WT_CLEAR(local_key);
		}
		data = &local_data;
		WT_CLEAR(local_data);
	}

	/*
	 * Handle the key -- the key may be unchanged, in which case don't
	 * touch it.
	 *
	 * If the key is an overflow, it may not have been instantiated yet.
	 */
	if (key_return) {
		if (ip->data == NULL)
			WT_RET(__wt_bt_ovfl_to_indx(toc, page, ip));
		if (callback == NULL) {
			WT_RET(__wt_bt_dbt_copyout(
			    toc, key, &toc->key, ip->data, ip->size));
		} else {
			key->data = ip->data;
			key->size = ip->size;
		}
	}

	/*
	 * Handle the data item.
	 */
	item = ip->ditem;
	switch (page->hdr->type) {
	case WT_PAGE_LEAF:
		if (WT_ITEM_TYPE(item) != WT_ITEM_DATA)
			goto overflow;

		if (callback == NULL) {
			WT_RET(__wt_bt_dbt_copyout(toc, data,
			    &toc->data, WT_ITEM_BYTE(item),
			    (u_int32_t)WT_ITEM_LEN(item)));
		} else {
			data->data = WT_ITEM_BYTE(item);
			data->size = (u_int32_t)WT_ITEM_LEN(item);
		}
		break;
	case WT_PAGE_DUP_LEAF:
		if (WT_ITEM_TYPE(item) != WT_ITEM_DUP)
			goto overflow;

		if (callback == NULL) {
			WT_RET(__wt_bt_dbt_copyout(toc, data,
			    &toc->data, ip->data, ip->size));
		} else {
			data->data = ip->data;
			data->size = ip->size;
		}
		break;
	WT_DEFAULT_FORMAT(db);
	}

	if (0) {
overflow:	/* Handle overflow data items. */
		ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(ip->ditem);
		if (F_ISSET(data, WT_DBT_ALLOC))
			WT_RET(__wt_bt_ovfl_to_dbt(toc, ovfl, data));
		else {
			WT_RET(__wt_bt_ovfl_to_dbt(toc, ovfl, &toc->data));
			data->data = toc->data.data;
			data->size = toc->data.size;
		}
	}

	if (callback == NULL)
		return (0);

	return (callback(db, key, data));
}

/*
 * __wt_bt_dbt_copyout --
 *	Do the actual allocation and copy for a returned DBT.
 */
static int
__wt_bt_dbt_copyout(
    WT_TOC *toc, DBT *dbt, DBT *local_dbt, u_int8_t *p, u_int32_t size)
{
	ENV *env;

	env = toc->env;

	/*
	 * We use memory in the TOC handle to return keys -- it's a per-thread
	 * structure, so there's no chance of a race.
	 */
	if (F_ISSET(dbt, WT_DBT_ALLOC)) {
		if (dbt->data_len < size) {
			WT_RET(__wt_realloc(
			    env, dbt->data_len, size, &dbt->data));
			dbt->data_len = size;
		}
	} else {
		if (local_dbt->data_len < size) {
			WT_RET(__wt_realloc(
			    env, size, size + 40, &local_dbt->data));
			local_dbt->data_len = size + 40;
		}
		dbt->data = local_dbt->data;
	}

	memcpy(dbt->data, p, size);
	dbt->size = size;
	return (0);
}
