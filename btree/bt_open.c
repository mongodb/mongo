/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_bt_server_start(WT_TOC *, WT_PAGE *);
static int __wt_bt_vrfy_sizes(DB *);

/*
 * __wt_bt_open --
 *	Open a Btree.
 */
int
__wt_bt_open(WT_TOC *toc)
{
	DB *db;
	IDB *idb;
	IENV *ienv;
	int isleaf;

	db = toc->db;
	idb = db->idb;
	ienv = toc->env->ienv;

	/* Check page size configuration. */
	WT_RET(__wt_bt_vrfy_sizes(db));

	/* Open the fle. */
	WT_RET(__wt_open(toc, idb->dbname, idb->mode,
	    F_ISSET(idb, WT_CREATE) ? WT_CREATE : 0, &idb->fh));

	/* If the file is empty, we're done. */
	if (idb->fh->file_size == 0) {
		idb->root_addr = WT_ADDR_INVALID;
		return (0);
	}

	/*
	 * If the file exists, update the DB handle based on the information
	 * in the on-disk WT_PAGE_DESC structure.  (If the file is not empty,
	 * there had better be a description record.)  Then, read in the root
	 * page.
	 */
	WT_RET(__wt_bt_desc_read(toc));

	/*
	 * The isleaf value tells us how big a page to read.  If the tree has
	 * split, the root page is an internal page, otherwise it's a leaf page.
	 */
	isleaf = idb->root_addr == WT_ADDR_FIRST_PAGE ? 1 : 0;
	WT_RET(
	    __wt_bt_page_in(toc, idb->root_addr, isleaf, 1, &idb->root_page));

	/*
	 * If we're not configured for single threaded behavior and the root
	 * page isn't a leaf page, fork server threads to serve the database
	 * pages.
	 */
	if (!F_ISSET(ienv, WT_SINGLE_THREADED) && !isleaf)
		WT_RET(__wt_bt_server_start(toc, idb->root_page));

	return (0);
}

u_int __wt_sthread_count = 10;

/*
 * __wt_bt_server_start --
 *	Fork off server threads for the second-level subtrees.
 */
static int
__wt_bt_server_start(WT_TOC *toc, WT_PAGE *page)
{
	extern u_int __wt_cthread_count;
	ENV *env;
	IDB *idb;
	WT_INDX *ip;
	WT_SRVR *srvr;
	u_int32_t addr, cnt, first_addr, i, n, per_server;
	int isleaf, srvr_id;

	env = toc->env;
	idb = toc->db->idb;

	/* We start 10 servers for now; this needs to be tuneable. */
	idb->srvrq_entries = __wt_sthread_count;
	WT_RET(__wt_calloc(
	    NULL, idb->srvrq_entries, sizeof(WT_SRVR), &idb->srvrq));
	for (srvr_id = 0; srvr_id < (int)idb->srvrq_entries; ++srvr_id) {
		srvr = idb->srvrq + srvr_id;
		srvr->id = srvr_id + 1;
		srvr->env = env;
		WT_RET(__wt_calloc(env,
		    __wt_cthread_count, sizeof(WT_TOC_CACHELINE), &srvr->ops));
		WT_RET(__wt_cache_create(toc, &srvr->cache));
		WT_RET(__wt_stat_alloc_srvr_stats(env, &srvr->stats));
		WT_RET(__wt_thread_create(&srvr->tid, __wt_workq, srvr));
	}

	/*
	 * We're willing to pin up to 100 pages; this needs to be tuneable.
	 *
	 * If the tree is only two levels, or has more than 100 pages in
	 * the 2nd level, pin just the root page and walk the root page,
	 * allocating a server to each group of items.  If the tree has at
	 * least 3 levels and less than 100 pages in the 2nd level, pin
	 * the entire 2nd level: walk the 2nd level, allocating a server to
	 * each group of items.
	 *
	 * !!!
	 * This is a rough, first-cut -- change this to pin 100 pages (or
	 * some tuneable number of pages) all the time.  It's possible to
	 * pin some subset of the 2nd level of the tree, it doesn't have
	 * to be a pin of an entire level, regardless.
	 */
	__wt_bt_first_offp(page, &addr, &isleaf);

	if (isleaf || page->indx_count > 100) {
		srvr_id = 1;
		n = 0;
		per_server = page->indx_count / idb->srvrq_entries;

		if (FLD_ISSET(env->verbose, WT_VERB_SERVERS))
			__wt_env_errx(env,
			    "database %s: pinning level #1 (%lu root entries, "
			    "%lu per server)", idb->dbname,
			    (u_long)page->indx_count, (u_long)per_server);

		WT_INDX_FOREACH(page, ip, i) {
			if (srvr_id <
			    (int)idb->srvrq_entries && ++n > per_server) {
				n = 0;
				++srvr_id;
			}
			ip->srvr_id = srvr_id;
		}
	} else {
		/* Walk the 2nd level of the tree, counting up items. */
		for (cnt = 0, first_addr = addr;
		    addr != WT_ADDR_INVALID; addr = page->hdr->nextaddr) {
			WT_RET(__wt_bt_page_in(toc, addr, 0, 1, &page));
			cnt += page->indx_count;
		}

		/* Walk the 2nd level of the tree, assign items to servers. */
		srvr_id = 1;
		n = 0;
		per_server = cnt / idb->srvrq_entries;

		if (FLD_ISSET(env->verbose, WT_VERB_SERVERS))
			__wt_env_errx(env,
			    "database %s: pinning level #2 (%lu root entries, "
			    "%lu per server)", idb->dbname,
			    (u_long)page->indx_count, (u_long)per_server);

		for (addr = first_addr;
		    addr != WT_ADDR_INVALID; addr = page->hdr->nextaddr) {
			WT_RET(__wt_bt_page_in(toc, addr, 0, 1, &page));

			WT_INDX_FOREACH(page, ip, i) {
				if (srvr_id < (int)idb->srvrq_entries &&
				    ++n > per_server) {
					n = 0;
					++srvr_id;
				}
				ip->srvr_id = srvr_id;
			}
		}
	}

	/*
	 * The DB cache is never unpinned, don't even look at it if there's
	 * a question of discarding memory.
	 */
	F_SET(toc->cache, WT_READONLY);

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
	 * Limit allocation and page sizes to 128MB.  There isn't a reason
	 * (other than testing) we can't support larger sizes (any size up
	 * to the smaller of an off_t and a size_t), but an application
	 * specifying allocation or page sizes larger than 128MB is almost
	 * certainly making a mistake.
	 */
#define	WT_UNEXPECTED(s)	((s) > 128 * WT_MEGABYTE)

	/*
	 * If the values haven't been set, set the defaults.
	 *
	 * Default to a small fragment size, so overflow items don't consume
	 * a lot of space.
	 */
	if (db->allocsize == 0)
		db->allocsize = 512;
	else if (WT_UNEXPECTED(db->allocsize))
		goto unexpected;

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
	else if (WT_UNEXPECTED(db->intlsize))
		goto unexpected;
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
	else if (WT_UNEXPECTED(db->leafsize)) {
unexpected:	__wt_db_errx(db,
		    "Allocation and page sizes are limited to 128MB");
		return (WT_ERROR);
	}
	if (db->leafitemsize == 0)
		if (db->leafsize <= 4096)
			db->leafitemsize = 80;
		else
			db->leafitemsize = db->leafsize / 40;

	/*
	 * We only have 3 bytes of length for on-page items, so the maximum
	 * on-page item size is limited to 16MB.
	 */
	if (db->intlitemsize > WT_ITEM_MAX_LEN)
		db->intlitemsize = WT_ITEM_MAX_LEN;
	if (db->leafitemsize > WT_ITEM_MAX_LEN)
		db->leafitemsize = WT_ITEM_MAX_LEN;

	/* Extents are 10MB by default. */
	if (db->extsize == 0)
		db->extsize = WT_MEGABYTE;

	/*
	 * By default, any duplicate set that reaches 25% of a leaf page is
	 * moved into its own separate tree.
	 */
	if (db->btree_dup_offpage == 0)
		db->btree_dup_offpage = 4;

	/* Check everything for safety. */
	if (db->allocsize < 512 || db->allocsize % 512 != 0) {
		__wt_db_errx(db,
		    "The fragment size must be a multiple of 512B");
		return (WT_ERROR);
	}
	if (db->intlsize % db->allocsize != 0 ||
	    db->leafsize % db->allocsize != 0 ||
	    db->extsize % db->allocsize != 0) {
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
	    (((s) - (WT_PAGE_HDR_SIZE + WT_PAGE_DESC_SIZE + 10)) / 4)
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
