/*-
 * Copyright (c) 2014-present MongoDB, Inc.
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
 * __btree_clear --
 *     Clear a Btree, either on handle discard or re-open.
 */
static int
__btree_clear(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    btree = S2BT(session);

    /*
     * If the tree hasn't gone through an open/close cycle, there's no cleanup to be done.
     */
    if (!F_ISSET(btree, WT_BTREE_CLOSED))
        return (0);

    /* Close the Huffman tree. */
    __wt_btree_huffman_close(session);

    /* Terminate any associated collator. */
    if (btree->collator_owned && btree->collator->terminate != NULL)
        WT_TRET(btree->collator->terminate(btree->collator, &session->iface));

    /* Destroy locks. */
    __wt_rwlock_destroy(session, &btree->ovfl_lock);
    __wt_spin_destroy(session, &btree->flush_lock);

    /* Free allocated memory. */
    __wt_free(session, btree->key_format);
    __wt_free(session, btree->value_format);

    return (ret);
}

/*
 * __wt_btree_open --
 *     Open a Btree.
 */
int
__wt_btree_open(WT_SESSION_IMPL *session, const char *op_cfg[])
{
    WT_BLOCK_FILE_OPENER *opener;
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CKPT ckpt;
    WT_CONFIG_ITEM cval;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    size_t root_addr_size;
    uint8_t root_addr[WT_BTREE_MAX_ADDR_COOKIE];
    const char *filename;
    bool creation, forced_salvage;

    btree = S2BT(session);
    dhandle = session->dhandle;

    /*
     * This may be a re-open, clean up the btree structure. Clear the fields that don't persist
     * across a re-open. Clear all flags other than the operation flags (which are set by the
     * connection handle software that called us).
     */
    WT_RET(__btree_clear(session));
    memset(btree, 0, WT_BTREE_CLEAR_SIZE);
    F_CLR(btree, ~WT_BTREE_SPECIAL_FLAGS);

    /* Set the data handle first, our called functions reasonably use it. */
    btree->dhandle = dhandle;

    /* Checkpoint and verify files are readonly. */
    if (dhandle->checkpoint != NULL || F_ISSET(btree, WT_BTREE_VERIFY) ||
      F_ISSET(S2C(session), WT_CONN_READONLY))
        F_SET(btree, WT_BTREE_READONLY);

    /* Get the checkpoint information for this name/checkpoint pair. */
    WT_RET(__wt_meta_checkpoint(session, dhandle->name, dhandle->checkpoint, &ckpt));

    /*
     * Bulk-load is only permitted on newly created files, not any empty file -- see the checkpoint
     * code for a discussion.
     */
    creation = ckpt.raw.size == 0;
    if (!creation && F_ISSET(btree, WT_BTREE_BULK))
        WT_ERR_MSG(session, EINVAL, "bulk-load is only supported on newly created objects");

    /* Handle salvage configuration. */
    forced_salvage = false;
    if (F_ISSET(btree, WT_BTREE_SALVAGE)) {
        WT_ERR(__wt_config_gets(session, op_cfg, "force", &cval));
        forced_salvage = cval.val != 0;
    }

    /* Initialize and configure the WT_BTREE structure. */
    WT_ERR(__btree_conf(session, &ckpt));

    /*
     * Get an opener abstraction that the block manager can use to open any of the files that
     * represent a btree. In the case of a tiered Btree, that would allow opening different files
     * according to an object id in a reference. For a non-tiered Btree, the opener will know to
     * always open a single file (given by the filename).
     */
    WT_ERR(__wt_tiered_opener(session, dhandle, &opener, &filename));

    /* Connect to the underlying block manager. */
    WT_ERR(__wt_block_manager_open(session, filename, opener, dhandle->cfg, forced_salvage,
      F_ISSET(btree, WT_BTREE_READONLY), btree->allocsize, &btree->bm));

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
     * Open the specified checkpoint unless it's a special command (special commands are responsible
     * for loading their own checkpoints, if any).
     */
    if (!F_ISSET(btree, WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
        /*
         * There are two reasons to load an empty tree rather than a checkpoint: either there is no
         * checkpoint (the file is being created), or the load call returns no root page (the
         * checkpoint is for an empty file).
         */
        WT_ERR(bm->checkpoint_load(bm, session, ckpt.raw.data, ckpt.raw.size, root_addr,
          &root_addr_size, F_ISSET(btree, WT_BTREE_READONLY)));
        if (creation || root_addr_size == 0)
            WT_ERR(__btree_tree_open_empty(session, creation));
        else {
            WT_ERR(__wt_btree_tree_open(session, root_addr, root_addr_size));

            /* Warm the cache, if possible. */
            WT_WITH_PAGE_INDEX(session, ret = __btree_preload(session));
            WT_ERR(ret);

            /* Get the last record number in a column-store file. */
            if (btree->type != BTREE_ROW)
                WT_ERR(__btree_get_last_recno(session));
        }
    }

    /*
     * Eviction ignores trees until the handle's open flag is set, configure eviction before that
     * happens.
     *
     * Files that can still be bulk-loaded cannot be evicted. Permanently cache-resident files can
     * never be evicted. Special operations don't enable eviction. The underlying commands may turn
     * on eviction (for example, verify turns on eviction while working a file to keep from
     * consuming the cache), but it's their decision. If an underlying command reconfigures
     * eviction, it must either clear the evict-disabled-open flag or restore the eviction
     * configuration when finished so that handle close behaves correctly.
     */
    if (btree->original ||
      F_ISSET(btree, WT_BTREE_IN_MEMORY | WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY)) {
        WT_ERR(__wt_evict_file_exclusive_on(session));
        btree->evict_disabled_open = true;
    }

    if (0) {
err:
        WT_TRET(__wt_btree_close(session));
    }
    __wt_meta_checkpoint_free(session, &ckpt);

    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_btree_close --
 *     Close a Btree.
 */
int
__wt_btree_close(WT_SESSION_IMPL *session)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_RET;

    btree = S2BT(session);

    /*
     * The close process isn't the same as discarding the handle: we might re-open the handle, which
     * isn't a big deal, but the backing blocks for the handle may not yet have been discarded from
     * the cache, and eviction uses WT_BTREE structure elements. Free backing resources but leave
     * the rest alone, and we'll discard the structure when we discard the data handle.
     *
     * Handles can be closed multiple times, ignore all but the first.
     */
    if (F_ISSET(btree, WT_BTREE_CLOSED))
        return (0);
    F_SET(btree, WT_BTREE_CLOSED);

    /*
     * Verify the history store state. If the history store is open and this btree has history store
     * entries, it can't be a metadata file, nor can it be the history store file.
     */
    WT_ASSERT(session,
      !F_ISSET(S2C(session), WT_CONN_HS_OPEN) || !btree->hs_entries ||
        (!WT_IS_METADATA(btree->dhandle) && !WT_IS_HS(btree->dhandle)));

    /* Clear the saved checkpoint information. */
    __wt_meta_saved_ckptlist_free(session);

    /*
     * If we turned eviction off and never turned it back on, do that now, otherwise the counter
     * will be off.
     */
    if (btree->evict_disabled_open) {
        btree->evict_disabled_open = false;
        __wt_evict_file_exclusive_off(session);
    }

    /* Discard any underlying block manager resources. */
    if ((bm = btree->bm) != NULL) {
        btree->bm = NULL;

        /* Unload the checkpoint, unless it's a special command. */
        if (!F_ISSET(btree, WT_BTREE_SALVAGE | WT_BTREE_UPGRADE | WT_BTREE_VERIFY))
            WT_TRET(bm->checkpoint_unload(bm, session));

        /* Close the underlying block manager reference. */
        WT_TRET(bm->close(bm, session));
    }

    return (ret);
}

/*
 * __wt_btree_discard --
 *     Discard a Btree.
 */
int
__wt_btree_discard(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_DECL_RET;

    ret = __btree_clear(session);

    btree = S2BT(session);
    __wt_overwrite_and_free(session, btree);
    session->dhandle->handle = NULL;

    return (ret);
}

/*
 * __wt_btree_config_encryptor --
 *     Return an encryptor handle based on the configuration.
 */
int
__wt_btree_config_encryptor(
  WT_SESSION_IMPL *session, const char **cfg, WT_KEYED_ENCRYPTOR **kencryptorp)
{
    WT_CONFIG_ITEM cval, enc, keyid;
    WT_DECL_RET;
    const char *enc_cfg[] = {NULL, NULL};

    /*
     * We do not use __wt_config_gets_none here because "none" and the empty string have different
     * meanings. The empty string means inherit the system encryption setting and "none" means this
     * table is in the clear even if the database is encrypted.
     */
    WT_RET(__wt_config_gets(session, cfg, "encryption.name", &cval));
    if (cval.len == 0)
        *kencryptorp = S2C(session)->kencryptor;
    else if (WT_STRING_MATCH("none", cval.str, cval.len))
        *kencryptorp = NULL;
    else {
        WT_RET(__wt_config_gets_none(session, cfg, "encryption.keyid", &keyid));
        WT_RET(__wt_config_gets(session, cfg, "encryption", &enc));
        if (enc.len != 0)
            WT_RET(__wt_strndup(session, enc.str, enc.len, &enc_cfg[0]));
        ret = __wt_encryptor_config(session, &cval, &keyid, (WT_CONFIG_ARG *)enc_cfg, kencryptorp);
        __wt_free(session, enc_cfg[0]);
        WT_RET(ret);
    }
    return (0);
}

/*
 * __btree_conf --
 *     Configure a WT_BTREE structure.
 */
static int
__btree_conf(WT_SESSION_IMPL *session, WT_CKPT *ckpt)
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval, metadata;
    WT_CONNECTION_IMPL *conn;
    int64_t maj_version, min_version;
    uint32_t bitcnt;
    const char **cfg;
    bool fixed;

    btree = S2BT(session);
    cfg = btree->dhandle->cfg;
    conn = S2C(session);

    /* Dump out format information. */
    if (WT_VERBOSE_ISSET(session, WT_VERB_VERSION)) {
        WT_RET(__wt_config_gets(session, cfg, "version.major", &cval));
        maj_version = cval.val;
        WT_RET(__wt_config_gets(session, cfg, "version.minor", &cval));
        min_version = cval.val;
        __wt_verbose(session, WT_VERB_VERSION, "%" PRId64 ".%" PRId64, maj_version, min_version);
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

    /* Row-store key comparison. */
    if (btree->type == BTREE_ROW) {
        WT_RET(__wt_config_gets_none(session, cfg, "collator", &cval));
        if (cval.len != 0) {
            WT_RET(__wt_config_gets(session, cfg, "app_metadata", &metadata));
            WT_RET(__wt_collator_config(session, btree->dhandle->name, &cval, &metadata,
              &btree->collator, &btree->collator_owned));
        }
    }

    /* Column-store: check for fixed-size data. */
    if (btree->type == BTREE_COL_VAR) {
        WT_RET(__wt_struct_check(session, cval.str, cval.len, &fixed, &bitcnt));
        if (fixed) {
            if (bitcnt == 0 || bitcnt > 8)
                WT_RET_MSG(session, EINVAL,
                  "fixed-width field sizes must be greater than 0 and less than or equal to 8");
            btree->bitcnt = (uint8_t)bitcnt;
            btree->type = BTREE_COL_FIX;
        }
    }

    /* Page sizes */
    WT_RET(__btree_page_sizes(session));

    WT_RET(__wt_config_gets(session, cfg, "cache_resident", &cval));
    if (cval.val)
        F_SET(btree, WT_BTREE_IN_MEMORY);
    else
        F_CLR(btree, WT_BTREE_IN_MEMORY);

    WT_RET(__wt_config_gets(session, cfg, "ignore_in_memory_cache_size", &cval));
    if (cval.val) {
        if (!F_ISSET(conn, WT_CONN_IN_MEMORY))
            WT_RET_MSG(session, EINVAL,
              "ignore_in_memory_cache_size setting is only valid with databases configured to run "
              "in-memory");
        F_SET(btree, WT_BTREE_IGNORE_CACHE);
    } else
        F_CLR(btree, WT_BTREE_IGNORE_CACHE);

    /*
     * The metadata isn't blocked by in-memory cache limits because metadata "unroll" is performed
     * by updates that are potentially blocked by the cache-full checks.
     */
    if (WT_IS_METADATA(btree->dhandle))
        F_SET(btree, WT_BTREE_IGNORE_CACHE);

    WT_RET(__wt_config_gets(session, cfg, "log.enabled", &cval));
    if (cval.val)
        F_CLR(btree, WT_BTREE_NO_LOGGING);
    else
        F_SET(btree, WT_BTREE_NO_LOGGING);

    WT_RET(__wt_config_gets(session, cfg, "tiered_object", &cval));
    if (cval.val)
        F_SET(btree, WT_BTREE_NO_CHECKPOINT);
    else
        F_CLR(btree, WT_BTREE_NO_CHECKPOINT);

    /* Checksums */
    WT_RET(__wt_config_gets(session, cfg, "checksum", &cval));
    if (WT_STRING_MATCH("on", cval.str, cval.len))
        btree->checksum = CKSUM_ON;
    else if (WT_STRING_MATCH("off", cval.str, cval.len))
        btree->checksum = CKSUM_OFF;
    else if (WT_STRING_MATCH("uncompressed", cval.str, cval.len))
        btree->checksum = CKSUM_UNCOMPRESSED;
    else
        btree->checksum = CKSUM_UNENCRYPTED;

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
        WT_RET(__wt_config_gets(session, cfg, "internal_key_truncate", &cval));
        btree->internal_key_truncate = cval.val != 0;

        WT_RET(__wt_config_gets(session, cfg, "prefix_compression", &cval));
        btree->prefix_compression = cval.val != 0;
        WT_RET(__wt_config_gets(session, cfg, "prefix_compression_min", &cval));
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
     * Configure compression adjustment.
     * When doing compression, assume compression rates that will result in
     * pages larger than the maximum in-memory images allowed. If we're
     * wrong, we adjust downward (but we're almost certainly correct, the
     * maximum in-memory images allowed are only 4x the maximum page size,
     * and compression always gives us more than 4x).
     *	Don't do compression adjustment for fixed-size column store, the
     * leaf page sizes don't change. (We could adjust internal pages but not
     * internal pages, but that seems an unlikely use case.)
     */
    btree->intlpage_compadjust = false;
    btree->maxintlpage_precomp = btree->maxintlpage;
    btree->leafpage_compadjust = false;
    btree->maxleafpage_precomp = btree->maxleafpage;
    if (btree->compressor != NULL && btree->compressor->compress != NULL &&
      btree->type != BTREE_COL_FIX) {
        /*
         * Don't do compression adjustment when on-disk page sizes are less than 16KB. There's not
         * enough compression going on to fine-tune the size, all we end up doing is hammering
         * shared memory.
         *
         * Don't do compression adjustment when on-disk page sizes are equal to the maximum
         * in-memory page image, the bytes taken for compression can't grow past the base value.
         */
        if (btree->maxintlpage >= 16 * 1024 && btree->maxmempage_image > btree->maxintlpage) {
            btree->intlpage_compadjust = true;
            btree->maxintlpage_precomp = btree->maxmempage_image;
        }
        if (btree->maxleafpage >= 16 * 1024 && btree->maxmempage_image > btree->maxleafpage) {
            btree->leafpage_compadjust = true;
            btree->maxleafpage_precomp = btree->maxmempage_image;
        }
    }

    /* Set special flags for the history store table. */
    if (strcmp(session->dhandle->name, WT_HS_URI) == 0) {
        F_SET(btree->dhandle, WT_DHANDLE_HS);
        F_SET(btree, WT_BTREE_NO_LOGGING);
    }

    /* Configure encryption. */
    WT_RET(__wt_btree_config_encryptor(session, cfg, &btree->kencryptor));

    /* Configure read-only. */
    WT_RET(__wt_config_gets(session, cfg, "readonly", &cval));
    if (cval.val)
        F_SET(btree, WT_BTREE_READONLY);

    /* Initialize locks. */
    WT_RET(__wt_rwlock_init(session, &btree->ovfl_lock));
    WT_RET(__wt_spin_init(session, &btree->flush_lock, "btree flush"));

    btree->modified = false; /* Clean */

    btree->syncing = WT_BTREE_SYNC_OFF;                           /* Not syncing */
    btree->checkpoint_gen = __wt_gen(session, WT_GEN_CHECKPOINT); /* Checkpoint generation */

    /*
     * The first time we open a btree, we'll be initializing the write gen to the connection-wide
     * base write generation since this is the largest of all btree write generations from the
     * previous run. This has the nice property of ensuring that the range of write generations used
     * by consecutive runs do not overlap which aids with debugging.
     *
     * If we're reopening a btree or importing a new one to a running system, the btree write
     * generation from the last run may actually be ahead of the connection-wide base write
     * generation. In that case, we should initialize our write gen just ahead of our btree specific
     * write generation.
     *
     * The runtime write generation is important since it's going to determine what we're going to
     * use as the base write generation (and thus what pages to wipe transaction ids from). The idea
     * is that we want to initialize it once the first time we open the btree during a run and then
     * for every subsequent open, we want to reuse it. This so that we're still able to read
     * transaction ids from the previous time a btree was open in the same run.
     *
     * FIXME-WT-6819: When we begin discarding dhandles more aggressively, we need to check that
     * updates aren't having their transaction ids wiped after reopening the dhandle. The runtime
     * write generation is relevant here since it should remain static across the entire run.
     */
    btree->write_gen = WT_MAX(ckpt->write_gen + 1, conn->base_write_gen);
    WT_ASSERT(session, ckpt->write_gen >= ckpt->run_write_gen);

    /* If this is the first time opening the tree this run. */
    if (F_ISSET(session, WT_SESSION_IMPORT) || ckpt->run_write_gen < conn->base_write_gen)
        btree->run_write_gen = btree->write_gen;
    else
        btree->run_write_gen = ckpt->run_write_gen;

    /*
     * In recovery use the last checkpointed run write generation number as base write generation
     * number to reset the transaction ids of the pages that were modified before the restart. The
     * transaction ids are retained only on the pages that are written after the restart.
     *
     * Rollback to stable does not operate on logged tables and metadata, so it is skipped.
     *
     * The only scenario where the checkpoint run write generation number is less than the
     * connection last checkpoint base write generation number is when rollback to stable doesn't
     * happen during the recovery due to the unavailability of history store file.
     */
    if (!F_ISSET(conn, WT_CONN_RECOVERING) || WT_IS_METADATA(btree->dhandle) ||
      __wt_btree_immediately_durable(session) ||
      ckpt->run_write_gen < conn->last_ckpt_base_write_gen)
        btree->base_write_gen = btree->run_write_gen;
    else
        btree->base_write_gen = ckpt->run_write_gen;

    /*
     * We've just overwritten the runtime write generation based off the fact that know that we're
     * importing and therefore, the checkpoint data's runtime write generation is meaningless. We
     * need to ensure that the underlying dhandle doesn't get discarded without being included in a
     * subsequent checkpoint including the new overwritten runtime write generation. Otherwise,
     * we'll reopen, won't know that we're in the import case and will incorrectly use the old
     * system's runtime write generation.
     */
    if (F_ISSET(session, WT_SESSION_IMPORT))
        btree->modified = true;

    return (0);
}

/*
 * __wt_root_ref_init --
 *     Initialize a tree root reference, and link in the root page.
 */
void
__wt_root_ref_init(WT_SESSION_IMPL *session, WT_REF *root_ref, WT_PAGE *root, bool is_recno)
{
    WT_UNUSED(session); /* Used in a macro for diagnostic builds */
    memset(root_ref, 0, sizeof(*root_ref));

    root_ref->page = root;
    F_SET(root_ref, WT_REF_FLAG_INTERNAL);
    WT_REF_SET_STATE(root_ref, WT_REF_MEM);

    root_ref->ref_recno = is_recno ? 1 : WT_RECNO_OOB;

    root->pg_intl_parent_ref = root_ref;
}

/*
 * __wt_btree_tree_open --
 *     Read in a tree from disk.
 */
int
__wt_btree_tree_open(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size)
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
     * A buffer into which we read a root page; don't use a scratch buffer, the buffer's allocated
     * memory becomes the persistent in-memory page.
     */
    WT_CLEAR(dsk);

    /*
     * Read and verify the page (verify to catch encrypted objects we can't decrypt, where we read
     * the object successfully but we can't decrypt it, and we want to fail gracefully).
     *
     * Create a printable version of the address to pass to verify.
     */
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(bm->addr_string(bm, session, tmp, addr, addr_size));

    F_SET(session, WT_SESSION_QUIET_CORRUPT_FILE);
    if ((ret = __wt_blkcache_read(session, &dsk, addr, addr_size)) == 0)
        ret = __wt_verify_dsk(session, tmp->data, &dsk);
    /*
     * Flag any failed read or verification: if we're in startup, it may be fatal.
     */
    if (ret != 0)
        F_SET(S2C(session), WT_CONN_DATA_CORRUPTION);
    F_CLR(session, WT_SESSION_QUIET_CORRUPT_FILE);
    if (ret != 0)
        __wt_err(session, ret, "unable to read root page from %s", session->dhandle->name);
    /*
     * Failure to open metadata means that the database is unavailable. Try to provide a helpful
     * failure message.
     */
    if (ret != 0 && WT_IS_METADATA(session->dhandle)) {
        __wt_err(session, ret, "WiredTiger has failed to open its metadata");
        __wt_err(session, ret,
          "This may be due to the database files being encrypted, being from an older version or "
          "due to corruption on disk");
        __wt_err(session, ret,
          "You should confirm that you have opened the database with the correct options including "
          "all encryption and compression options");
    }
    WT_ERR(ret);

    /*
     * Build the in-memory version of the page. Clear our local reference to the allocated copy of
     * the disk image on return, the in-memory object steals it.
     */
    WT_ERR(__wt_page_inmem(session, NULL, dsk.data,
      WT_DATA_IN_ITEM(&dsk) ? WT_PAGE_DISK_ALLOC : WT_PAGE_DISK_MAPPED, &page, NULL));
    dsk.mem = NULL;

    /* Finish initializing the root, root reference links. */
    __wt_root_ref_init(session, &btree->root, page, btree->type != BTREE_ROW);

err:
    __wt_buf_free(session, &dsk);
    __wt_scr_free(session, &tmp);

    return (ret);
}

/*
 * __btree_tree_open_empty --
 *     Create an empty in-memory tree.
 */
static int
__btree_tree_open_empty(WT_SESSION_IMPL *session, bool creation)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *root;
    WT_PAGE_INDEX *pindex;
    WT_REF *ref;

    btree = S2BT(session);
    root = NULL;
    ref = NULL;

    /*
     * Newly created objects can be used for cursor inserts or for bulk loads; set a flag that's
     * cleared when a row is inserted into the tree.
     */
    if (creation)
        btree->original = 1;

    /*
     * A note about empty trees: the initial tree is a single root page. It has a single reference
     * to a leaf page, marked deleted. The leaf page will be created by the first update. If the
     * root is evicted without being modified, that's OK, nothing is ever written.
     *
     * !!!
     * Be cautious about changing the order of updates in this code: to call __wt_page_out on error,
     * we require a correct page setup at each point where we might fail.
     */
    switch (btree->type) {
    case BTREE_COL_FIX:
    case BTREE_COL_VAR:
        WT_ERR(__wt_page_alloc(session, WT_PAGE_COL_INT, 1, true, &root));
        root->pg_intl_parent_ref = &btree->root;

        pindex = WT_INTL_INDEX_GET_SAFE(root);
        ref = pindex->index[0];
        ref->home = root;
        ref->page = NULL;
        ref->addr = NULL;
        F_SET(ref, WT_REF_FLAG_LEAF);
        WT_REF_SET_STATE(ref, WT_REF_DELETED);
        ref->ref_recno = 1;
        break;
    case BTREE_ROW:
        WT_ERR(__wt_page_alloc(session, WT_PAGE_ROW_INT, 1, true, &root));
        root->pg_intl_parent_ref = &btree->root;

        pindex = WT_INTL_INDEX_GET_SAFE(root);
        ref = pindex->index[0];
        ref->home = root;
        ref->page = NULL;
        ref->addr = NULL;
        F_SET(ref, WT_REF_FLAG_LEAF);
        WT_REF_SET_STATE(ref, WT_REF_DELETED);
        WT_ERR(__wt_row_ikey_incr(session, root, 0, "", 1, ref));
        break;
    }

    /* Bulk loads require a leaf page for reconciliation: create it now. */
    if (F_ISSET(btree, WT_BTREE_BULK)) {
        WT_ERR(__wt_btree_new_leaf_page(session, ref));
        F_SET(ref, WT_REF_FLAG_LEAF);
        WT_REF_SET_STATE(ref, WT_REF_MEM);
        WT_ERR(__wt_page_modify_init(session, ref->page));
        __wt_page_only_modify_set(session, ref->page);
    }

    /* Finish initializing the root, root reference links. */
    __wt_root_ref_init(session, &btree->root, root, btree->type != BTREE_ROW);

    return (0);

err:
    if (ref != NULL && ref->page != NULL)
        __wt_page_out(session, &ref->page);
    if (root != NULL)
        __wt_page_out(session, &root);
    return (ret);
}

/*
 * __wt_btree_new_leaf_page --
 *     Create an empty leaf page.
 */
int
__wt_btree_new_leaf_page(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    switch (btree->type) {
    case BTREE_COL_FIX:
        WT_RET(__wt_page_alloc(session, WT_PAGE_COL_FIX, 0, false, &ref->page));
        break;
    case BTREE_COL_VAR:
        WT_RET(__wt_page_alloc(session, WT_PAGE_COL_VAR, 0, false, &ref->page));
        break;
    case BTREE_ROW:
        WT_RET(__wt_page_alloc(session, WT_PAGE_ROW_LEAF, 0, false, &ref->page));
        break;
    }

    /*
     * When deleting a chunk of the name-space, we can delete internal pages. However, if we are
     * ever forced to re-instantiate that piece of the namespace, it comes back as a leaf page.
     * Reset the WT_REF type as it's possible that it has changed.
     */
    F_CLR(ref, WT_REF_FLAG_INTERNAL);
    F_SET(ref, WT_REF_FLAG_LEAF);

    return (0);
}

/*
 * __btree_preload --
 *     Pre-load internal pages.
 */
static int
__btree_preload(WT_SESSION_IMPL *session)
{
    WT_ADDR_COPY addr;
    WT_BTREE *btree;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_REF *ref;
    uint64_t block_preload;

    btree = S2BT(session);
    block_preload = 0;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));

    /* Pre-load the second-level internal pages. */
    WT_INTL_FOREACH_BEGIN (session, btree->root.page, ref)
        if (__wt_ref_addr_copy(session, ref, &addr)) {
            WT_ERR(__wt_blkcache_read(session, tmp, addr.addr, addr.size));
            ++block_preload;
        }
    WT_INTL_FOREACH_END;

err:
    __wt_scr_free(session, &tmp);

    WT_STAT_CONN_INCRV(session, block_preload, block_preload);
    return (ret);
}

/*
 * __btree_get_last_recno --
 *     Set the last record number for a column-store.
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
    btree->last_recno = page->type == WT_PAGE_COL_VAR ? __col_var_last_recno(next_walk) :
                                                        __col_fix_last_recno(next_walk);

    return (__wt_page_release(session, next_walk, 0));
}

/*
 * __btree_page_sizes --
 *     Verify the page sizes. Some of these sizes are automatically checked using limits defined in
 *     the API, don't duplicate the logic here.
 */
static int
__btree_page_sizes(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint64_t cache_size;
    uint32_t leaf_split_size, max;
    const char **cfg;

    btree = S2BT(session);
    conn = S2C(session);
    cfg = btree->dhandle->cfg;

    /*
     * Get the allocation size. Allocation sizes must be a power-of-two, nothing else makes sense.
     */
    WT_RET(__wt_direct_io_size_check(session, cfg, "allocation_size", &btree->allocsize));
    if (!__wt_ispo2(btree->allocsize))
        WT_RET_MSG(session, EINVAL, "the allocation size must be a power of two");

    /*
     * Get the internal/leaf page sizes. All page sizes must be in units of the allocation size.
     */
    WT_RET(__wt_direct_io_size_check(session, cfg, "internal_page_max", &btree->maxintlpage));
    WT_RET(__wt_direct_io_size_check(session, cfg, "leaf_page_max", &btree->maxleafpage));
    if (btree->maxintlpage < btree->allocsize || btree->maxintlpage % btree->allocsize != 0 ||
      btree->maxleafpage < btree->allocsize || btree->maxleafpage % btree->allocsize != 0)
        WT_RET_MSG(session, EINVAL,
          "page sizes must be a multiple of the page allocation size (%" PRIu32 "B)",
          btree->allocsize);

    /*
     * Default in-memory page image size for compression is 4x the maximum internal or leaf page
     * size, and enforce the on-disk page sizes as a lower-limit for the in-memory image size.
     */
    WT_RET(__wt_config_gets(session, cfg, "memory_page_image_max", &cval));
    btree->maxmempage_image = (uint32_t)cval.val;
    max = WT_MAX(btree->maxintlpage, btree->maxleafpage);
    if (btree->maxmempage_image == 0)
        btree->maxmempage_image = 4 * max;
    else if (btree->maxmempage_image < max)
        WT_RET_MSG(session, EINVAL,
          "in-memory page image size must be larger than the maximum page size (%" PRIu32
          "B < %" PRIu32 "B)",
          btree->maxmempage_image, max);

    /*
     * Don't let pages grow large compared to the cache size or we can end
     * up in a situation where nothing can be evicted.  Make sure at least
     * 10 pages fit in cache when it is at the dirty trigger where threads
     * stall.
     *
     * Take care getting the cache size: with a shared cache, it may not
     * have been set.  Don't forget to update the API documentation if you
     * alter the bounds for any of the parameters here.
     */
    WT_RET(__wt_config_gets(session, cfg, "memory_page_max", &cval));
    btree->maxmempage = (uint64_t)cval.val;
    if (!F_ISSET(conn, WT_CONN_CACHE_POOL) && (cache_size = conn->cache_size) > 0)
        btree->maxmempage = (uint64_t)WT_MIN(
          btree->maxmempage, (conn->cache->eviction_dirty_trigger * cache_size) / 1000);

    /* Enforce a lower bound of a single disk leaf page */
    btree->maxmempage = WT_MAX(btree->maxmempage, btree->maxleafpage);

    /*
     * Try in-memory splits once we hit 80% of the maximum in-memory page size. This gives
     * multi-threaded append workloads a better chance of not stalling.
     */
    btree->splitmempage = (8 * btree->maxmempage) / 10;

    /*
     * Get the split percentage (reconciliation splits pages into smaller than the maximum page size
     * chunks so we don't split every time a new entry is added). Determine how large newly split
     * pages will be. Set to the minimum, if the read value is less than that.
     */
    WT_RET(__wt_config_gets(session, cfg, "split_pct", &cval));
    if (cval.val < WT_BTREE_MIN_SPLIT_PCT) {
        btree->split_pct = WT_BTREE_MIN_SPLIT_PCT;
        __wt_verbose_notice(session, WT_VERB_SPLIT,
          "Re-setting split_pct for %s to the minimum allowed of %d%%", session->dhandle->name,
          WT_BTREE_MIN_SPLIT_PCT);
    } else
        btree->split_pct = (int)cval.val;
    leaf_split_size = __wt_split_page_size(btree->split_pct, btree->maxleafpage, btree->allocsize);

    /*
     * In-memory split configuration.
     */
    if (__wt_config_gets(session, cfg, "split_deepen_min_child", &cval) == WT_NOTFOUND ||
      cval.val == 0)
        btree->split_deepen_min_child = WT_SPLIT_DEEPEN_MIN_CHILD_DEF;
    else
        btree->split_deepen_min_child = (u_int)cval.val;
    if (__wt_config_gets(session, cfg, "split_deepen_per_child", &cval) == WT_NOTFOUND ||
      cval.val == 0)
        btree->split_deepen_per_child = WT_SPLIT_DEEPEN_PER_CHILD_DEF;
    else
        btree->split_deepen_per_child = (u_int)cval.val;

    /*
     * Get the maximum internal/leaf page key/value sizes.
     *
     * In-memory configuration overrides any key/value sizes, there's no such thing as an overflow
     * item in an in-memory configuration.
     */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY)) {
        btree->maxleafkey = WT_BTREE_MAX_OBJECT_SIZE;
        btree->maxleafvalue = WT_BTREE_MAX_OBJECT_SIZE;
        return (0);
    }

    WT_RET(__wt_config_gets(session, cfg, "leaf_key_max", &cval));
    btree->maxleafkey = (uint32_t)cval.val;
    WT_RET(__wt_config_gets(session, cfg, "leaf_value_max", &cval));
    btree->maxleafvalue = (uint32_t)cval.val;

    /*
     * Default max for leaf keys: split-page / 10. Default max for leaf values: split-page / 2.
     *
     * It's difficult for applications to configure this in any exact way as they have to duplicate
     * our calculation of how many keys must fit on a page, and given a split-percentage and page
     * header, that isn't easy to do.
     */
    if (btree->maxleafkey == 0)
        btree->maxleafkey = leaf_split_size / 10;
    if (btree->maxleafvalue == 0)
        btree->maxleafvalue = leaf_split_size / 2;

    return (0);
}

/*
 * __wt_btree_immediately_durable --
 *     Check whether this btree is configured for immediate durability.
 */
bool
__wt_btree_immediately_durable(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    /*
     * This is used to determine whether timestamp updates should be rolled back for this btree.
     * With in-memory, the logging setting on tables is still important and when enabled they should
     * be considered "durable".
     */
    return ((FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED) ||
              (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))) &&
      !F_ISSET(btree, WT_BTREE_NO_LOGGING));
}

/*
 * __wt_btree_switch_object --
 *     Switch to a writeable object for a tiered btree.
 */
int
__wt_btree_switch_object(WT_SESSION_IMPL *session, uint32_t object_id, uint32_t flags)
{
    WT_BM *bm;
    WT_DECL_RET;

    bm = S2BT(session)->bm;

    /*
     * When initially opening a tiered Btree, a tier switch is done internally without the btree
     * being fully opened. That's okay, the btree will be told later about the current object
     * number.
     */
    if (bm != NULL)
        ret = bm->switch_object(bm, session, object_id, flags);

    return (ret);
}
