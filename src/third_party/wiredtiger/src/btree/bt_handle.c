/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
static int __btree_tree_open_empty(WT_SESSION_IMPL *, bool);

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
	const char *filename;
	bool creation, forced_salvage, readonly;

	dhandle = session->dhandle;
	btree = S2BT(session);

	/* Checkpoint files are readonly. */
	readonly = dhandle->checkpoint != NULL ||
	    F_ISSET(S2C(session), WT_CONN_READONLY);

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
	forced_salvage = false;
	if (F_ISSET(btree, WT_BTREE_SALVAGE)) {
		WT_ERR(__wt_config_gets(session, op_cfg, "force", &cval));
		forced_salvage = cval.val != 0;
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
			WT_ERR(__btree_tree_open_empty(session, creation));
		else {
			WT_ERR(__wt_btree_tree_open(
			    session, root_addr, root_addr_size));

			/*
			 * Rebalance uses the cache, but only wants the root
			 * page, nothing else.
			 */
			if (!F_ISSET(btree, WT_BTREE_REBALANCE)) {
				/* Warm the cache, if possible. */
				WT_WITH_PAGE_INDEX(session,
				    ret = __btree_preload(session));
				WT_ERR(ret);

				/*
				 * Get the last record number in a column-store
				 * file.
				 */
				if (btree->type != BTREE_ROW)
					WT_ERR(__btree_get_last_recno(session));
			}
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
	WT_DECL_RET;

	btree = S2BT(session);

	if ((bm = btree->bm) != NULL) {
		/* Unload the checkpoint, unless it's a special command. */
		if (!F_ISSET(btree,
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
	btree->kencryptor = NULL;

	btree->bulk_load_ok = false;

	F_CLR(btree, WT_BTREE_SPECIAL_FLAGS);

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
	WT_CONFIG_ITEM cval, enc, keyid, metadata;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	int64_t maj_version, min_version;
	uint32_t bitcnt;
	bool fixed;
	const char **cfg, *enc_cfg[] = { NULL, NULL };

	btree = S2BT(session);
	cfg = btree->dhandle->cfg;
	conn = S2C(session);

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
	WT_RET(__wt_struct_confchk(session, &cval));
	if (WT_STRING_MATCH("r", cval.str, cval.len))
		btree->type = BTREE_COL_VAR;
	else
		btree->type = BTREE_ROW;
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->key_format));

	WT_RET(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_RET(__wt_struct_confchk(session, &cval));
	WT_RET(__wt_strndup(session, cval.str, cval.len, &btree->value_format));

	/* Row-store key comparison and key gap for prefix compression. */
	if (btree->type == BTREE_ROW) {
		WT_RET(__wt_config_gets_none(session, cfg, "collator", &cval));
		if (cval.len != 0) {
			WT_RET(__wt_config_gets(
			    session, cfg, "app_metadata", &metadata));
			WT_RET(__wt_collator_config(
			    session, btree->dhandle->name, &cval, &metadata,
			    &btree->collator, &btree->collator_owned));
		}

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

	WT_RET(__wt_config_gets(session, cfg, "cache_resident", &cval));
	if (cval.val)
		F_SET(btree, WT_BTREE_IN_MEMORY | WT_BTREE_NO_EVICTION);
	else
		F_CLR(btree, WT_BTREE_IN_MEMORY | WT_BTREE_NO_EVICTION);

	WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
	if (cval.val)
		F_CLR(btree, WT_BTREE_NO_LOGGING);
	else
		F_SET(btree, WT_BTREE_NO_LOGGING);

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
		btree->internal_key_truncate = cval.val != 0;

		WT_RET(__wt_config_gets(
		    session, cfg, "prefix_compression", &cval));
		btree->prefix_compression = cval.val != 0;
		WT_RET(__wt_config_gets(
		    session, cfg, "prefix_compression_min", &cval));
		btree->prefix_compression_min = (u_int)cval.val;
		/* FALLTHROUGH */
	case BTREE_COL_VAR:
		WT_RET(__wt_config_gets(session, cfg, "dictionary", &cval));
		btree->dictionary = (u_int)cval.val;
		break;
	}

	WT_RET(__wt_config_gets_none(session, cfg, "block_compressor", &cval));
	WT_RET(__wt_compressor_config(session, &cval, &btree->compressor));

	/*
	 * We do not use __wt_config_gets_none here because "none"
	 * and the empty string have different meanings.  The
	 * empty string means inherit the system encryption setting
	 * and "none" means this table is in the clear even if the
	 * database is encrypted.  If this is the metadata handle
	 * always inherit from the connection.
	 */
	WT_RET(__wt_config_gets(session, cfg, "encryption.name", &cval));
	if (WT_IS_METADATA(session, btree->dhandle) || cval.len == 0)
		btree->kencryptor = conn->kencryptor;
	else if (WT_STRING_MATCH("none", cval.str, cval.len))
		btree->kencryptor = NULL;
	else {
		WT_RET(__wt_config_gets_none(
		    session, cfg, "encryption.keyid", &keyid));
		WT_RET(__wt_config_gets(session, cfg, "encryption", &enc));
		if (enc.len != 0)
			WT_RET(__wt_strndup(session, enc.str, enc.len,
			    &enc_cfg[0]));
		ret = __wt_encryptor_config(session, &cval, &keyid,
		    (WT_CONFIG_ARG *)enc_cfg, &btree->kencryptor);
		__wt_free(session, enc_cfg[0]);
		WT_RET(ret);
	}

	/* Initialize locks. */
	WT_RET(__wt_rwlock_alloc(
	    session, &btree->ovfl_lock, "btree overflow lock"));
	WT_RET(__wt_spin_init(session, &btree->flush_lock, "btree flush"));

	btree->checkpointing = WT_CKPT_OFF;		/* Not checkpointing */
	btree->modified = 0;				/* Clean */
	btree->write_gen = ckpt->write_gen;		/* Write generation */

	return (0);
}

/*
 * __wt_root_ref_init --
 *	Initialize a tree root reference, and link in the root page.
 */
void
__wt_root_ref_init(WT_REF *root_ref, WT_PAGE *root, bool is_recno)
{
	memset(root_ref, 0, sizeof(*root_ref));

	root_ref->page = root;
	root_ref->state = WT_REF_MEM;

	root_ref->ref_recno = is_recno ? 1 : WT_RECNO_OOB;

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
	WT_BM *bm;
	WT_BTREE *btree;
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;
	WT_ITEM dsk;
	WT_PAGE *page;

	btree = S2BT(session);
	bm = btree->bm;

	/*
	 * A buffer into which we read a root page; don't use a scratch buffer,
	 * the buffer's allocated memory becomes the persistent in-memory page.
	 */
	WT_CLEAR(dsk);

	/*
	 * Read and verify the page (verify to catch encrypted objects we can't
	 * decrypt, where we read the object successfully but we can't decrypt
	 * it, and we want to fail gracefully).
	 *
	 * Create a printable version of the address to pass to verify.
	 */
	WT_ERR(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));

	F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
	if ((ret = __wt_bt_read(session, &dsk, addr, addr_size)) == 0)
		ret = __wt_verify_dsk(session, tmp->data, &dsk);
	F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);
	if (ret != 0)
		__wt_err(session, ret,
		    "unable to read root page from %s", session->dhandle->name);
	/*
	 * Failure to open metadata means that the database is unavailable.
	 * Try to provide a helpful failure message.
	 */
	if (ret != 0 && WT_IS_METADATA(session, session->dhandle)) {
		__wt_errx(session,
		    "WiredTiger has failed to open its metadata");
		__wt_errx(session, "This may be due to the database"
		    " files being encrypted, being from an older"
		    " version or due to corruption on disk");
		__wt_errx(session, "You should confirm that you have"
		    " opened the database with the correct options including"
		    " all encryption and compression options");
	}
	WT_ERR(ret);

	/*
	 * Build the in-memory version of the page. Clear our local reference to
	 * the allocated copy of the disk image on return, the in-memory object
	 * steals it.
	 */
	WT_ERR(__wt_page_inmem(session, NULL, dsk.data, dsk.memsize,
	    WT_DATA_IN_ITEM(&dsk) ?
	    WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page));
	dsk.mem = NULL;

	/* Finish initializing the root, root reference links. */
	__wt_root_ref_init(&btree->root, page, btree->type != BTREE_ROW);

err:	__wt_buf_free(session, &dsk);
	__wt_scr_free(session, &tmp);

	return (ret);
}

/*
 * __btree_tree_open_empty --
 *	Create an empty in-memory tree.
 */
static int
__btree_tree_open_empty(WT_SESSION_IMPL *session, bool creation)
{
	WT_BTREE *btree;
	WT_DECL_RET;
	WT_PAGE *leaf, *root;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref;

	btree = S2BT(session);
	root = leaf = NULL;
	ref = NULL;

	/*
	 * Newly created objects can be used for cursor inserts or for bulk
	 * loads; set a flag that's cleared when a row is inserted into the
	 * tree. Objects being bulk-loaded cannot be evicted, we set it
	 * globally, there's no point in searching empty trees for eviction.
	 */
	if (creation) {
		btree->bulk_load_ok = true;
		__wt_btree_evictable(session, false);
	}

	/*
	 * A note about empty trees: the initial tree is a single root page.
	 * It has a single reference to a leaf page, marked deleted.  The leaf
	 * page will be created by the first update.  If the root is evicted
	 * without being modified, that's OK, nothing is ever written.
	 *
	 * !!!
	 * Be cautious about changing the order of updates in this code: to call
	 * __wt_page_out on error, we require a correct page setup at each point
	 * where we might fail.
	 */
	switch (btree->type) {
	case BTREE_COL_FIX:
	case BTREE_COL_VAR:
		WT_ERR(__wt_page_alloc(
		    session, WT_PAGE_COL_INT, 1, true, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_GET_SAFE(root);
		ref = pindex->index[0];
		ref->home = root;
		ref->page = NULL;
		ref->addr = NULL;
		ref->state = WT_REF_DELETED;
		ref->ref_recno = 1;
		break;
	case BTREE_ROW:
		WT_ERR(__wt_page_alloc(
		    session, WT_PAGE_ROW_INT, 1, true, &root));
		root->pg_intl_parent_ref = &btree->root;

		pindex = WT_INTL_INDEX_GET_SAFE(root);
		ref = pindex->index[0];
		ref->home = root;
		ref->page = NULL;
		ref->addr = NULL;
		ref->state = WT_REF_DELETED;
		WT_ERR(__wt_row_ikey_incr(session, root, 0, "", 1, ref));
		break;
	}

	/* Bulk loads require a leaf page for reconciliation: create it now. */
	if (F_ISSET(btree, WT_BTREE_BULK)) {
		WT_ERR(__wt_btree_new_leaf_page(session, &leaf));
		ref->page = leaf;
		ref->state = WT_REF_MEM;
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
 *	Create an empty leaf page.
 */
int
__wt_btree_new_leaf_page(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	switch (btree->type) {
	case BTREE_COL_FIX:
		WT_RET(__wt_page_alloc(
		    session, WT_PAGE_COL_FIX, 0, false, pagep));
		break;
	case BTREE_COL_VAR:
		WT_RET(__wt_page_alloc(
		    session, WT_PAGE_COL_VAR, 0, false, pagep));
		break;
	case BTREE_ROW:
		WT_RET(__wt_page_alloc(
		    session, WT_PAGE_ROW_LEAF, 0, false, pagep));
		break;
	}
	return (0);
}

/*
 * __wt_btree_evictable --
 *      Setup or release a cache-resident tree.
 */
void
__wt_btree_evictable(WT_SESSION_IMPL *session, bool on)
{
	WT_BTREE *btree;

	btree = S2BT(session);

	/* Permanently cache-resident files can never be evicted. */
	if (F_ISSET(btree, WT_BTREE_IN_MEMORY))
		return;

	if (on)
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
		__wt_ref_info(ref, &addr, &addr_size, NULL);
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
	    __col_var_last_recno(next_walk) : __col_fix_last_recno(next_walk);

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
	WT_CONNECTION_IMPL *conn;
	uint64_t cache_size;
	uint32_t intl_split_size, leaf_split_size;
	const char **cfg;

	btree = S2BT(session);
	conn = S2C(session);
	cfg = btree->dhandle->cfg;

	/*
	 * Get the allocation size.  Allocation sizes must be a power-of-two,
	 * nothing else makes sense.
	 */
	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "allocation_size", &btree->allocsize));
	if (!__wt_ispo2(btree->allocsize))
		WT_RET_MSG(session,
		    EINVAL, "the allocation size must be a power of two");

	/*
	 * Get the internal/leaf page sizes.
	 * All page sizes must be in units of the allocation size.
	 */
	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "internal_page_max", &btree->maxintlpage));
	WT_RET(__wt_direct_io_size_check(
	    session, cfg, "leaf_page_max", &btree->maxleafpage));
	if (btree->maxintlpage < btree->allocsize ||
	    btree->maxintlpage % btree->allocsize != 0 ||
	    btree->maxleafpage < btree->allocsize ||
	    btree->maxleafpage % btree->allocsize != 0)
		WT_RET_MSG(session, EINVAL,
		    "page sizes must be a multiple of the page allocation "
		    "size (%" PRIu32 "B)", btree->allocsize);

	/*
	 * Don't let pages grow large compared to the cache size or we can end
	 * up in a situation where nothing can be evicted.  Take care getting
	 * the cache size: with a shared cache, it may not have been set.
	 */
	WT_RET(__wt_config_gets(session, cfg, "memory_page_max", &cval));
	btree->maxmempage = (uint64_t)cval.val;
	if (!F_ISSET(conn, WT_CONN_CACHE_POOL)) {
		if ((cache_size = conn->cache_size) > 0)
			btree->maxmempage =
			    WT_MIN(btree->maxmempage, cache_size / 10);
	}
	/* Enforce a lower bound of a single disk leaf page */
	btree->maxmempage = WT_MAX(btree->maxmempage, btree->maxleafpage);

	/*
	 * Try in-memory splits once we hit 80% of the maximum in-memory page
	 * size.  This gives multi-threaded append workloads a better chance of
	 * not stalling.
	 */
	btree->splitmempage = 8 * btree->maxmempage / 10;

	/*
	 * Get the split percentage (reconciliation splits pages into smaller
	 * than the maximum page size chunks so we don't split every time a
	 * new entry is added). Determine how large newly split pages will be.
	 */
	WT_RET(__wt_config_gets(session, cfg, "split_pct", &cval));
	btree->split_pct = (int)cval.val;
	intl_split_size = __wt_split_page_size(btree, btree->maxintlpage);
	leaf_split_size = __wt_split_page_size(btree, btree->maxleafpage);

	/*
	 * In-memory split configuration.
	 */
	if (__wt_config_gets(
	    session, cfg, "split_deepen_min_child", &cval) == WT_NOTFOUND ||
	    cval.val == 0)
		btree->split_deepen_min_child = WT_SPLIT_DEEPEN_MIN_CHILD_DEF;
	else
		btree->split_deepen_min_child = (u_int)cval.val;
	if (__wt_config_gets(
	    session, cfg, "split_deepen_per_child", &cval) == WT_NOTFOUND ||
	    cval.val == 0)
		btree->split_deepen_per_child = WT_SPLIT_DEEPEN_PER_CHILD_DEF;
	else
		btree->split_deepen_per_child = (u_int)cval.val;

	/*
	 * Get the maximum internal/leaf page key/value sizes.
	 *
	 * In-memory configuration overrides any key/value sizes, there's no
	 * such thing as an overflow item in an in-memory configuration.
	 */
	if (F_ISSET(conn, WT_CONN_IN_MEMORY)) {
		btree->maxintlkey = WT_BTREE_MAX_OBJECT_SIZE;
		btree->maxleafkey = WT_BTREE_MAX_OBJECT_SIZE;
		btree->maxleafvalue = WT_BTREE_MAX_OBJECT_SIZE;
		return (0);
	}

	/*
	 * In historic versions of WiredTiger, the maximum internal/leaf page
	 * key/value sizes were set by the internal_item_max and leaf_item_max
	 * configuration strings. Look for those strings if we don't find the
	 * newer ones.
	 */
	WT_RET(__wt_config_gets(session, cfg, "internal_key_max", &cval));
	btree->maxintlkey = (uint32_t)cval.val;
	if (btree->maxintlkey == 0) {
		WT_RET(
		    __wt_config_gets(session, cfg, "internal_item_max", &cval));
		btree->maxintlkey = (uint32_t)cval.val;
	}
	WT_RET(__wt_config_gets(session, cfg, "leaf_key_max", &cval));
	btree->maxleafkey = (uint32_t)cval.val;
	WT_RET(__wt_config_gets(session, cfg, "leaf_value_max", &cval));
	btree->maxleafvalue = (uint32_t)cval.val;
	if (btree->maxleafkey == 0 && btree->maxleafvalue == 0) {
		WT_RET(__wt_config_gets(session, cfg, "leaf_item_max", &cval));
		btree->maxleafkey = (uint32_t)cval.val;
		btree->maxleafvalue = (uint32_t)cval.val;
	}

	/*
	 * Default/maximum for internal and leaf page keys: split-page / 10.
	 * Default for leaf page values: split-page / 2.
	 *
	 * It's difficult for applications to configure this in any exact way as
	 * they have to duplicate our calculation of how many keys must fit on a
	 * page, and given a split-percentage and page header, that isn't easy
	 * to do. If the maximum internal key value is too large for the page,
	 * reset it to the default.
	 */
	if (btree->maxintlkey == 0 || btree->maxintlkey > intl_split_size / 10)
		    btree->maxintlkey = intl_split_size / 10;
	if (btree->maxleafkey == 0)
		    btree->maxleafkey = leaf_split_size / 10;
	if (btree->maxleafvalue == 0)
		    btree->maxleafvalue = leaf_split_size / 2;

	return (0);
}
