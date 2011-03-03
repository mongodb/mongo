/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_desc_io(WT_TOC *, void *, int);

/*
 * __wt_desc_stat --
 *	Fill in the statistics from the file's description.
 */
int
__wt_desc_stat(WT_TOC *toc)
{
	WT_PAGE_DESC desc;
	WT_STATS *stats;

	stats = toc->db->idb->dstats;

	WT_RET(__wt_desc_io(toc, &desc, 1));

	WT_STAT_SET(stats, MAGIC, desc.magic);
	WT_STAT_SET(stats, MAJOR, desc.majorv);
	WT_STAT_SET(stats, MINOR, desc.minorv);
	WT_STAT_SET(stats, INTLMAX, desc.intlmax);
	WT_STAT_SET(stats, INTLMIN, desc.intlmin);
	WT_STAT_SET(stats, LEAFMAX, desc.leafmax);
	WT_STAT_SET(stats, LEAFMIN, desc.leafmin);
	WT_STAT_SET(stats, BASE_RECNO, desc.recno_offset);
	WT_STAT_SET(stats, FIXED_LEN, desc.fixed_len);

	return (0);
}

/*
 * __wt_desc_read --
 *	Read the descriptor structure from page 0.
 */
int
__wt_desc_read(WT_TOC *toc)
{
	DB *db;
	WT_PAGE_DESC desc;

	db = toc->db;

	WT_RET(__wt_desc_io(toc, &desc, 1));

	db->intlmax = desc.intlmax;		/* Update DB handle */
	db->intlmin = desc.intlmin;
	db->leafmax = desc.leafmax;
	db->leafmin = desc.leafmin;
	db->idb->root_page.addr = desc.root_addr;
	db->idb->root_page.size = desc.root_size;
	db->idb->free_addr = desc.free_addr;
	db->idb->free_size = desc.free_size;
	db->fixed_len = desc.fixed_len;

	/*
	 * XXX
	 * This is the wrong place to do this -- need to think about how
	 * to update open/configuration information in a reasonable way.
	 */
	if (db->fixed_len != 0)
		F_SET(db->idb, WT_COLUMN);

	return (0);
}

/*
 * __wt_desc_write --
 *	Update the description page.
 */
int
__wt_desc_write(WT_TOC *toc)
{
	DB *db;
	IDB *idb;
	WT_PAGE_DESC desc;
	int ret;

	db = toc->db;
	idb = db->idb;
	ret = 0;

	desc.magic = WT_BTREE_MAGIC;
	desc.majorv = WT_BTREE_MAJOR_VERSION;
	desc.minorv = WT_BTREE_MINOR_VERSION;
	desc.intlmax = db->intlmax;
	desc.intlmin = db->intlmin;
	desc.leafmax = db->leafmax;
	desc.leafmin = db->leafmin;
	desc.recno_offset = 0;
	desc.root_addr = idb->root_page.addr;
	desc.root_size = idb->root_page.size;
	desc.free_addr = idb->free_addr;
	desc.free_size = idb->free_size;
	desc.fixed_len = (uint8_t)db->fixed_len;
	desc.flags = 0;
	if (F_ISSET(idb, WT_RLE))
		F_SET(&desc, WT_PAGE_DESC_RLE);

	WT_RET(__wt_desc_io(toc, &desc, 0));

	return (ret);
}

/*
 * __wt_desc_io --
 *	Read/write the WT_DESC sector.
 */
static int
__wt_desc_io(WT_TOC *toc, void *p, int is_read)
{
	WT_FH *fh;
	ENV *env;

	fh = toc->db->idb->fh;
	env = toc->env;

	return (is_read ?
	    __wt_read(env, fh, (off_t)0, 512, p) :
	    __wt_write(env, fh, (off_t)0, 512, p));
}
