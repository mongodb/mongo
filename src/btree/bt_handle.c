/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __btree_conf(WT_SESSION_IMPL *, WT_CKPT *ckpt);
static int __btree_get_last_recno(WT_SESSION_IMPL *);
static int __btree_page_sizes(WT_SESSION_IMPL *);
static int __btree_preload(WT_SESSION_IMPL *);
static int __btree_tree_open_empty(WT_SESSION_IMPL *, int, int);

static int pse1(WT_SESSION_IMPL *, const char *, uint32_t, uint32_t);
static int pse2(WT_SESSION_IMPL *, const char *, uint32_t, uint32_t, int);

/*
 * __wt_btree_open --
 *	Open a Btree.
 */
int
__wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[])
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_CKPT ckpt;
	WT_CONFIG_ITEM cval;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	size_t root_addr_size;
	uint8_t root_addr[WT_BTREE_MAX_ADDR_COOKIE];
	int creation, forced_salvage, readonly;
	const char *filename;

	dhandle = session->dhandle;
	btree = S2BT(session);

	/* Checkpoint files are readonly. */
	readonly = dhandle->checkpoint == NULL ? 0 : 1;

	/* Get the checkpoint information for this name/checkpoint pair. */
	WT_CLEAR(ckpt);
	WT_RET(__wt_meta_checkpoint(
	    session, dhandle->name, dhandle->checkpoint, &ckpt));

	/*
	 * Bulk-load is only permitted on newly created files, not any empty
	 * file -- see the checkpoint code for a discussion.
	 */
	creation = ckpt.raw.size == 0;
	if (!creation && F_ISSET(btree, WT_BTREE_BULK))
		WT_ERR_MSG(session, EINVAL,
		    "bulk-load is only supported on newly created objects");

	/* Handle salvage configuration. */
	forced_salvage = 0;
	if (F_ISSET(btree, WT_BTREE_SALVAGE)) {
		WT_ERR(__wt_config_gets(session, op_cfg, "force", &cval));
		forced_salvage = (cval.val != 0);
	}

	/* Initialize and configure the WT_BTREE structure. */
	WT_ERR(__btree_conf(session, &ckpt));

	/* Connect to the underlying block manager. */
	filename = dhandle->name;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_ERR_MSG(session, EINVAL, "expected a 'file:' URI");

	WT_ERR(__wt_block_manager_open(session, filename, dhandle->cfg,
	    forced_salvage, readonly, btree->allocsize, &btree->bm));
	bm = btree->bm;

	/*
	 * !!!
	 * As part of block-manager configuration, we need to return the maximum
	 * sized address cookie that a block manager will ever return.  There's
	 * a limit of WT_BTREE_MAX_ADDR_COOKIE, but at 255B, it's too large for
	 * a Btree with 512B internal pages.  The default block manager packs
	 * a wt_off_t and 2 uint32_t's into its cookie, so there's no problem
	 * now, but when we create a block manager extension API, we need some
	 * way to consider the block manager's maximum cookie size versus the
	 * minimum Btree internal node size.
	 */
	btree->block_header = bm->block_header(bm);

	/*
	 * Open the specified checkpoint unless it's a special command (special
	 * commands are responsible for loading their own checkpoints, if any).
	 */
	if (!F_ISSET(btree,
	    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
		/*
		 * There are two reasons to load an empty tree rather than a
		 * checkpoint: either there is no checkpoint (the file is
		 * being created), or the load call returns no root page (the
		 * checkpoint is for an empty file).
		 */
		WT_ERR(bm->checkpoint_load(bm, session,
		    ckpt.raw.data, ckpt.raw.size,
		    root_addr, &root_addr_size, readonly));
		if (creation || root_addr_size == 0)
			WT_ERR(__btree_tree_open_empty(
			    session, creation, readonly));
		else {
			WT_ERR(__wt_btree_tree_open(
			    session, root_addr, root_addr_size));

			/* Warm the cache, if possible. */
			WT_ERR(__btree_preload(session));

			/* Get the last record number in a column-store file. */
			if (btree->type != BTREE_ROW)
				WT_ERR(__btree_get_last_recno(session));
		}
	}

	if (0) {
err:		WT_TRET(__wt_btree_close(session));
	}
	__wt_meta_checkpoint_free(session, &ckpt);

	return (ret);
}

/*
 * __wt_btree_close --
 *	Close a Btree.
 */
int
__wt_btree_close(WT_SESSION_IMPL *session)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;

	dhandle = session->dhandle;
	btree = S2BT(session);

	if ((bm = btree->bm) != NULL) {
		/* Unload the checkpoint, unless it's a special command. */
		if (F_ISSET(dhandle, WT_DHANDLE_OPEN) &&
		    !F_ISSET(btree,
		    WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
			WT_TRET(bm->checkpoint_unload(bm, session));

		/* Close the underlying block manager reference. */
		WT_TRET(bm->close(bm, session));

		btree->bm = NULL;
	}

	/* Close the Huffman tree. */
	__wt_btree_huffman_close(session);

	/* Destroy locks. */
	WT_TRET(__wt_rwlock_destroy(session, &btree->ovfl_lock));
	__wt_spin_destroy(session, &btree->flush_lock);

	/* Free allocated memory. */
	__wt_free(session, btree->key_format);
	__wt_free(session, btree->value_format);

	if (btree->collator_owned) {
		if (btree->collator->terminate != NULL)
			WT_TRET(btree->collator->terminate(
			    btree->collator, &session->iface));
		btree->collator_owned = 0;
	}
	btree->collator = NULL;

	btree->bulk_load_ok = 0;

	return (ret);
}

/*
 * __btree_conf --
 *	Configure a WT_BTREE structure.
 */
static int
__btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval, metadata;
	WT_CONNECTION_IMPL *conn;
	WT_NAMED_COMPRESSOR *ncomp;
	int64_t maj_version, min_version;
	uint32_t bitcnt;
	int fixed;
	const char **cfg;

	btree = S2BT(session);
	conn = S2C(session);
	cfg = btree->dhandle->cfg;

	/* Dump out format information. */
	if (WT_VERBOSE_ISSET(session, WT_VERB_VERSION)) {
		WT_RET(__wt_config_gets(session, cfg, "version.major", &cval));
		maj_version = cval.val;
		WT_RET(__wt_config_gets(session, cfg, "version.minor", &cval));
		min_version = cval.val;
		WT_RET(__wt_verbose(session, WT_VERB_VERSION,
		    "%" PRIu64 ".%" PRIu64, maj_version, min_version));
	}

	/* Get the file ID. */
	WT_RET(__wt_config_gets(session, cfg, "id", &cval));
	btree->id = (uint32_t)cval.val;

	/* Validate file types and check the data format plan. */
	WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_RET(__wt_struct_check(session, cval.str, cval.len, NULL, NULL));
	if (WT_STRING_MATCH("r", cval.str, cval.len))
		btree->type = BTREE_COL_VAR;
	else
		btree->type = BTREE_ROW;
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->key_format));

	WT_RET(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_RET(__wt_struct_check(session, cval.str, cval.len, NULL, NULL));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->value_format));

	/* Row-store key comparison and key gap for prefix compression. */
	if (btree->type == BTREE_ROW) {
		WT_RET(
		    __wt_config_gets(session, cfg, "app_metadata", &metadata));
		WT_RET(__wt_config_gets(session, cfg, "collator", &cval));
		if (cval.len != 0)
			WT_RET(__wt_collator_config(
			    session, btree->dhandle->name, &cval, &metadata,
			    &btree->collator, &btree->collator_owned));

		WT_RET(__wt_config_gets(session, cfg, "key_gap", &cval));
		btree->key_gap = (uint32_t)cval.val;
	}

	/* Column-store: check for fixed-size data. */
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
	WT_RET(__btree_page_sizes(session));

	/* Eviction; the metadata file is never evicted. */
	if (WT_IS_METADATA(btree->dhandle))
		F_SET(btree, WT_BTREE_NO_EVICTION | WT_BTREE_NO_HAZARD);
	else {
		WT_RET(__wt_config_gets(session, cfg, "cache_resident", &cval));
		if (cval.val)
			F_SET(btree, WT_BTREE_NO_EVICTION | WT_BTREE_NO_HAZARD);
		else
			F_CLR(btree, WT_BTREE_NO_EVICTION);
	}

	/* Checksums */
	WT_RET(__wt_config_gets(session, cfg, "checksum", &cval));
	if (WT_STRING_MATCH("on", cval.str, cval.len))
		btree->checksum = CKSUM_ON;
	else if (WT_STRING_MATCH("off", cval.str, cval.len))
		btree->checksum = CKSUM_OFF;
	else
		btree->checksum = CKSUM_UNCOMPRESSED;

	/* Huffman encoding */
	WT_RET(__wt_btree_huffman_open(session));

	/*
	 * Reconciliation configuration:
	 *	Block compression (all)
	 *	Dictionary compression (variable-length column-store, row-store)
	 *	Page-split percentage
	 *	Prefix compression (row-store)
	 *	Suffix compression (row-store)
	 */
	switch (btree->type) {
	case BTREE_COL_FIX:
		break;
	case BTREE_ROW:
		WT_RET(__wt_config_gets(
		    session, cfg, "internal_key_truncate", &cval));
		btree->internal_key_truncate = cval.val == 0 ? 0 : 1;

		WT_RET(__wt_config_gets(
		    session, cfg, "prefix_compression", &cval));
		btree->prefix_compression = cval.val == 0 ? 0 : 1;
		WT_RET(__wt_config_gets(
		    session, cfg, "prefix_compression_min", &cval));
		btree->prefix_compression_min = (u_int)cval.val;
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		WT_RET(__wt_config_gets(session, cfg, "dictionary", &cval));
		btree->dictionary = (u_int)cval.val;
		break;
	}

	WT_RET(__wt_config_gets(session, cfg, "block_compressor", &cval));
	if (cval.len > 0) {
		TAILQ_FOREACH(ncomp, &conn->compqh, q)
			if (WT_STRING_MATCH(ncomp->name, cval.str, cval.len)) {
				btree->compressor = ncomp->compressor;
				break;
			}
		if (btree->compressor == NULL)
			WT_RET_MSG(session, EINVAL,
			    "unknown block compressor '%.*s'",
			    (int)cval.len, cval.str);
	}

	/* Initialize locks. */
	WT_RET(__wt_rwlock_alloc(
	    session, &btree->ovfl_lock, "btree overflow lock"));
	WT_RET(__wt_spin_init(session, &btree->flush_lock, "btree flush lock"));

	__wt_stat_init_dsrc_stats(&btree->dhandle->stats);

	btree->write_gen = ckpt->write_gen;		/* Write generation */
	btree->modified = 0;				/* Clean */

	return (0);
}

/*
 * __wt_root_ref_init --
 *	Initialize a tree root reference, and link in the root page.
 */
void
__wt_root_ref_init(WT_REF *root_ref, WT_PAGE *root, int is_recno)
{
	memset(root_ref, 0, sizeof(*root_ref));

	root_ref->page = root;
	root_ref->state = WT_REF_MEM;

	root_ref->key.recno = is_recno ? 1 : 0;

	root->pg_intl_parent_ref = root_ref;
}

/*
 * __wt_btree_tree_open --
 *	Read in a tree from disk.
 */
int
__wt_btree_tree_open(
    WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_ITEM dsk;
	WT_PAGE *page;

	btree = S2BT(session);

	/*
	 * A buffer into which we read a root page; don't use a scratch buffer,
	 * the buffer's allocated memory becomes the persistent in-memory page.
	 */
	WT_CLEAR(dsk);

	/* Read the page, then build the in-memory version of the page. */
	WT_ERR(__wt_bt_read(session, &dsk, addr, addr_size));
	WT_ERR(__wt_page_inmem(session, NULL, dsk.data,
	    WT_DATA_IN_ITEM(&dsk) ?
	    WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED , &page));

	/* Finish initializing the root, root reference links. */
	__wt_root_ref_init(&btree->root, page, btree->type != BTREE_ROW);

	if (0) {
err:		__wt_buf_free(session, &dsk);
	}
	return (ret);
}

/*
 * __btree_tree_open_empty --
 *	Create an empty in-memory tree.
 */
static int
__btree_tree_open_empty(WT_SESSION_IMPL *session, int creation, int readonly)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *root, *leaf;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref;

	btree = S2BT(session);
	root = leaf = NULL;

	/*
	 * Newly created objects can be used for cursor inserts or for bulk
	 * loads; set a flag that's cleared when a row is inserted into the
	 * tree.   Objects being bulk-loaded cannot be evicted, we set it
	 * globally, there's no point in searching empty trees for eviction.
	 */
	if (creation) {
		btree->bulk_load_ok = 1;
		__wt_btree_evictable(session, 0);
	}

	/*
	 * A note about empty trees: the initial tree is a root page and a leaf
	 * page.  We need a pair of pages instead of just a single page because
	 * we can reconcile the leaf page while the root stays pinned in memory.
	 * If the pair is evicted without being modified, that's OK, nothing is
	 * ever written.
	 *
	 * Create the root and leaf pages.
	 *
	 * !!!
	 * Be cautious about changing the order of updates in this code: to call
	 * __wt_page_out on error, we require a correct page setup at each point
	 * where we might fail.
	 */
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(
		    __wt_page_alloc(session, WT_PAGE_COL_INT, 1, 1, 1, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_COPY(root);
		ref = pindex->index[0];
		ref->home = root;
		WT_ERR(__wt_btree_new_leaf_page(session, &leaf));
		ref->page = leaf;
		ref->addr = NULL;
		ref->state = WT_REF_MEM;
		ref->key.recno = 1;
		break;
	case BTREE_ROW:
		WT_ERR(
		    __wt_page_alloc(session, WT_PAGE_ROW_INT, 0, 1, 1, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_COPY(root);
		ref = pindex->index[0];
		ref->home = root;
		WT_ERR(__wt_btree_new_leaf_page(session, &leaf));
		ref->page = leaf;
		ref->addr = NULL;
		ref->state = WT_REF_MEM;
		WT_ERR(__wt_row_ikey_incr(
		    session, root, 0, "", 1, &ref->key.ikey));
		break;
	WT_ILLEGAL_VALUE_ERR(session);
	}

	/*
	 * Mark the leaf page dirty: we didn't create an entirely valid root
	 * page (specifically, the root page's disk address isn't set, and it's
	 * the act of reconciling the leaf page that makes it work, we don't
	 * try and use the original disk address of modified pages).  We could
	 * get around that by leaving the leaf page clean and building a better
	 * root page, but then we get into trouble because a checkpoint marks
	 * the root page dirty to force a write, and without reconciling the
	 * leaf page we won't realize there's no records to write, we'll write
	 * a root page, which isn't correct for an empty tree.
	 *
	 * Earlier versions of this code kept the leaf page clean, but with the
	 * "empty" flag set in the leaf page's modification structure; in that
	 * case, checkpoints works (forced reconciliation of a root with a
	 * single "empty" page wouldn't write any blocks). That version had
	 * memory leaks because the eviction code didn't correctly handle pages
	 * that were "clean" (and so never reconciled), yet "modified" with an
	 * "empty" flag.  The goal of this code is to mimic a real tree that
	 * simply has no records, for whatever reason, and trust reconciliation
	 * to figure out it's empty and not write any blocks.
	 *
	 * We do not set the tree's modified flag because the checkpoint code
	 * skips unmodified files in closing checkpoints (checkpoints that
	 * don't require a write unless the file is actually dirty).  There's
	 * no need to reconcile this file unless the application does a real
	 * checkpoint or it's actually modified.
	 *
	 * Only do this for a live tree, not for checkpoints.  If we open an
	 * empty checkpoint, the leaf page cannot be dirty or eviction may try
	 * to write it, which will fail because checkpoints are read-only.
	 */
	if (!readonly) {
		WT_ERR(__wt_page_modify_init(session, leaf));
		__wt_page_only_modify_set(session, leaf);
	}

	/* Finish initializing the root, root reference links. */
	__wt_root_ref_init(&btree->root, root, btree->type != BTREE_ROW);

	return (0);

err:	if (leaf != NULL)
		__wt_page_out(session, &leaf);
	if (root != NULL)
		__wt_page_out(session, &root);
	return (ret);
}

/*
 * __wt_btree_new_leaf_page --
 *	Create an empty leaf page and link it into a reference in its parent.
 */
int
__wt_btree_new_leaf_page(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	switch (btree->type) {
	case BTREE_COL_FIX:
		WT_RET(
		    __wt_page_alloc(session, WT_PAGE_COL_FIX, 1, 0, 1, pagep));
		break;
	case BTREE_COL_VAR:
		WT_RET(
		    __wt_page_alloc(session, WT_PAGE_COL_VAR, 1, 0, 1, pagep));
		break;
	case BTREE_ROW:
		WT_RET(
		    __wt_page_alloc(session, WT_PAGE_ROW_LEAF, 0, 0, 1, pagep));
		break;
	WT_ILLEGAL_VALUE(session);
	}
	return (0);
}

/*
 * __wt_btree_evictable --
 *      Setup or release a cache-resident tree.
 */
void
__wt_btree_evictable(WT_SESSION_IMPL *session, int on)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/* The metadata file is never evicted. */
	if (on && !WT_IS_METADATA(btree->dhandle))
		F_CLR(btree, WT_BTREE_NO_EVICTION);
	else
		F_SET(btree, WT_BTREE_NO_EVICTION);
}

/*
 * __btree_preload --
 *	Pre-load internal pages.
 */
static int
__btree_preload(WT_SESSION_IMPL *session)
{
	WT_BM *bm;
	WT_BTREE *btree;
	WT_REF *ref;
	size_t addr_size;
	const uint8_t *addr;

	btree = S2BT(session);
	bm = btree->bm;

	/* Pre-load the second-level internal pages. */
	WT_INTL_FOREACH_BEGIN(session, btree->root.page, ref) {
		WT_RET(__wt_ref_info(session, ref, &addr, &addr_size, NULL));
		if (addr != NULL)
			WT_RET(bm->preload(bm, session, addr, addr_size));
	} WT_INTL_FOREACH_END;
	return (0);
}

/*
 * __btree_get_last_recno --
 *	Set the last record number for a column-store.
 */
static int
__btree_get_last_recno(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_REF *next_walk;

	btree = S2BT(session);

	next_walk = NULL;
	WT_RET(__wt_tree_walk(session, &next_walk, WT_READ_PREV));
	if (next_walk == NULL)
		return (WT_NOTFOUND);

	page = next_walk->page;
	btree->last_recno = page->type == WT_PAGE_COL_VAR ?
	    __col_var_last_recno(page) : __col_fix_last_recno(page);

	return (__wt_page_release(session, next_walk, 0));
}

/*
 * __btree_page_sizes --
 *	Verify the page sizes. Some of these sizes are automatically checked
 *	using limits defined in the API, don't duplicate the logic here.
 */
static int
__btree_page_sizes(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_CONFIG_ITEM cval;
	uint64_t cache_size;
	uint32_t intl_split_size, leaf_split_size;
	const char **cfg;

	btree = S2BT(session);
	cfg = btree->dhandle->cfg;

	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "allocation_size", &btree->allocsize));
	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "internal_page_max", &btree->maxintlpage));
	WT_RET(__wt_config_gets(session, cfg, "internal_item_max", &cval));
	btree->maxintlitem = (uint32_t)cval.val;
	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "leaf_page_max", &btree->maxleafpage));
	WT_RET(__wt_config_gets(session, cfg, "leaf_item_max", &cval));
	btree->maxleafitem = (uint32_t)cval.val;

	WT_RET(__wt_config_gets(session, cfg, "split_pct", &cval));
	btree->split_pct = (int)cval.val;

	/*
	 * When a page is forced to split, we want at least 50 entries on its
	 * parent.
	 */
	WT_RET(__wt_config_gets(session, cfg, "memory_page_max", &cval));
	btree->maxmempage = WT_MAX((uint64_t)cval.val, 50 * btree->maxleafpage);

	/*
	 * Don't let pages grow to more than half the cache size.  Otherwise,
	 * with very small caches, we can end up in a situation where nothing
	 * can be evicted.  Take care getting the cache size: with a shared
	 * cache, it may not have been set.
	 */
	cache_size = S2C(session)->cache_size;
	if (cache_size > 0)
		btree->maxmempage = WT_MIN(btree->maxmempage, cache_size / 2);

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
	 * Set the split percentage: reconciliation splits to a smaller-than-
	 * maximum page size so we don't split every time a new entry is added.
	 */
	intl_split_size = __wt_split_page_size(btree, btree->maxintlpage);
	leaf_split_size = __wt_split_page_size(btree, btree->maxleafpage);

	/*
	 * Default values for internal and leaf page items: make sure at least
	 * 8 items fit on split pages.
	 */
	if (btree->maxintlitem == 0)
		    btree->maxintlitem = intl_split_size / 8;
	if (btree->maxleafitem == 0)
		    btree->maxleafitem = leaf_split_size / 8;

	/*
	 * If raw compression is configured, the application owns page layout,
	 * it's not our problem.   Hopefully the application chose well.
	 */
	if (btree->compressor != NULL &&
	    btree->compressor->compress_raw != NULL)
		return (0);

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
		    btree->maxintlpage, btree->maxintlitem, btree->split_pct));
	if (btree->maxleafitem > leaf_split_size / 2)
		return (pse2(session, "leaf",
		    btree->maxleafpage, btree->maxleafitem, btree->split_pct));

	return (0);
}

/*
 * __wt_split_page_size --
 *	Split page size calculation: we don't want to repeatedly split every
 * time a new entry is added, so we split to a smaller-than-maximum page size.
 */
uint32_t
__wt_split_page_size(WT_BTREE *btree, uint32_t maxpagesize)
{
	uintmax_t a;
	uint32_t split_size;

	/*
	 * Ideally, the split page size is some percentage of the maximum page
	 * size rounded to an allocation unit (round to an allocation unit so
	 * we don't waste space when we write).
	 */
	a = maxpagesize;			/* Don't overflow. */
	split_size = (uint32_t)
	    WT_ALIGN((a * (u_int)btree->split_pct) / 100, btree->allocsize);

	/*
	 * If the result of that calculation is the same as the allocation unit
	 * (that happens if the maximum size is the same size as an allocation
	 * unit, use a percentage of the maximum page size).
	 */
	if (split_size == btree->allocsize)
		split_size = (uint32_t)((a * (u_int)btree->split_pct) / 100);

	return (split_size);
}

/*
 * pse1 --
 *	Page size error message 1.
 */
static int
pse1(WT_SESSION_IMPL *session, const char *type, uint32_t max, uint32_t ovfl)
{
	WT_RET_MSG(session, EINVAL,
	    "%s page size (%" PRIu32 "B) too small for the maximum item size "
	    "(%" PRIu32 "B); the page must be able to hold at least 2 items",
	    type, max, ovfl);
}

/*
 * pse2 --
 *	Page size error message 2.
 */
static int
pse2(WT_SESSION_IMPL *session,
    const char *type, uint32_t max, uint32_t ovfl, int pct)
{
	WT_RET_MSG(session, EINVAL,
	    "%s page size (%" PRIu32 "B) too small for the maximum item size "
	    "(%" PRIu32 "B), because of the split percentage (%d %%); a split "
	    "page must be able to hold at least 2 items",
	    type, max, ovfl, pct);
}
