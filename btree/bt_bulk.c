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
 * __wt_db_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_db_bulk_load(DB *db, u_int32_t flags, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM key_item, data_item;
	WT_ITEM_OVFL key_ovfl, data_ovfl;
	WT_PAGE_HDR *hdr, *next_hdr;
	u_int32_t addr, len, next_addr, space_avail;
	u_int8_t *p;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;
	addr = WT_ADDR_INVALID;
	hdr = NULL;

	DB_FLAG_CHK(db, "Db.bulk_load", flags, WT_APIMASK_DB_BULK_LOAD);
	WT_ASSERT(ienv, !LF_ISSET(WT_DUPLICATES));
	WT_ASSERT(ienv, LF_ISSET(WT_SORTED_INPUT));

	memset(&key_item, 0, sizeof(key_item));
	memset(&data_item, 0, sizeof(data_item));

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key->size > db->maxitemsize) {
			key_ovfl.len = key->size;
			if ((ret =
			    __wt_bt_ovfl_load(db, key, &key_ovfl.addr)) != 0)
				goto err;
			key->data = &key_ovfl;
			key->size = sizeof(key_ovfl);
			key_item.type = WT_ITEM_OVERFLOW;
		} else
			key_item.type = WT_ITEM_STANDARD;
		if (data->size > db->maxitemsize) {
			data_ovfl.len = data->size;
			if ((ret =
			    __wt_bt_ovfl_load(db, data, &data_ovfl.addr)) != 0)
				goto err;
			data->data = &data_ovfl;
			data->size = sizeof(data_ovfl);
			data_item.type = WT_ITEM_OVERFLOW;
		} else
			data_item.type = WT_ITEM_STANDARD;

		/* 
		 * If there's insufficient space available, allocate a space
		 * from the backing file and connect it to the in-memory tree.
		 */
		if ((hdr == NULL ||
		    WT_ITEM_SPACE_REQ(key->size) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail)) {
			/* Allocate a new page. */
			if ((ret = __wt_bt_falloc(bt,
			    db->frags_per_page, &next_hdr, &next_addr)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_BTREE_LEAF;
			next_hdr->prevaddr = addr;
			next_hdr->nextaddr = WT_ADDR_INVALID;

			/* Write any filled page. */
			if (hdr != NULL) {
				hdr->nextaddr = next_addr;
				if ((ret = __wt_bt_fwrite(bt,
				    addr, db->frags_per_page, hdr)) != 0)
					goto err;
			}

			space_avail = WT_DATA_SPACE(db->pagesize);
			hdr = next_hdr;
			addr = next_addr;
			p = WT_PAGE_DATA(hdr);
		}

		hdr->entries += 2;
		space_avail -=
		    WT_ITEM_SPACE_REQ(key->size) +
		    WT_ITEM_SPACE_REQ(data->size);

		/* Copy the key/data pair onto the page. */
		key_item.len = key->size;
		memcpy(p, &key_item, sizeof(key_item));
		memcpy(p + sizeof(key_item), key->data, key->size);
		p += WT_ITEM_SPACE_REQ(key->size);

		data_item.len = data->size;
		memcpy(p, &data_item, sizeof(data_item));
		memcpy(p + sizeof(data_item), data->data, data->size);
		p += WT_ITEM_SPACE_REQ(data->size);
	}

	/* Write any partially-filled page. */
	if (ret == 1 && hdr != NULL)
		ret = __wt_bt_fwrite(bt, addr, db->frags_per_page, hdr);

	return (ret);

err:	/* CLEAN OUT MEMORY */
	return (WT_ERROR);
}
