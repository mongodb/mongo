/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __wt_open_verify(SESSION *, BTREE *);
static int __wt_open_verify_page_sizes(SESSION *, BTREE *);

/*
 * __wt_bt_open --
 *	Open a Btree.
 */
int
__wt_bt_open(SESSION *session, int ok_create)
{
	BTREE *btree;

	btree = session->btree;

	/* Check page size configuration. */
	WT_RET(__wt_open_verify(session, btree));

	/* Open the fle. */
	WT_RET(__wt_open(session,
	    btree->name, btree->mode, ok_create, &btree->fh));

	/*
	 * If the file size is 0, write a description page; if the file size
	 * is non-zero, update the BTREE handle based on the on-disk description
	 * page.  (If the file isn't empty, there must be a description page.)
	 */
	if (btree->fh->file_size == 0)
		WT_RET(__wt_desc_write(session));
	else {
		WT_RET(__wt_desc_read(session));

		/* If there's a root page, pin it. */
		if (btree->root_page.addr != WT_ADDR_INVALID)
			WT_RET(__wt_root_pin(session));
	}

	/* Read the free-list into memory. */
	WT_RET(__wt_block_read(session));

	return (0);
}

/*
 * __wt_open_verify --
 *	Verify anything we can't verify before we're about to open the file;
 *	set defaults as necessary.
 */
static int
__wt_open_verify(SESSION *session, BTREE *btree)
{
	/* Verify the page sizes. */
	WT_RET(__wt_open_verify_page_sizes(session, btree));

	/* Verify other configuration combinations. */
	if (btree->fixed_len != 0 &&
	    (btree->huffman_key || btree->huffman_data)) {
		__wt_err(session, 0,
		    "Fixed-size column-store files may not be Huffman encoded");
		return (WT_ERROR);
	}

	return (0);
}

/*
 * __wt_open_verify_page_sizes --
 *	Verify the page sizes.
 */
static int
__wt_open_verify_page_sizes(SESSION *session, BTREE *btree)
{
	/*
	 * The application can set lots of page sizes.  It's complicated, so
	 * instead of verifying the relationships when they're set, verify
	 * then when the file is opened and we know we have the final values.
	 *  (Besides, if we verify the relationships when they're set, the
	 * application has to set them in a specific order or we'd need one
	 * set function that took 10 parameters.)
	 *
	 * If the values haven't been set, set the defaults.
	 *
	 * Default to a small fragment size, so overflow items don't consume
	 * a lot of space.
	 */
	if (btree->allocsize == 0)
		btree->allocsize = WT_BTREE_ALLOCATION_SIZE_MIN;

	/* Allocation sizes must be a power-of-two, nothing else makes sense. */
	if (!__wt_ispo2(btree->allocsize)) {
		__wt_err(session, 0,
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
	if (btree->allocsize > WT_BTREE_ALLOCATION_SIZE_MAX) {
		__wt_err(session, 0,
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
	if (btree->intlmin == 0)
		btree->intlmin = WT_BTREE_INTLMIN_DEFAULT;
	if (btree->intlmax == 0)
		btree->intlmax =
		    WT_MAX(btree->intlmin, WT_BTREE_INTLMAX_DEFAULT);
	if (btree->intlitemsize == 0) {
		if (btree->intlmin <= 1024)
			btree->intlitemsize = 50;
		else
			btree->intlitemsize = btree->intlmin / 40;
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
	if (btree->leafmin == 0)
		btree->leafmin = WT_BTREE_LEAFMIN_DEFAULT;
	if (btree->leafmax == 0)
		btree->leafmax =
		    WT_MAX(btree->leafmin, WT_BTREE_LEAFMAX_DEFAULT);
	if (btree->leafitemsize == 0) {
		if (btree->leafmin <= 4096)
			btree->leafitemsize = 80;
		else
			btree->leafitemsize = btree->leafmin / 40;
	}

	/* Final checks for safety. */
	if (btree->intlmin % btree->allocsize != 0 ||
	    btree->intlmax % btree->allocsize != 0 ||
	    btree->leafmin % btree->allocsize != 0 ||
	    btree->leafmax % btree->allocsize != 0) {
		__wt_err(session, 0,
		    "all page sizes must be a multiple of %lu bytes",
		    (u_long)btree->allocsize);
		return (WT_ERROR);
	}

	if (btree->intlmin > btree->intlmax ||
	    btree->leafmin > btree->leafmax) {
		__wt_err(session, 0,
		    "minimum page sizes must be less than or equal to maximum "
		    "page sizes");
		return (WT_ERROR);
	}

	if (btree->intlmin > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->intlmax > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->leafmin > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->leafmax > WT_BTREE_PAGE_SIZE_MAX) {
		__wt_err(session, 0,
		    "all page sizes must less than or equal to %luMB",
		    (u_long)WT_BTREE_PAGE_SIZE_MAX / WT_MEGABYTE);
		return (WT_ERROR);
	}

	/*
	 * We only have 3 bytes of length for on-page items, so the maximum
	 * on-page item size is limited to 16MB.
	 */
	if (btree->intlitemsize > WT_CELL_MAX_LEN)
		btree->intlitemsize = WT_CELL_MAX_LEN;
	if (btree->leafitemsize > WT_CELL_MAX_LEN)
		btree->leafitemsize = WT_CELL_MAX_LEN;

	/*
	 * A leaf page must hold at least 2 key/data pairs, otherwise the
	 * whole btree thing breaks down because we can't split.  We have
	 * to include WT_DESC_SIZE in leaf page calculations, it's not
	 * strictly necessary in internal pages because page 0 is always
	 * a leaf page.  The additional 10 bytes is for slop -- Berkeley DB
	 * took roughly a decade to get the calculation correct, and that
	 * way I can skip the suspense.
	 */
#define	WT_MINIMUM_DATA_SPACE(btree, s)					\
	    (((s) - (WT_PAGE_DISK_SIZE + WT_PAGE_DESC_SIZE + 10)) / 4)
	if (btree->intlitemsize >
	    WT_MINIMUM_DATA_SPACE(btree, btree->intlmin)) {
		__wt_err(session, 0,
		    "The internal page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}
	if (btree->leafitemsize >
	    WT_MINIMUM_DATA_SPACE(btree, btree->leafmin)) {
		__wt_err(session, 0,
		    "The leaf page size is too small for its maximum item "
		    "size");
		return (WT_ERROR);
	}

	/*
	 * A fixed-size column-store should be able to store at least 20
	 * objects on a page, otherwise it just doesn't make sense.
	 */
	if (F_ISSET(btree, WT_COLUMN) &&
	    btree->fixed_len != 0 && btree->leafmin / btree->fixed_len < 20) {
		__wt_err(session, 0,
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
__wt_root_pin(SESSION *session)
{
	BTREE *btree;

	btree = session->btree;

	/* Get the root page, which had better be there. */
	WT_RET(__wt_page_in(session, NULL, &btree->root_page, 0));

	WT_PAGE_SET_PIN(btree->root_page.page);
	__wt_hazard_clear(session, btree->root_page.page);

	return (0);
}
