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
 * __wt_db_desc_init --
 *	Initialize the database description on page 0.
 */
void
__wt_db_desc_init(DB *db, WT_PAGE *page)
{
	WT_PAGE_DESC desc;

	desc.magic = WT_BTREE_MAGIC;
	desc.majorv = WT_BTREE_MAJOR_VERSION;
	desc.minorv = WT_BTREE_MINOR_VERSION;
	desc.pagesize = db->pagesize;
	desc.root_addr = WT_ADDR_INVALID;
	desc.free_addr = WT_ADDR_INVALID;
	desc.base_recno = 0;
	desc.unused[0] = 0;
	desc.unused[1] = 0;
	desc.unused[2] = 0;
	desc.unused[3] = 0;
	desc.unused[4] = 0;
	desc.unused[5] = 0;
	desc.unused[6] = 0;
	desc.unused[7] = 0;

	memcpy((u_int8_t *)page->hdr + WT_HDR_SIZE, &desc, WT_DESC_SIZE);
}

/*
 * __wt_db_desc_verify --
 *	Verify the database description on page 0.
 */
int
__wt_db_desc_verify(DB *db, WT_PAGE *page)
{
	WT_PAGE_DESC desc;

	memcpy(&desc, (u_int8_t *)page->hdr + WT_HDR_SIZE, WT_DESC_SIZE);

	return (desc.magic != WT_BTREE_MAGIC ||
	    desc.majorv != WT_BTREE_MAJOR_VERSION ||
	    desc.minorv != WT_BTREE_MINOR_VERSION ||
	    desc.pagesize != db->pagesize ||
	    desc.base_recno != 0 ||
	    desc.unused[0] != 0 ||
	    desc.unused[1] != 0 ||
	    desc.unused[2] != 0 ||
	    desc.unused[3] != 0 ||
	    desc.unused[4] != 0 ||
	    desc.unused[5] != 0 ||
	    desc.unused[6] != 0 ||
	    desc.unused[7] != 0 ? WT_ERROR : 0);
}

/*
 * __wt_db_desc_set_root --
 *	Update the root addr.
 */
int
__wt_db_desc_set_root(DB *db, u_int32_t root_addr)
{
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_DESC desc;
	int ret;

	idb = db->idb;

	if ((ret = __wt_db_page_in(db,
	    WT_ADDR_FIRST_PAGE, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
		return (ret);

	idb->root_addr = root_addr;

	memcpy(&desc, (u_int8_t *)page->hdr + WT_HDR_SIZE, WT_DESC_SIZE);
	desc.root_addr = root_addr;
	memcpy((u_int8_t *)page->hdr + WT_HDR_SIZE, &desc, WT_DESC_SIZE);

	return (__wt_db_page_out(db, page, WT_MODIFIED));
}

/*
 * __wt_db_desc_read --
 *	Read the descriptor structure from page 0.
 */
int
__wt_db_desc_read(DB *db, WT_PAGE_DESC *desc)
{
	WT_PAGE *page;
	int ret;

	if ((ret = __wt_db_page_in(db,
	    WT_ADDR_FIRST_PAGE, WT_FRAGS_PER_PAGE(db), &page, 0)) != 0)
		return (ret);

	memcpy(desc, (u_int8_t *)page->hdr + WT_HDR_SIZE, WT_DESC_SIZE);

	return (__wt_db_page_out(db, page, 0));
}
