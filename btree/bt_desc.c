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
	WT_PAGE_DESC desc;

	desc.magic = WT_BTREE_MAGIC;
	desc.majorv = WT_BTREE_MAJOR_VERSION;
	desc.minorv = WT_BTREE_MINOR_VERSION;
	desc.leafsize = db->leafsize;
	desc.intlsize = db->intlsize;
	desc.base_recno = 0;
	desc.root_addr = WT_ADDR_INVALID;
	desc.free_addr = WT_ADDR_INVALID;
	desc.unused[0] = 0;
	desc.unused[1] = 0;
	desc.unused[2] = 0;
	desc.unused[3] = 0;
	desc.unused[4] = 0;
	desc.unused[5] = 0;
	desc.unused[6] = 0;

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
	WT_PAGE_DESC desc;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);

	WT_STAT_SET(db->dstats, MAGIC, "magic number", desc.magic);
	WT_STAT_SET(db->dstats, MAJOR, "major version number", desc.majorv);
	WT_STAT_SET(db->dstats, MINOR, "minor version number", desc.minorv);
	WT_STAT_SET(db->dstats, LEAFSIZE, "leaf page size", desc.leafsize);
	WT_STAT_SET(db->dstats, INTLSIZE, "internal page size", desc.intlsize);
	WT_STAT_SET(
	    db->dstats, BASE_RECNO, "base record number", desc.base_recno);
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

	return (desc.magic != WT_BTREE_MAGIC ||
	    desc.majorv != WT_BTREE_MAJOR_VERSION ||
	    desc.minorv != WT_BTREE_MINOR_VERSION ||
	    desc.leafsize != db->leafsize ||
	    desc.intlsize != db->intlsize ||
	    desc.base_recno != 0 ||
	    desc.unused[0] != 0 ||
	    desc.unused[1] != 0 ||
	    desc.unused[2] != 0 ||
	    desc.unused[3] != 0 ||
	    desc.unused[4] != 0 ||
	    desc.unused[5] != 0 ||
	    desc.unused[6] != 0 ? WT_ERROR : 0);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_bt_desc_dump --
 *	Verify the database description on page 0.
 */
void
__wt_bt_desc_dump(WT_PAGE *page, FILE *fp)
{
	WT_PAGE_DESC desc;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);

	fprintf(fp, "magic: %#lx, major: %lu, minor: %lu\n",
	    (u_long)desc.magic, (u_long)desc.majorv, (u_long)desc.minorv);
	fprintf(fp, "intlsize: %lu, leafsize: %lu, base record: %llu\n",
	    (u_long)desc.intlsize,
	    (u_long)desc.leafsize, desc.base_recno);
	if (desc.root_addr == WT_ADDR_INVALID)
		fprintf(fp, "root addr (none), ");
	else
		fprintf(fp, "root addr %lu, ", (u_long)desc.root_addr);
	if (desc.free_addr == WT_ADDR_INVALID)
		fprintf(fp, "free addr (none), ");
	else
		fprintf(fp, "free addr %lu, ", (u_long)desc.free_addr);
	fprintf(fp, "\n");
}
#endif

/*
 * __wt_bt_desc_read --
 *	Read the descriptor structure from page 0, and update the DB handle
 *	to reflect that information.
 */
int
__wt_bt_desc_read(DB *db)
{
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_DESC desc;

	idb = db->idb;

	/*
	 * When we first read the description chunk after first opening the
	 * file, we're reading blind, we don't know the database page size.
	 *
	 * Read in the first fragment of the database and get the root addr
	 * and pagesizes from it.
	 */
	WT_RET(__wt_cache_in(idb->stoc,
	    WT_ADDR_TO_OFF(db, WT_ADDR_FIRST_PAGE),
	    (u_int32_t)WT_FRAGMENT, WT_UNFORMATTED, &page));

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);
	db->leafsize = desc.leafsize;
	db->intlsize = desc.intlsize;
	idb->root_addr = desc.root_addr;

	/* Then discard it from the cache, it's probably the wrong size. */
	WT_RET(__wt_cache_out(idb->stoc, page, WT_UNFORMATTED));

	return (0);
}

/*
 * __wt_bt_desc_write --
 *	Update the root addr.
 */
int
__wt_bt_desc_write(DB *db, u_int32_t root_addr)
{
	IDB *idb;
	WT_PAGE *page;
	WT_PAGE_DESC desc;
	WT_STOC *stoc;

	idb = db->idb;
	stoc = idb->stoc;

	WT_RET(__wt_cache_in(stoc,
	    WT_ADDR_TO_OFF(db, WT_ADDR_FIRST_PAGE), db->leafsize, 0, &page));

	idb->root_addr = root_addr;

	memcpy(
	    &desc, (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, WT_PAGE_DESC_SIZE);
	desc.root_addr = root_addr;
	desc.leafsize = db->leafsize;
	desc.intlsize = db->intlsize;
	memcpy(
	    (u_int8_t *)page->hdr + WT_PAGE_HDR_SIZE, &desc, WT_PAGE_DESC_SIZE);

	return (__wt_cache_out(stoc, page, WT_MODIFIED));
}

