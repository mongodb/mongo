/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_vrfy_sizes(DB *);

/*
 * __wt_bt_open --
 *	Open a Btree.
 */
int
__wt_bt_open(DB *db)
{
	IDB *idb;
	int ret;

	idb = db->idb;

	/* Check page size configuration. */
	if ((ret = __wt_bt_vrfy_sizes(db)) != 0)
		return (ret);

	/* Open the underlying database file. */
	if ((ret = __wt_cache_db_open(db)) != 0)
		return (ret);

	/*
	 * If the file exsists, update the DB handle based on the information
	 * in the on-disk WT_PAGE_DESC structure.  (If the number of frags in
	 * the file is non-zero, there had better be a description record.)
	 */
	if (idb->frags != 0) {
		if ((ret = __wt_bt_desc_read(db)) != 0)
			return (ret);
	} else
		idb->root_addr = WT_ADDR_INVALID;

	return (0);
}

/*
 * __wt_bt_vrfy_sizes --
 *	Verify any configured sizes, and set the defaults.
 */
static int
__wt_bt_vrfy_sizes(DB *db)
{
	/*
	 * The application can set lots of sizes.  It's complicated enough
	 * that instead of verifying them when they're set, we verify them
	 * when the database is opened and we know we have the final values.
	 * (Besides, if we verify them when they're set, the application
	 * has to set them in a specific order or we'd have to have one set
	 * function that took 10 parameters.)
	 *
	 * If the values haven't been set, set the defaults.
	 */

	/*
	 * Default to a small fragment size, so overflow items don't consume
	 * a lot of space.  
	 */
	if (db->fragsize == 0)
		db->fragsize = 512;

	/*
	 * Internal pages are also fairly small, we want it to fit into the
	 * L1 cache.   We try and put at least 40 keys on each internal page
	 * (40 because that results in 100M keys in a level 5 Btree).  But,
	 * if it's a small page, push anything bigger than about 50 bytes
	 * off-page.   Here's the table:
	 *	Pagesize	Largest key retained on-page:
	 *	512B		 50 bytes
	 *	1K		 50 bytes
	 *	2K		 51 bytes
	 *	4K		102 bytes
	 *	8K		204 bytes
	 * and so on, roughly doubling for each power-of-two.
	 */
	if (db->intlsize == 0)
		db->intlsize = 8 * 1024;
	if (db->intlitemsize == 0)
		if (db->intlsize <= 1024)
			db->intlitemsize = 50;
		else
			db->intlitemsize = db->intlsize / 40;

	/*
	 * Leaf pages are larger to amortize I/O across a large chunk of the
	 * data space, but still minimize the chance of a broken write.  We
	 * only require 20 key/data pairs fit onto a leaf page.  Again, if it's
	 * a small page, push anything bigger than about 80 bytes off-page.
	 * Here's the table:
	 *	Pagesize	Largest key or data item retained on-page:
	 *	512B		 80 bytes
	 *	 1K		 80 bytes
	 *	 2K		 80 bytes
	 *	 4K		 80 bytes
	 *	 8K		204 bytes
	 *	16K		409 bytes
	 * and so on, roughly doubling for each power-of-two.
	 */
	if (db->leafsize == 0)
		db->leafsize = 32 * 1024;
	if (db->leafitemsize == 0)
		if (db->leafsize <= 4096)
			db->leafitemsize = 80;
		else
			db->leafitemsize = db->leafsize / 40;

	/* Extents are 10MB by default. */
	if (db->extsize == 0)
		db->extsize = MEGABYTE;

	/*
	 * By default, any duplicate set that reaches 25% of a leaf page is
	 * moved into its own separate tree.
	 */
	if (db->btree_dup_offpage == 0)
		db->btree_dup_offpage = 4;

	/* Check everything for safety. */
	if (db->fragsize < 512 || db->fragsize % 512 != 0) {
		__wt_db_errx(db,
		    "The fragment size must be a multiple of 512B");
		return (WT_ERROR);
	}
	if (db->intlsize % db->fragsize != 0 ||
	    db->leafsize % db->fragsize != 0 ||
	    db->extsize % db->fragsize != 0) {
		__wt_db_errx(db,
		    "The internal, leaf and extent sizes must be a multiple "
		    "of the fragment size");
		return (WT_ERROR);
	}

	/*
	 * A leaf page must hold at least 2 key/data pairs, otherwise the
	 * whole btree thing breaks down because we can't split.  We have
	 * to include WT_DESC_SIZE in leaf page calculations, it's not
	 * strictly necessary in internal pages because page 0 is always
	 * a leaf page.  The additional 10 bytes is for slop -- Berkeley DB
	 * took roughly a decade to get the calculation correct, and that
	 * way I can skip the suspense.
	 */
#define	WT_MINIMUM_DATA_SPACE(db, s)					\
	    (((s) - (WT_HDR_SIZE + WT_DESC_SIZE + 10)) / 4)
	if (db->intlitemsize > WT_MINIMUM_DATA_SPACE(db, db->intlsize)) {
		__wt_db_errx(db,
		    "The internal page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}
	if (db->leafitemsize > WT_MINIMUM_DATA_SPACE(db, db->leafsize)) {
		__wt_db_errx(db,
		    "The leaf page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}

	return (0);
}
