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
 * __wt_bt_ovfl_load --
 *	Store an overflow item in the database, returning the starting
 *	page.
 */
int
__wt_bt_ovfl_load(DB *db, DBT *dbt, u_int32_t *addrp)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags;
	u_int64_t bytes;
	int ret;

	bt = db->idb->btree;

	/* Allocate a chunk of file space. */
	WT_OVERFLOW_BYTES_TO_FRAGS(db, dbt->size, frags);
	if ((ret = __wt_bt_falloc(bt, frags, &hdr, &addr)) != 0)
		return (ret);

	/* Initialize the returned space. */
	hdr->type = WT_PAGE_OVFL;
	hdr->u.datalen = dbt->size;

	/* Copy the DBT into place. */
	memcpy(WT_PAGE_DATA(hdr), dbt->data, dbt->size);

	/* Write the overflow item back to the file. */
	if ((ret = __wt_bt_fwrite(bt, addr, frags, hdr)) != 0)
		return (ret);

	*addrp = addr;
	return (0);
}
