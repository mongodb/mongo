/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_desc_io(SESSION *, void *, int);

/*
 * __wt_desc_stat --
 *	Fill in the statistics from the file's description.
 */
int
__wt_desc_stat(SESSION *session)
{
	WT_PAGE_DESC desc;
	WT_STATS *stats;

	stats = session->btree->fstats;

	WT_RET(__wt_desc_io(session, &desc, 1));

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
__wt_desc_read(SESSION *session)
{
	BTREE *btree;
	WT_PAGE_DESC desc;

	btree = session->btree;

	WT_RET(__wt_desc_io(session, &desc, 1));

	btree->intlmax = desc.intlmax;		/* Update DB handle */
	btree->intlmin = desc.intlmin;
	btree->leafmax = desc.leafmax;
	btree->leafmin = desc.leafmin;
	btree->root_page.addr = desc.root_addr;
	btree->root_page.size = desc.root_size;
	btree->free_addr = desc.free_addr;
	btree->free_size = desc.free_size;
	btree->fixed_len = desc.fixed_len;

	/*
	 * XXX
	 * This is the wrong place to do this -- need to think about how
	 * to update open/configuration information in a reasonable way.
	 */
	if (btree->fixed_len != 0)
		F_SET(btree, WT_COLUMN);

	return (0);
}

/*
 * __wt_desc_write --
 *	Update the description page.
 */
int
__wt_desc_write(SESSION *session)
{
	BTREE *btree;
	WT_PAGE_DESC desc;
	int ret;

	btree = session->btree;
	ret = 0;

	WT_CLEAR(desc);
	desc.magic = WT_BTREE_MAGIC;
	desc.majorv = WT_BTREE_MAJOR_VERSION;
	desc.minorv = WT_BTREE_MINOR_VERSION;
	desc.intlmax = btree->intlmax;
	desc.intlmin = btree->intlmin;
	desc.leafmax = btree->leafmax;
	desc.leafmin = btree->leafmin;
	desc.recno_offset = 0;
	desc.root_addr = btree->root_page.addr;
	desc.root_size = btree->root_page.size;
	desc.free_addr = btree->free_addr;
	desc.free_size = btree->free_size;
	desc.fixed_len = (uint8_t)btree->fixed_len;
	desc.flags = 0;
	if (F_ISSET(btree, WT_RLE))
		F_SET(&desc, WT_PAGE_DESC_RLE);

	WT_RET(__wt_desc_io(session, &desc, 0));

	return (ret);
}

/*
 * __wt_desc_io --
 *	Read/write the WT_DESC sector.
 */
static int
__wt_desc_io(SESSION *session, void *p, int is_read)
{
	WT_FH *fh;

	fh = session->btree->fh;

	return (is_read ?
	    __wt_read(session, fh, (off_t)0, 512, p) :
	    __wt_write(session, fh, (off_t)0, 512, p));
}
