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
 * __wt_bt_desc_init --
 *	Initialize the database description on page 0.
 */
void
__wt_bt_desc_init(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_PAGE_DESC desc;

	idb = db->idb;

	desc.magic = WT_BTREE_MAGIC;
	desc.majorv = WT_BTREE_MAJOR_VERSION;
	desc.minorv = WT_BTREE_MINOR_VERSION;
	desc.leafsize = db->leafsize;
	desc.intlsize = db->intlsize;
	desc.base_recno = 0;
	desc.root_addr = WT_ADDR_INVALID;
	desc.free_addr = WT_ADDR_INVALID;
	desc.fixed_len = 0;
	desc.flags = 0;
	if (F_ISSET(idb, WT_REPEAT_COMP))
		F_SET(&desc, WT_PAGE_DESC_REPEAT);
	memset(desc.unused1, 0, sizeof(desc.unused1));
	memset(desc.unused2, 0, sizeof(desc.unused2));

	memcpy(
	    (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, &desc, WT_PAGE_DESC_SIZE);
}

/*
 * __wt_bt_desc_stats --
 *	Fill in the statistics from the database description on page 0.
 */
void
__wt_bt_desc_stats(DB *db, WT_PAGE *page)
{
	IDB *idb;
	WT_PAGE_DESC desc;

	idb = db->idb;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);

	WT_STAT_SET(idb->dstats, MAGIC, desc.magic);
	WT_STAT_SET(idb->dstats, MAJOR, desc.majorv);
	WT_STAT_SET(idb->dstats, MINOR, desc.minorv);
	WT_STAT_SET(idb->dstats, LEAFSIZE, desc.leafsize);
	WT_STAT_SET(idb->dstats, INTLSIZE, desc.intlsize);
	WT_STAT_SET(idb->dstats, BASE_RECNO, desc.base_recno);
	WT_STAT_SET(idb->dstats, FIXED_LEN, desc.fixed_len);
}

/*
 * __wt_bt_desc_verify --
 *	Verify the database description on page 0.
 */
int
__wt_bt_desc_verify(DB *db, WT_PAGE *page)
{
	WT_PAGE_DESC desc;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);

	if (desc.magic != WT_BTREE_MAGIC) {
		__wt_api_db_errx(db, "magic number %#lx, expected %#lx",
		    (u_long)desc.magic, WT_BTREE_MAGIC);
		return (WT_ERROR);
	}
	if (desc.majorv != WT_BTREE_MAJOR_VERSION) {
		__wt_api_db_errx(db, "major version %d, expected %d",
		    (int)desc.majorv, WT_BTREE_MAJOR_VERSION);
		return (WT_ERROR);
	}
	if (desc.minorv != WT_BTREE_MINOR_VERSION) {
		__wt_api_db_errx(db, "minor version %d, expected %d",
		    (int)desc.minorv, WT_BTREE_MINOR_VERSION);
		return (WT_ERROR);
	}
	if (desc.leafsize != db->leafsize) {
		__wt_api_db_errx(db, "leaf page size %lu, expected %lu",
		    (u_long)db->leafsize, (u_long)desc.leafsize);
		return (WT_ERROR);
	}
	if (desc.intlsize != db->intlsize) {
		__wt_api_db_errx(db, "internal page size %lu, expected %lu",
		    (u_long)db->intlsize, (u_long)desc.intlsize);
		return (WT_ERROR);
	}
	if (desc.base_recno != 0) {
		__wt_api_db_errx(db, "base recno %llu, expected 0",
		    (u_quad)desc.base_recno);
		return (WT_ERROR);
	}
	if (desc.fixed_len == 0 && F_ISSET(&desc, WT_PAGE_DESC_REPEAT)) {
		__wt_api_db_errx(db,
		    "repeat counts configured but no fixed length record "
		    "size specified");
		return (WT_ERROR);
	}
	if (F_ISSET(&desc, ~WT_PAGE_DESC_MASK)) {
		__wt_api_db_errx(db,
		    "unexpected flags found in description record");
		return (WT_ERROR);
	}

	if (desc.unused1[0] || desc.unused1[1] || desc.unused1[2] ||
	    desc.unused2[0] || desc.unused2[1] || desc.unused2[2] ||
	    desc.unused2[3] || desc.unused2[4] || desc.unused2[5]) {
		__wt_api_db_errx(db,
		    "unexpected values found in unused fields");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_bt_desc_read --
 *	Read the descriptor structure from page 0, and update the DB handle
 *	to reflect that information.
 */
int
__wt_bt_desc_read(WT_TOC *toc, u_int32_t *root_addrp)
{
	DB *db;
	ENV *env;
	WT_FH *fh;
	WT_PAGE *page;
	WT_PAGE_DESC *desc;
	int diskread;
	u_int8_t buf[WT_FRAGMENT];

	db = toc->db;
	env = toc->env;
	fh = db->idb->fh;

	/* If the file size is 0, we're done. */
	if (fh->file_size == 0) {
		if (root_addrp != NULL)
			*root_addrp = WT_ADDR_INVALID;
		return (0);
	}

	/*
	 * Read in the first fragment of the database and get the root addr
	 * and pagesizes from it.
	 *
	 * We may be reading blind (when we first read the description chunk
	 * after opening the file we don't yet know the database page size).
	 * Look in the cache and see if it has something for us; if that
	 * doesn't work, read the disk directly.
	 */
	WT_RET(__wt_cache_in(toc, WT_ADDR_FIRST_PAGE, 0, &page));
	if (page == NULL) {
		WT_RET(__wt_read(env, fh,
		    WT_ADDR_TO_OFF(db, WT_ADDR_FIRST_PAGE),
		    (u_int32_t)WT_FRAGMENT, buf));
		desc = (WT_PAGE_DESC *)(buf + WT_PAGE_HDR_SIZE);
		diskread = 1;
	} else {
		desc =
		   (WT_PAGE_DESC *)((u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE);
		diskread = 0;
	}
	db->leafsize = desc->leafsize;
	db->intlsize = desc->intlsize;
	db->fixed_len = desc->fixed_len;
	if (root_addrp != NULL)
		*root_addrp = desc->root_addr;

	return (diskread ? 0 : __wt_cache_out(toc, page, 0));
}

/*
 * __wt_bt_desc_write --
 *	Update the root addr.
 */
int
__wt_bt_desc_write(WT_TOC *toc, u_int32_t root_addr)
{
	DB *db;
	WT_PAGE *page;
	WT_PAGE_DESC *desc;

	db = toc->db;

	WT_RET(__wt_cache_in(toc, WT_ADDR_FIRST_PAGE, db->leafsize, &page));

	desc = (WT_PAGE_DESC *)((u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE);
	desc->root_addr = root_addr;
	desc->leafsize = db->leafsize;
	desc->intlsize = db->intlsize;
	desc->fixed_len = (u_int8_t)db->fixed_len;

	return (__wt_cache_out(toc, page, WT_MODIFIED));
}

