/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __btree_conf(WT_SESSION_IMPL *);
static int __btree_init(WT_SESSION_IMPL *, const char *);
static int __btree_page_sizes(WT_SESSION_IMPL *);
static int __btree_type(WT_SESSION_IMPL *);

/*
 * __wt_btree_create --
 *	Create a Btree.
 */
int
__wt_btree_create(WT_SESSION_IMPL *session, const char *name)
{
	WT_FH *fh;
	int ret;

	/* Check to see if the file exists -- we don't want to overwrite it. */
	if (__wt_exist(name)) {
		__wt_errx(session,
		    "the file %s already exists; to re-create it, remove it "
		    "first, then create it",
		    name);
		return (WT_ERROR);
	}

	/* Open the underlying file handle. */
	WT_RET(__wt_open(session, name, 0666, 1, &fh));

	/* Write out the file's meta-data. */
	ret = __wt_desc_write(session, fh);

	/* Close the file handle. */
	WT_TRET(__wt_close(session, fh));

	return (ret);
}

/*
 * __wt_btree_open --
 *	Open a Btree.
 *
 *	Note that the config string must point to allocated memory: it will
 *	be stored in the returned btree handle and freed when the handle is
 *	closed.
 */
int
__wt_btree_open(WT_SESSION_IMPL *session,
    const char *name, const char *config, uint32_t flags)
{
	WT_CONNECTION_IMPL *conn;
	WT_BTREE *btree;
	WT_PAGE *page;

	conn = S2C(session);
	btree = session->btree;

	/* Create the WT_BTREE structure. */
	WT_RET(__wt_calloc_def(session, 1, &btree));
	btree->flags = flags;
	btree->conn = conn;
	session->btree = btree;

	/* Use the config string: it will be freed when the btree handle. */
	btree->config = config;

	/* Initialize the WT_BTREE structure. */
	WT_RET(__btree_init(session, name));

	/* Open the underlying file handle. */
	WT_RET(__wt_open(session, name, 0666, 1, &btree->fh));

	/*
	 * Read in the file's metadata, configure the WT_BTREE structure based
	 * on the configuration string, read in the free-list.
	 *
	 * XXX Take extra care with this if WT_BTREE_VERIFY is set?
	 */
	WT_RET(__wt_desc_read(session));
	WT_RET(__btree_conf(session));
	WT_RET(__wt_block_read(session));

	/*
	 * If there's no root page, create an in-memory page but leave it clean;
	 * if it's never written, that's OK.  If there is a root page, read it
	 * in and pin it.
	 */
	if (btree->root_page.addr == WT_ADDR_INVALID) {
		WT_RET(__wt_calloc_def(session, 1, &page));
		switch (btree->type) {
		case BTREE_COL_FIX:
			page->u.col_leaf.recno = 1;
			page->type = WT_PAGE_COL_FIX;
			break;
		case BTREE_COL_RLE:
			page->u.col_leaf.recno = 1;
			page->type = WT_PAGE_COL_RLE;
			break;
		case BTREE_COL_VAR:
			page->u.col_leaf.recno = 1;
			page->type = WT_PAGE_COL_VAR;
			break;
		case BTREE_ROW:
			page->type = WT_PAGE_ROW_LEAF;
			break;
		}
		page->parent = NULL;
		page->parent_ref = &btree->root_page;
		F_SET(page, WT_PAGE_INITIAL_EMPTY);

		btree->root_page.state = WT_REF_MEM;
		btree->root_page.addr = WT_ADDR_INVALID;
		btree->root_page.size = 0;
		btree->root_page.page = page;
	} else  {
		WT_RET(__wt_page_in(session, NULL, &btree->root_page, 0));
		F_SET(btree->root_page.page, WT_PAGE_PINNED);
		__wt_hazard_clear(session, btree->root_page.page);
	}

	/* Add to the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_INSERT_TAIL(&conn->dbqh, btree, q);
	++conn->dbqcnt;
	__wt_unlock(session, conn->mtx);

	WT_STAT_INCR(conn->stats, file_open);

	return (0);
}

/*
 * __btree_init --
 *	Initialize the WT_BTREE structure, after an zero-filled allocation.
 */
static int
__btree_init(WT_SESSION_IMPL *session, const char *name)
{
	WT_BTREE *btree;

	btree = session->btree;

	WT_RET(__wt_strdup(session, name, &btree->name));

	btree->root_page.addr = WT_ADDR_INVALID;

	TAILQ_INIT(&btree->freeqa);
	TAILQ_INIT(&btree->freeqs);

	btree->free_addr = WT_ADDR_INVALID;

	btree->btree_compare = __wt_bt_lex_compare;

	WT_RET(__wt_stat_alloc_btree_stats(session, &btree->stats));
	WT_RET(__wt_stat_alloc_btree_file_stats(session, &btree->fstats));

	return (0);
}

/*
 * __wt_btree_close --
 *	Close a Btree.
 */
int
__wt_btree_close(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	int ret;

	btree = session->btree;
	conn = btree->conn;
	ret = 0;

	/* Remove from the connection's list. */
	__wt_lock(session, conn->mtx);
	TAILQ_REMOVE(&conn->dbqh, btree, q);
	--conn->dbqcnt;
	__wt_unlock(session, conn->mtx);

	/*
	 * If it's a normal tree, ask the eviction thread to flush any pages
	 * that remain in the cache.
	 */
	if (!F_ISSET(btree, WT_BTREE_NO_EVICTION))
		__wt_evict_file_serial(session, 1, ret);

	/* Write out the free list. */
	WT_TRET(__wt_block_write(session));

	/* Write out the file's metadata. */
	WT_TRET(__wt_desc_update(session));

	/* Close the underlying file handle. */
	WT_TRET(__wt_close(session, btree->fh));

	__wt_free(session, btree->config);
	__wt_free(session, btree->name);

	__wt_btree_huffman_close(session);

	__wt_buf_free(session, &btree->key_srch);

	__wt_walk_end(session, &btree->evict_walk);

	__wt_free(session, btree->stats);
	__wt_free(session, btree->fstats);

	__wt_free(session, session->btree);

	return (ret);
}

/*
 * __btree_conf --
 *	Configure the btree and verify the configuration relationships.
 */
static int
__btree_conf(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;

	btree = session->btree;

	/* File type. */
	WT_RET(__btree_type(session));

	/* Page sizes. */
	WT_RET(__btree_page_sizes(session));

	/* Huffman encoding configuration. */
	WT_RET(__wt_btree_huffman_open(session));

	WT_RET(__wt_config_getones(session,
	    btree->config, "btree_key_gap", &cval));
	btree->key_gap = (uint32_t)cval.val;

	return (0);
}

/*
 * __btree_type --
 *	Figure out the database type.
 */
static int
__btree_type(WT_SESSION_IMPL *session)
{
	const char *config;
	char *endp, t;
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;

	btree = session->btree;
	config = btree->config;

	WT_RET(__wt_config_getones(session, config, "key_format", &cval));
	if (cval.len > 0 && cval.str[0] == 'r')
		btree->type = BTREE_COL_VAR;
	else
		btree->type = BTREE_ROW;

	/* Check for fixed-length data. */
	WT_RET(__wt_config_getones(session, config, "value_format", &cval));
	if (cval.len > 1 && ((t = cval.str[cval.len - 1]) == 'u' || t == 'S')) {
		btree->fixed_len = (uint32_t)strtol(cval.str, &endp, 10);
		if (endp == cval.str + cval.len - 1 && btree->fixed_len != 0)
			btree->type = BTREE_COL_FIX;
	}

	/* Check for run-length encoding */
	WT_RET(__wt_config_getones(session,
	    config, "runlength_encoding", &cval));
	if (cval.val != 0) {
		if (btree->type != BTREE_COL_FIX) {
			__wt_errx(session,
			    "Run-length encoding is incompatible with variable "
			    "length column-store records, you must specify a "
			    "fixed-length record");
			return (WT_ERROR);
		}
		btree->type = BTREE_COL_RLE;
	}
	return (0);
}

/*
 * __btree_page_sizes --
 *	Verify the page sizes.
 */
static int
__btree_page_sizes(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	const char *config;

	btree = session->btree;
	config = btree->config;

	WT_RET(__wt_config_getones(session, config, "allocation_size", &cval));
	btree->allocsize = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(session, config, "intl_node_max", &cval));
	btree->intlmax = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(session, config, "intl_node_min", &cval));
	btree->intlmin = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(session, config, "leaf_node_max", &cval));
	btree->leafmax = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(session, config, "leaf_node_min", &cval));
	btree->leafmin = (uint32_t)cval.val;

	/* Allocation sizes must be a power-of-two, nothing else makes sense. */
	if (!__wt_ispo2(btree->allocsize)) {
		__wt_errx(
		    session, "the allocation size must be a power of two");
		return (WT_ERROR);
	}

	/*
	 * Limit allocation units to 128MB, and page sizes to 512MB.  There's
	 * no reason (other than testing) we can't support larger sizes (any
	 * sizes up to the smaller of an off_t and a size_t should work), but
	 * an application specifying larger allocation or page sizes is almost
	 * certainly making a mistake.
	 */
	if (btree->allocsize < WT_BTREE_ALLOCATION_SIZE_MIN ||
	    btree->allocsize > WT_BTREE_ALLOCATION_SIZE_MAX) {
		__wt_errx(session,
		    "the allocation size must be at least %" PRIu32 "B "
		    "and no larger than %" PRIu32 "MB",
		    WT_BTREE_ALLOCATION_SIZE_MIN,
		    WT_BTREE_ALLOCATION_SIZE_MAX / WT_MEGABYTE);
		return (WT_ERROR);
	}

	/* All page sizes must be in units of the allocation size. */
	if (btree->intlmin < btree->allocsize ||
	    btree->intlmin % btree->allocsize != 0 ||
	    btree->intlmax < btree->allocsize ||
	    btree->intlmax % btree->allocsize != 0 ||
	    btree->leafmin < btree->allocsize ||
	    btree->leafmin % btree->allocsize != 0 ||
	    btree->leafmax < btree->allocsize ||
	    btree->leafmax % btree->allocsize != 0) {
		__wt_errx(session,
		    "all page sizes must be a multiple of the page allocation "
		    "size (%" PRIu32 "B)", btree->allocsize);
		return (WT_ERROR);
	}

	if (btree->intlmin > btree->intlmax ||
	    btree->leafmin > btree->leafmax) {
		__wt_errx(session,
		    "minimum page sizes must be less than or equal to maximum "
		    "page sizes");
		return (WT_ERROR);
	}

	if (btree->intlmin > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->intlmax > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->leafmin > WT_BTREE_PAGE_SIZE_MAX ||
	    btree->leafmax > WT_BTREE_PAGE_SIZE_MAX) {
		__wt_errx(session,
		    "page sizes may not be larger than %" PRIu32 "MB",
		    WT_BTREE_PAGE_SIZE_MAX / WT_MEGABYTE);
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
	btree->intlitemsize = btree->intlmin <= 1024 ? 50 : btree->intlmin / 40;

	/*
	 * Leaf pages are larger to amortize I/O across a large chunk of the
	 * data space.  We only require 20 key/data pairs fit onto a leaf page.
	 * Again, if it's a small page, push anything bigger than about 80
	 * bytes off-page.  Here's the table:
	 *	Pagesize	Largest key or data item retained on-page:
	 *	512B		 80 bytes
	 *	 1K		 80 bytes
	 *	 2K		 80 bytes
	 *	 4K		 80 bytes
	 *	 8K		204 bytes
	 *	16K		409 bytes
	 * and so on, roughly doubling for each power-of-two.
	 */
	btree->leafitemsize = btree->leafmin <= 4096 ? 80 : btree->leafmin / 20;

	/*
	 * A fixed-size column-store should be able to store at least 20
	 * objects on a page, otherwise it just doesn't make sense.
	 */
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_RLE:
		if (btree->leafmin / btree->fixed_len < 20) {
			__wt_errx(session,
			    "the configured leaf page size cannot store at "
			    "least 20 fixed-length objects");
			return (WT_ERROR);
		}
		break;
	case BTREE_COL_VAR:
	case BTREE_ROW:
		break;
	}

	return (0);
}
