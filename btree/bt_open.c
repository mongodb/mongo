/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_open_verify(DB *);
static int __wt_open_verify_page_sizes(DB *);

/*
 * __wt_bt_open --
 *	Open a Btree.
 */
int
__wt_bt_open(WT_TOC *toc, int ok_create)
{
	DB *db;
	ENV *env;
	IDB *idb;

	db = toc->db;
	env = toc->env;
	idb = db->idb;

	/* Check page size configuration. */
	WT_RET(__wt_open_verify(db));

	/* Open the fle. */
	WT_RET(__wt_open(env, idb->name, idb->mode, ok_create, &idb->fh));

	/*
	 * If the file size is 0, write a description page; if the file size
	 * is non-zero, update the DB handle based on the on-disk description
	 * page.  (If the file isn't empty, there must be a description page.)
	 */
	if (idb->fh->file_size == 0)
		WT_RET(__wt_desc_write(toc));
	else {
		WT_RET(__wt_desc_read(toc));

		/* If there's a root page, pin it. */
		if (idb->root_off.addr != WT_ADDR_INVALID)
			WT_RET(__wt_root_pin(toc));
	}

	return (0);
}

/*
 * __wt_open_verify --
 *	Verify anything we can't verify before we're about to open the file;
 *	set defaults as necessary.
 */
static int
__wt_open_verify(DB *db)
{
	IDB *idb;

	idb = db->idb;

	/* Verify the page sizes. */
	WT_RET(__wt_open_verify_page_sizes(db));

	/* Verify other configuration combinations. */
	if (db->fixed_len != 0 && (idb->huffman_key || idb->huffman_data)) {
		__wt_api_db_errx(db,
		    "Fixed size column-store databases may not be Huffman "
		    "compressed");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_open_verify_page_sizes --
 *	Verify the page sizes.
 */
static int
__wt_open_verify_page_sizes(DB *db)
{
	IDB *idb;

	idb = db->idb;

	/*
	 * The application can set lots of page sizes.  It's complicated, so
	 * instead of verifying the relationships when they're set, verify
	 * then when the database is opened and we know we have the final
	 * values.  (Besides, if we verify the relationships when they're set,
	 * the application has to set them in a specific order or we'd need
	 * one set function that took 10 parameters.)
	 *
	 * If the values haven't been set, set the defaults.
	 *
	 * Default to a small fragment size, so overflow items don't consume
	 * a lot of space.
	 */
	if (db->allocsize == 0)
		db->allocsize = WT_BTREE_ALLOCATION_SIZE;

	/* Allocation sizes must be a power-of-two, nothing else makes sense. */
	if (!__wt_ispo2(db->allocsize)) {
		__wt_api_db_errx(db,
		   "the allocation size must be a power of two");
		return (WT_ERROR);
	}

	/*
	 * Limit allocation units to 256MB, and page sizes to 128MB.  There's
	 * no reason (other than testing) we can't support larger sizes (any
	 * sizes up to the smaller of an off_t and a size_t should work), but
	 * an application specifying larger allocation or page sizes is almost
	 * certainly making a mistake.
	 */
	if (db->allocsize > WT_BTREE_ALLOCATION_SIZE_MAX) {
		__wt_api_db_errx(db,
		   "the allocation size must less than or equal to %luMB",
		    (u_long)(WT_BTREE_PAGE_SIZE_MAX / WT_MEGABYTE));
		return (WT_ERROR);
	}

	/*
	 * Internal pages are also usually small, we want it to fit into the
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
	if (db->intlmin == 0)
		db->intlmin = WT_BTREE_INTLMIN_DEFAULT;
	if (db->intlmax == 0)
		db->intlmax = WT_MAX(db->intlmin, WT_BTREE_INTLMAX_DEFAULT);
	if (db->intlitemsize == 0) {
		if (db->intlmin <= 1024)
			db->intlitemsize = 50;
		else
			db->intlitemsize = db->intlmin / 40;
	}

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
	if (db->leafmin == 0)
		db->leafmin = WT_BTREE_LEAFMIN_DEFAULT;
	if (db->leafmax == 0)
		db->leafmax = WT_MAX(db->leafmin, WT_BTREE_LEAFMAX_DEFAULT);
	if (db->leafitemsize == 0) {
		if (db->leafmin <= 4096)
			db->leafitemsize = 80;
		else
			db->leafitemsize = db->leafmin / 40;
	}

	/* Final checks for safety. */
	if (db->intlmin % db->allocsize != 0 ||
	    db->intlmax % db->allocsize != 0 ||
	    db->leafmin % db->allocsize != 0 ||
	    db->leafmax % db->allocsize != 0) {
		__wt_api_db_errx(db,
		    "all page sizes must be a multiple of %lu bytes",
		    (u_long)db->allocsize);
		return (WT_ERROR);
	}

	if (db->intlmin > db->intlmax || db->leafmin > db->leafmax) {
		__wt_api_db_errx(db,
		    "minimum page sizes must be less than or equal to maximum "
		    "page sizes");
		return (WT_ERROR);
	}

	if (db->intlmin > WT_BTREE_PAGE_SIZE_MAX ||
	    db->intlmax > WT_BTREE_PAGE_SIZE_MAX ||
	    db->leafmin > WT_BTREE_PAGE_SIZE_MAX ||
	    db->leafmax > WT_BTREE_PAGE_SIZE_MAX) {
		__wt_api_db_errx(db,
		    "all page sizes must less than or equal to %luMB",
		    (u_long)WT_BTREE_PAGE_SIZE_MAX / WT_MEGABYTE);
		return (WT_ERROR);
	}

	/*
	 * We only have 3 bytes of length for on-page items, so the maximum
	 * on-page item size is limited to 16MB.
	 */
	if (db->intlitemsize > WT_ITEM_MAX_LEN)
		db->intlitemsize = WT_ITEM_MAX_LEN;
	if (db->leafitemsize > WT_ITEM_MAX_LEN)
		db->leafitemsize = WT_ITEM_MAX_LEN;

	/*
	 * By default, any duplicate set that reaches 25% of a leaf page is
	 * moved into its own separate tree.
	 */
	if (db->btree_dup_offpage == 0)
		db->btree_dup_offpage = 4;

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
	    (((s) - (WT_PAGE_DISK_SIZE + WT_PAGE_DESC_SIZE + 10)) / 4)
	if (db->intlitemsize > WT_MINIMUM_DATA_SPACE(db, db->intlmin)) {
		__wt_api_db_errx(db,
		    "The internal page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}
	if (db->leafitemsize > WT_MINIMUM_DATA_SPACE(db, db->leafmin)) {
		__wt_api_db_errx(db,
		    "The leaf page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}

	/*
	 * A fixed-size column store should be able to store at least 20
	 * objects on a page, otherwise it just doesn't make sense.
	 */
	if (F_ISSET(idb, WT_COLUMN) &&
	    db->fixed_len != 0 && db->leafmin / db->fixed_len < 20) {
		__wt_api_db_errx(db,
		    "The leaf page size cannot store at least 20 fixed-length "
		    "objects");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_root_pin --
 *	Read in the root page and pin it into memory.
 */
int
__wt_root_pin(WT_TOC *toc)
{
	IDB *idb;

	idb = toc->db->idb;

	/* Get the root page. */
	WT_RET(__wt_page_in(toc, NULL, &idb->root_page, &idb->root_off, 0));
		F_SET(idb->root_page.page, WT_PINNED);
	__wt_hazard_clear(toc, idb->root_page.page);

	return (0);
}
