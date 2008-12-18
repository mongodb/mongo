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
 * __wt_db_ovfl_copy --
 *	Copy an overflow item in the database, returning the starting
 *	addr.  This routine is used when an overflow item is promoted
 *	to an internal page.
 */
int
__wt_db_ovfl_copy(DB *db, WT_ITEM_OVFL *from, WT_ITEM_OVFL *copy)
{
	DBT dbt;
	WT_BTREE *bt;
	WT_PAGE_HDR *ovfl_hdr;
	u_int32_t frags;
	int ret, tret;

	bt = db->idb->btree;

	/* Read in the overflow record. */
	WT_OVERFLOW_BYTES_TO_FRAGS(db, from->len, frags);
	if ((ret = __wt_db_fread(bt, from->addr, frags, &ovfl_hdr)) != 0)
		return (ret);

	/*
	 * Copy the overflow record to a new location, and set our return
	 * information.
	 */
	WT_CLEAR(dbt);
	dbt.data = WT_PAGE_BYTE(ovfl_hdr);
	dbt.size = from->len;
	ret = __wt_db_ovfl_write(db, &dbt, &copy->addr);
	copy->len = from->len;

	/* Discard the overflow record. */
	if ((tret =
	    __wt_db_fdiscard(bt, from->addr, ovfl_hdr)) != 0 && ret == 0)
		ret = tret;

	return (ret);
}

/*
 * __wt_db_ovfl_write --
 *	Store an overflow item in the database, returning the starting
 *	addr.
 */
int
__wt_db_ovfl_write(DB *db, DBT *dbt, u_int32_t *addrp)
{
	WT_BTREE *bt;
	WT_PAGE_HDR *hdr;
	u_int32_t addr, frags;
	u_int64_t bytes;
	int ret;

	bt = db->idb->btree;

	/* Allocate a chunk of file space. */
	WT_OVERFLOW_BYTES_TO_FRAGS(db, dbt->size, frags);
	if ((ret = __wt_db_falloc(bt, frags, &hdr, &addr)) != 0)
		return (ret);

	/* Initialize the returned space. */
	hdr->type = WT_PAGE_OVFL;
	hdr->u.datalen = dbt->size;

	/* Copy the DBT into place. */
	memcpy(WT_PAGE_BYTE(hdr), dbt->data, dbt->size);

	/* Write the overflow item back to the file. */
	if ((ret = __wt_db_fwrite(bt, addr, frags, hdr)) != 0)
		return (ret);

	*addrp = addr;
	return (0);
}
