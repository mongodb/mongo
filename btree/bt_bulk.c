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
 * __wt_bt_bulk_load --
 *	Db.bulk_load method.
 */
int
__wt_bt_bulk_load(DB *db, int (*cb)(DB *, DBT **, DBT **))
{
	DBT *key, *data;
	IENV *ienv;
	WT_BTREE *bt;
	WT_ITEM item;
	WT_PAGE_HDR *hdr, *next_hdr;
	u_int32_t len, next_pgno, pgno, space_avail;
	u_int8_t *p;
	int ret;

	ienv = db->ienv;
	bt = db->idb->btree;
	pgno = WT_PGNO_INVALID;
	hdr = NULL;

	memset(&item, 0, sizeof(item));
	item.type = WT_ITEM_STANDARD;

	while ((ret = cb(db, &key, &data)) == 0) {
		if (key->size > db->maxitemsize) {
			/* Create overflow key */
			__wt_db_errx(db, "OVERFLOW KEY");
			__wt_abort(db->ienv);
		}
		if (data->size > db->maxitemsize) {
			/* Create overflow data */
			__wt_db_errx(db, "OVERFLOW DATA");
			__wt_abort(db->ienv);
		}

		/* 
		 * If there's insufficient space available, allocate a space
		 * from the backing file and connect it to the in-memory tree.
		 */
		if ((hdr == NULL ||
		    WT_ITEM_SPACE_REQ(key->size) +
		    WT_ITEM_SPACE_REQ(data->size) > space_avail)) {
			/* Allocate a new page. */
			if ((ret = __wt_bt_falloc(bt,
			    WT_BYTES_TO_BLOCKS(db->pagesize),
			    &next_hdr, &next_pgno)) != 0)
				goto err;
			next_hdr->type = WT_PAGE_BTREE_LEAF;
			next_hdr->prevpg = pgno;
			next_hdr->nextpg = WT_PGNO_INVALID;

			/* Write any filled page. */
			if (hdr != NULL) {
				hdr->nextpg = next_pgno;
				if ((ret = __wt_bt_fwrite(bt,
				    WT_PGNO_TO_BLOCKS(db, pgno),
				    WT_BYTES_TO_BLOCKS(db->pagesize),
				    hdr)) != 0)
					goto err;
			}

			space_avail = WT_DATA_SPACE(db->pagesize);
			hdr = next_hdr;
			pgno = next_pgno;
			p = (u_int8_t *)hdr + WT_HDR_SIZE;
		}

		hdr->entries += 2;
		space_avail -=
		    WT_ITEM_SPACE_REQ(key->size) +
		    WT_ITEM_SPACE_REQ(data->size);

		/* Copy the key/data pair onto the page. */
		item.len = key->size;
		memcpy(p, &item, sizeof(item));
		memcpy(p + sizeof(item), key->data, key->size);
		p += WT_ITEM_SPACE_REQ(key->size);

		item.len = data->size;
		memcpy(p, &item, sizeof(item));
		memcpy(p + sizeof(item), data->data, data->size);
		p += WT_ITEM_SPACE_REQ(data->size);
	}

	/* Write any partially-filled page. */
	if (ret == 1 && hdr != NULL)
		ret = __wt_bt_fwrite(bt,
		    WT_PGNO_TO_BLOCKS(db, pgno),
		    WT_BYTES_TO_BLOCKS(db->pagesize), hdr);

	return (ret);

err:	/* CLEAN OUT MEMORY */
	return (WT_ERROR);
}
