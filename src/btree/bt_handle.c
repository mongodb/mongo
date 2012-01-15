/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int __btree_conf(
	WT_SESSION_IMPL *, const char *, const char *, const char *, uint32_t);
static int __btree_last(WT_SESSION_IMPL *);
static int __btree_page_sizes(WT_SESSION_IMPL *, const char *);
static int __btree_root_init_empty(WT_SESSION_IMPL *);
static int __btree_tree_init(WT_SESSION_IMPL *);

static int pse1(WT_SESSION_IMPL *, const char *, uint32_t, uint32_t);
static int pse2(WT_SESSION_IMPL *, const char *, uint32_t, uint32_t, uint32_t);

/*
 * __wt_btree_create --
 *	Create a Btree.
 */
int
__wt_btree_create(WT_SESSION_IMPL *session, const char *filename)
{
	return (__wt_bm_create(session, filename));
}

/*
 * __wt_btree_truncate --
 *	Truncate a Btree.
 */
int
__wt_btree_truncate(WT_SESSION_IMPL *session, const char *filename)
{
	return (__wt_bm_truncate(session, filename));
}

/*
 * __wt_btree_open --
 *	Open a Btree.
 */
int
__wt_btree_open(WT_SESSION_IMPL *session,
    const char *name, const char *filename,
    const char *config, const char *cfg[], uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONNECTION_IMPL *conn;
	int matched, ret;

	conn = S2C(session);

	/*
	 * The file configuration string must point to allocated memory: it
	 * is stored in the returned btree handle and freed when the handle
	 * is closed.
	 */

	/* Increment the reference count if we already have the btree open. */
	matched = 0;
	__wt_spin_lock(session, &conn->spinlock);
	TAILQ_FOREACH(btree, &conn->btqh, q) {
		if (strcmp(filename, btree->filename) == 0) {
			++btree->refcnt;
			session->btree = btree;
			matched = 1;
			break;
		}
	}
	if (matched) {
		__wt_spin_unlock(session, &conn->spinlock);

		/* Check that the handle is open. */
		__wt_readlock(session, btree->rwlock);
		matched = F_ISSET(btree, WT_BTREE_OPEN);
		__wt_rwunlock(session, btree->rwlock);

		if (!matched) {
			__wt_writelock(session, btree->rwlock);
			if (!F_ISSET(btree, WT_BTREE_OPEN)) {
				/* We're going to overwrite the old config. */
				__wt_free(session, btree->config);
				goto conf;
			}

			/* It was opened while we waited. */
			__wt_rwunlock(session, btree->rwlock);
		}

		/* The config string will not be needed: free it now. */
		__wt_free(session, config);
		return (0);
	}

	/* Allocate the WT_BTREE structure. */
	btree = NULL;
	if ((ret = __wt_calloc_def(session, 1, &btree)) == 0)
		ret = __wt_rwlock_alloc(
		     session, "btree handle", &btree->rwlock);
	if (ret == 0) {
		/* Lock the handle before it is inserted in the list. */
		__wt_writelock(session, btree->rwlock);

		/* Add to the connection list. */
		btree->refcnt = 1;
		TAILQ_INSERT_TAIL(&conn->btqh, btree, q);
		++conn->btqcnt;
	} else
		if (btree != NULL)
			__wt_free(session, btree);
	__wt_spin_unlock(session, &conn->spinlock);

	if (ret != 0)
		return (ret);

	session->btree = btree;

	/* Initialize and configure the WT_BTREE structure. */
conf:	WT_ERR(__btree_conf(session, name, filename, config, flags));

	/* Open the underlying block object. */
	WT_ERR(__wt_bm_open(session, btree->filename,
	    btree->config, cfg, F_ISSET(btree, WT_BTREE_SALVAGE) ? 1 : 0));
	WT_ERR(__wt_bm_block_header(session, &btree->block_header));

	/* Initialize the tree if not a special command. */
	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		WT_ERR(__btree_tree_init(session));

	F_SET(btree, WT_BTREE_OPEN);
	WT_STAT_INCR(conn->stats, file_open);

err:	__wt_rwunlock(session, btree->rwlock);
	if (ret != 0)
		(void)__wt_btree_close(session);
	return (ret);
}

/*
 * __wt_btree_reopen --
 *	Reset an open btree handle back to its initial state.
 */
int
__wt_btree_reopen(WT_SESSION_IMPL *session, uint32_t flags)
{
	WT_BTREE *btree;

	btree = session->btree;

	/*
	 * Clear any existing cache.  The reason for this is because verify and
	 * salvage don't want to deal with in-memory trees, that is, reads must
	 * be satisfied from the disk.
	 */
	if (btree->root_page != NULL)
		WT_RET(__wt_evict_file_serial(session, 1));
	WT_ASSERT(session, btree->root_page == NULL);

	/* After all pages are evicted, update the root's address. */
	if (btree->root_update) {
		/*
		 * Release the original blocks held by the root, that is,
		 * the blocks listed in the schema file.
		 */
		WT_RET(__wt_btree_free_root(session));

		WT_RET(__wt_btree_set_root(
		    session, btree->root_addr.addr, btree->root_addr.size));
		if (btree->root_addr.addr != NULL)
			__wt_free(session, btree->root_addr.addr);
		btree->root_update = 0;
	}

	btree->flags = flags;				/* XXX */

	/* Initialize the tree if not a special command. */
	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
		WT_RET(__btree_tree_init(session));

	F_SET(btree, WT_BTREE_OPEN);			/* XXX */
	return (0);
}

/*
 * __btree_conf --
 *	Configure a WT_BTREE structure.
 */
static int
__btree_conf(WT_SESSION_IMPL *session,
    const char *name, const char *filename, const char *config, uint32_t flags)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COLLATOR *ncoll;
	uint32_t bitcnt;
	int fixed;

	btree = session->btree;
	conn = S2C(session);

	/* Take copies of names for the new handle. */
	WT_RET(__wt_strdup(session, name, &btree->name));
	WT_RET(__wt_strdup(session, filename, &btree->filename));

	/* Validate file types and check the data format plan. */
	WT_RET(__wt_config_getones(session, config, "key_format", &cval));
	WT_RET(__wt_struct_check(session, cval.str, cval.len, NULL, NULL));
	if (cval.len > 0 && strncmp(cval.str, "r", cval.len) == 0)
		btree->type = BTREE_COL_VAR;
	else
		btree->type = BTREE_ROW;
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->key_format));

	WT_RET(__wt_config_getones(session, config, "value_format", &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->value_format));

	/* Row-store key comparison and key gap for prefix compression. */
	if (btree->type == BTREE_ROW) {
		WT_RET(__wt_config_getones(
		    session, config, "collator", &cval));
		if (cval.len > 0) {
			TAILQ_FOREACH(ncoll, &conn->collqh, q) {
				if (strncmp(
				    ncoll->name, cval.str, cval.len) == 0) {
					btree->collator = ncoll->collator;
					break;
				}
			}
			if (btree->collator == NULL)
				WT_RET_MSG(session, EINVAL,
				    "unknown collator '%.*s'",
				    (int)cval.len, cval.str);
		}
		WT_RET(__wt_config_getones(session, config, "key_gap", &cval));
		btree->key_gap = (uint32_t)cval.val;
	}
	/* Check for fixed-size data. */
	if (btree->type == BTREE_COL_VAR) {
		WT_RET(__wt_struct_check(
		    session, cval.str, cval.len, &fixed, &bitcnt));
		if (fixed) {
			if (bitcnt == 0 || bitcnt > 8)
				WT_RET_MSG(session, EINVAL,
				    "fixed-width field sizes must be greater "
				    "than 0 and less than or equal to 8");
			btree->bitcnt = (uint8_t)bitcnt;
			btree->type = BTREE_COL_FIX;
		}
	}

	/* Page sizes */
	WT_RET(__btree_page_sizes(session, config));

	/* Huffman encoding */
	WT_RET(__wt_btree_huffman_open(session, config));

	WT_RET(__wt_stat_alloc_btree_stats(session, &btree->stats));

	/* Take the config string: it will be freed with the btree handle. */
	btree->config = config;

	/* Set the flags. */
	btree->flags = flags;

	return (0);
}

/*
 * __btree_tree_init --
 *	Open the file in the block manager and read the root/last pages.
 */
static int
__btree_tree_init(WT_SESSION_IMPL *session)
{
	WT_BUF *addr;
	int ret;

	ret = 0;

	WT_RET(__wt_scr_alloc(session, 0, &addr));
	WT_ERR(__wt_btree_get_root(session, addr));

	/*
	 * If there's a root page in the file, read it in and pin it.
	 * If there's no root page, create an empty in-memory page.
	 */
	if (addr->data == NULL)
		WT_ERR(__btree_root_init_empty(session));
	else
		WT_ERR(__wt_btree_root_init(session, addr));

	/* Get the last page of the file. */
	WT_ERR(__btree_last(session));

err:	__wt_scr_free(&addr);

	return (ret);
}

/*
 * __wt_btree_root_init --
 *      Read in a tree from disk.
 */
int
__wt_btree_root_init(WT_SESSION_IMPL *session, WT_BUF *addr)
{
	WT_BTREE *btree;
	WT_BUF tmp;
	WT_PAGE *page;
	int ret;

	btree = session->btree;

	/* Read the root into memory. */
	WT_CLEAR(tmp);
	WT_RET(__wt_bm_read(session, &tmp, addr->data, addr->size));

	/* Build the in-memory version of the page. */
	WT_ERR(__wt_page_inmem(session, NULL, NULL, tmp.mem, &page));

	/* This page can never leave memory. */
	F_SET(page, WT_PAGE_PINNED);

	btree->root_page = page;
	return (0);

err:	__wt_buf_free(session, &tmp);
	return (ret);
}

/*
 * __btree_root_init_empty --
 *      Create an empty in-memory tree.
 */
static int
__btree_root_init_empty(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *root, *leaf;
	WT_REF *ref;
	int ret;

	btree = session->btree;
	root = leaf = NULL;
	ret = 0;

	/*
	 * Create a leaf page -- this can be reconciled while the root stays
	 * pinned.
	 */
	WT_ERR(__wt_calloc_def(session, 1, &leaf));
	switch (btree->type) {
	case BTREE_COL_FIX:
		leaf->u.col_fix.recno = 1;
		leaf->type = WT_PAGE_COL_FIX;
		break;
	case BTREE_COL_VAR:
		leaf->u.col_var.recno = 1;
		leaf->type = WT_PAGE_COL_VAR;
		break;
	case BTREE_ROW:
		leaf->type = WT_PAGE_ROW_LEAF;
		break;
	}
	leaf->entries = 0;

	/*
	 * A note about empty trees: the initial tree is a root page and a leaf
	 * page, neither of which are marked dirty.   If evicted without being
	 * modified, that's OK, nothing will ever be written.
	 *
	 * Create the empty root page.
	 *
	 * !!!
	 * Be cautious about changing the order of updates in this code: to call
	 * __wt_page_out on error, we require a correct page setup at each point
	 * where we might fail.
	 */
	WT_ERR(__wt_calloc_def(session, 1, &root));
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		root->type = WT_PAGE_COL_INT;
		root->u.intl.recno = 1;
		WT_ERR(__wt_calloc_def(session, 1, &root->u.intl.t));
		ref = root->u.intl.t;
		ref->page = leaf;
		ref->addr = NULL;
		ref->state = WT_REF_MEM;
		ref->u.recno = 1;
		break;
	case BTREE_ROW:
		root->type = WT_PAGE_ROW_INT;
		WT_ERR(__wt_calloc_def(session, 1, &root->u.intl.t));
		ref = root->u.intl.t;
		ref->page = leaf;
		ref->addr = NULL;
		ref->state = WT_REF_MEM;
		WT_ERR(__wt_row_ikey_alloc(
		    session, 0, "", 1, (WT_IKEY **)&(ref->u.key)));
		break;
	WT_ILLEGAL_VALUE(session);
	}
	root->entries = 1;
	root->parent = NULL;
	root->ref = NULL;
	F_SET(root, WT_PAGE_PINNED);

	leaf->ref = ref;
	leaf->parent = root;

	btree->root_page = root;

	/*
	 * Mark the child page dirty so that if it is evicted, the tree ends
	 * up sane.
	 */
	WT_ERR(__wt_page_modify_init(session, leaf));
	__wt_page_modify_set(leaf);

	return (0);

err:	if (leaf != NULL)
		__wt_page_out(session, leaf, 0);
	if (root != NULL)
		__wt_page_out(session, root, 0);
	return (ret);
}

/*
 * __wt_btree_root_empty --
 *	Bulk loads only work on empty trees: check before doing a bulk load.
 */
int
__wt_btree_root_empty(WT_SESSION_IMPL *session, WT_PAGE **leafp)
{
	WT_BTREE *btree;
	WT_PAGE *root, *child;

	btree = session->btree;
	root = btree->root_page;

	if (root->entries != 1)
		return (WT_ERROR);

	child = root->u.intl.t->page;
	if (child->entries != 0)
		return (WT_ERROR);

	*leafp = child;
	return (0);
}

/*
 * __btree_last --
 *      Read and pin the last page of the file.
 */
static int
__btree_last(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;

	btree = session->btree;

	if (btree->type == BTREE_ROW)
		return (0);

	page = NULL;
	WT_RET(__wt_tree_np(session, &page, 0, 0));
	if (page == NULL)
		return (WT_NOTFOUND);

	btree->last_page = page;
	btree->last_recno = __col_last_recno(page);

	F_SET(page, WT_PAGE_LAST_PAGE | WT_PAGE_PINNED);

	/*
	 * Publish: there must be a barrier to ensure the pinned flag is set
	 * before we discard our hazard reference.
	 */
	WT_WRITE_BARRIER();
	__wt_hazard_clear(session, page);

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
	int inuse, ret;

	btree = session->btree;
	conn = S2C(session);
	ret = 0;

	/* Remove from the connection's list. */
	__wt_spin_lock(session, &conn->spinlock);
	inuse = (--btree->refcnt > 0);
	if (!inuse) {
		TAILQ_REMOVE(&conn->btqh, btree, q);
		--conn->btqcnt;
	}
	__wt_spin_unlock(session, &conn->spinlock);
	if (inuse)
		return (0);

	if (F_ISSET(btree, WT_BTREE_OPEN))
		WT_STAT_DECR(conn->stats, file_open);

	/* Clear any cache. */
	if (btree->root_page != NULL)
		WT_TRET(__wt_evict_file_serial(session, 1));
	WT_ASSERT(session, btree->root_page == NULL);

	/* After all pages are evicted, update the root's address. */
	if (btree->root_update) {
		/*
		 * Release the original blocks held by the root, that is,
		 * the blocks listed in the schema file.
		 */
		WT_RET(__wt_btree_free_root(session));

		WT_RET(__wt_btree_set_root(
		    session, btree->root_addr.addr, btree->root_addr.size));
		if (btree->root_addr.addr != NULL)
			__wt_free(session, btree->root_addr.addr);
		btree->root_update = 0;
	}

	/* Close the underlying block manager reference. */
	WT_TRET(__wt_bm_close(session));

	/* Free allocated memory. */
	__wt_free(session, btree->name);
	__wt_free(session, btree->filename);
	__wt_free(session, btree->config);
	__wt_free(session, btree->key_format);
	__wt_free(session, btree->key_plan);
	__wt_free(session, btree->idxkey_format);
	__wt_free(session, btree->value_format);
	__wt_free(session, btree->value_plan);
	__wt_btree_huffman_close(session);
	__wt_free(session, btree->stats);

	WT_TRET(__wt_rwlock_destroy(session, btree->rwlock));
	__wt_free(session, session->btree);

	return (ret);
}

/*
 * __btree_page_sizes --
 *	Verify the page sizes.
 */
static int
__btree_page_sizes(WT_SESSION_IMPL *session, const char *config)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	uint32_t intl_split_size, leaf_split_size, split_pct;

	btree = session->btree;

	WT_RET(__wt_config_getones(session, config, "allocation_size", &cval));
	btree->allocsize = (uint32_t)cval.val;
	WT_RET(
	    __wt_config_getones(session, config, "internal_page_max", &cval));
	btree->maxintlpage = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(
	    session, config, "internal_item_max", &cval));
	btree->maxintlitem = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(session, config, "leaf_page_max", &cval));
	btree->maxleafpage = (uint32_t)cval.val;
	WT_RET(__wt_config_getones(
	    session, config, "leaf_item_max", &cval));
	btree->maxleafitem = (uint32_t)cval.val;

	/* Allocation sizes must be a power-of-two, nothing else makes sense. */
	if (!__wt_ispo2(btree->allocsize))
		WT_RET_MSG(session,
		    EINVAL, "the allocation size must be a power of two");

	/* All page sizes must be in units of the allocation size. */
	if (btree->maxintlpage < btree->allocsize ||
	    btree->maxintlpage % btree->allocsize != 0 ||
	    btree->maxleafpage < btree->allocsize ||
	    btree->maxleafpage % btree->allocsize != 0)
		WT_RET_MSG(session, EINVAL,
		    "page sizes must be a multiple of the page allocation "
		    "size (%" PRIu32 "B)", btree->allocsize);

	/*
	 * Set the split percentage: reconciliation splits to a
	 * smaller-than-maximum page size so we don't split every time a new
	 * entry is added.
	 */
	WT_RET(__wt_config_getones(session, config, "split_pct", &cval));
	split_pct = (uint32_t)cval.val;
	intl_split_size = WT_SPLIT_PAGE_SIZE(
	    btree->maxintlpage, btree->allocsize, split_pct);
	leaf_split_size = WT_SPLIT_PAGE_SIZE(
	    btree->maxleafpage, btree->allocsize, split_pct);

	/*
	 * Default values for internal and leaf page items: make sure at least
	 * 8 items fit on split pages.
	 */
	if (btree->maxintlitem == 0)
		    btree->maxintlitem = intl_split_size / 8;
	if (btree->maxleafitem == 0)
		    btree->maxleafitem = leaf_split_size / 8;
	/* Check we can fit at least 2 items on a page. */
	if (btree->maxintlitem > btree->maxintlpage / 2)
		return (pse1(session, "internal",
		    btree->maxintlpage, btree->maxintlitem));
	if (btree->maxleafitem > btree->maxleafpage / 2)
		return (pse1(session, "leaf",
		    btree->maxleafpage, btree->maxleafitem));

	/*
	 * Take into account the size of a split page:
	 *
	 * Make it a separate error message so it's clear what went wrong.
	 */
	if (btree->maxintlitem > intl_split_size / 2)
		return (pse2(session, "internal",
		    btree->maxintlpage, btree->maxintlitem, split_pct));
	if (btree->maxleafitem > leaf_split_size / 2)
		return (pse2(session, "leaf",
		    btree->maxleafpage, btree->maxleafitem, split_pct));

	/*
	 * Limit allocation units to 128MB, and page sizes to 512MB.  There's
	 * no reason we couldn't support larger sizes (any sizes up to the
	 * smaller of an off_t and a size_t should work), but an application
	 * specifying larger allocation or page sizes would likely be making
	 * as mistake.  The API checked this, but we assert it anyway.
	 */
	WT_ASSERT(session, btree->allocsize >= WT_BTREE_ALLOCATION_SIZE_MIN);
	WT_ASSERT(session, btree->allocsize <= WT_BTREE_ALLOCATION_SIZE_MAX);
	WT_ASSERT(session, btree->maxintlpage <= WT_BTREE_PAGE_SIZE_MAX);
	WT_ASSERT(session, btree->maxleafpage <= WT_BTREE_PAGE_SIZE_MAX);

	return (0);
}

static int
pse1(WT_SESSION_IMPL *session, const char *type, uint32_t max, uint32_t ovfl)
{
	WT_RET_MSG(session, EINVAL,
	    "%s page size (%" PRIu32 "B) too small for the maximum item size "
	    "(%" PRIu32 "B); the page must be able to hold at least 2 items",
	    type, max, ovfl);
}

static int
pse2(WT_SESSION_IMPL *session,
    const char *type, uint32_t max, uint32_t ovfl, uint32_t pct)
{
	WT_RET_MSG(session, EINVAL,
	    "%s page size (%" PRIu32 "B) too small for the maximum item size "
	    "(%" PRIu32 "B), because of the split percentage (%" PRIu32
	    "%%); a split page must be able to hold at least 2 items",
	    type, max, ovfl, pct);
}
