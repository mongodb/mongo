/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_dbt_copyout(DB *, DBT *, DBT *, u_int8_t *, u_int32_t);

/*
 * __wt_bt_dbt_return --
 *	Copy a WT_PAGE/WT_INDX pair into a key/data pair for return to the
 *	application.
 */
int
__wt_bt_dbt_return(DB *db,
    DBT *key, DBT *data, WT_PAGE *page, WT_INDX *ip, int key_return)
{
	IDB *idb;
	DBT local_key, local_data;
	WT_ITEM *item;
	WT_ITEM_OVFL *ovfl;
	int (*callback)(DB *, DBT *, DBT *);

	idb = db->idb;

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
			WT_RET((__wt_bt_ovfl_to_indx(db, page, ip)));
		if (callback == NULL) {
			WT_RET((__wt_bt_dbt_copyout(
			    db, key, &idb->key, ip->data, ip->size)));
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
			WT_RET((__wt_bt_dbt_copyout(db, data,
			    &idb->data, WT_ITEM_BYTE(item),
			    (u_int32_t)WT_ITEM_LEN(item))));
		} else {
			data->data = WT_ITEM_BYTE(item);
			data->size = (u_int32_t)WT_ITEM_LEN(item);
		}
		break;
	case WT_PAGE_DUP_LEAF:
		if (WT_ITEM_TYPE(item) != WT_ITEM_DUP)
			goto overflow;

		if (callback == NULL) {
			WT_RET((__wt_bt_dbt_copyout(db, data,
			    &idb->data, ip->data, ip->size)));
		} else {
			data->data = ip->data;
			data->size = ip->size;
		}
		break;
	WT_DEFAULT_FORMAT(db);
	}

	if (0) {
overflow:	/*
		 * Handle overflow data items.
		 *
		 * The overflow copy routines always attempt to resize the
		 * buffer if necessary, which isn't correct in the case of
		 * user-memory.  Check before calling them.
		 */
		ovfl = (WT_ITEM_OVFL *)WT_ITEM_BYTE(ip->ditem);
		if (F_ISSET(data, WT_DBT_ALLOC | WT_DBT_APPMEM)) {
			if (F_ISSET(data, WT_DBT_APPMEM) &&
			    data->data_len < ovfl->len)
				return (WT_TOOSMALL);
			WT_RET((__wt_bt_ovfl_to_dbt(db, ovfl, data)));
		} else {
			WT_RET((__wt_bt_ovfl_to_dbt(db, ovfl, &idb->data)));
			data->data = idb->data.data;
			data->size = idb->data.size;
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
    DB *db, DBT *dbt, DBT *local_dbt, u_int8_t *p, u_int32_t size)
{
	ENV *env;

	env = db->env;

	/*
	 * By default, we use memory in the DB handle to return keys; if
	 * the DB handle is being used by multiple threads, however, we
	 * can't do that, we have to use memory in the DBT itself.  Look
	 * for the appropriate flags.
	 */
	if (F_ISSET(dbt, WT_DBT_ALLOC)) {
		if (dbt->data_len < size) {
			WT_RET((__wt_realloc(
			    env, dbt->data_len, size, &dbt->data)));
			dbt->data_len = size;
		}
	} else if (F_ISSET(dbt, WT_DBT_APPMEM)) {
		if (dbt->data_len < size)
			return (WT_TOOSMALL);
	} else {
		if (local_dbt->data_len < size) {
			WT_RET((__wt_realloc(
			    env, size, size + 40, &local_dbt->data)));
			local_dbt->data_len = size + 40;
		}
		dbt->data = local_dbt->data;
	}

	memcpy(dbt->data, p, size);
	dbt->size = size;
	return (0);
}
