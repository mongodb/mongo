/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * There's a bunch of stuff we pass around during verification, group it together to make the code
 * prettier.
 */
typedef struct {
    uint64_t records_so_far; /* Records seen so far */

    WT_ITEM *max_key;  /* Largest key */
    WT_ITEM *max_addr; /* Largest key page */

#define WT_VERIFY_PROGRESS_INTERVAL 100
    uint64_t fcnt; /* Progress counter */

    /* Configuration options passed in. */
    wt_timestamp_t stable_timestamp; /* Stable timestamp to verify against if desired */
#define WT_VRFY_DUMP(vs) \
    ((vs)->dump_address || (vs)->dump_blocks || (vs)->dump_layout || (vs)->dump_pages)
    bool dump_address; /* Configure: dump special */
    bool dump_all_data;
    bool dump_key_data;
    bool dump_blocks;
    bool dump_layout, dump_tree_shape;
    bool dump_pages;
    bool read_corrupt;

    /* Page layout information. */
    uint64_t depth, depth_internal[100], depth_leaf[100], tree_stack[100], keys_count_stack[100],
      key_sz_stack[100], val_sz_stack[100], total_sz_stack[100];

    WT_ITEM *tmp1, *tmp2, *tmp3, *tmp4; /* Temporary buffers */

    int verify_err;
} WT_VSTUFF;

static void __verify_checkpoint_reset(WT_VSTUFF *);
static int __verify_compare_page_id(const void *, const void *);
static int __verify_page_content_fix(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
static int __verify_page_content_int(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
static int __verify_page_content_leaf(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
static int __verify_page_discard(WT_SESSION_IMPL *, WT_BM *);
static int __verify_row_int_key_order(
  WT_SESSION_IMPL *, WT_PAGE *, WT_REF *, uint32_t, WT_VSTUFF *);
static int __verify_row_leaf_key_order(WT_SESSION_IMPL *, WT_REF *, WT_VSTUFF *);
static int __verify_tree(WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);

/*
 * __verify_config --
 *     Debugging: verification supports dumping pages in various formats.
 */
static int
__verify_config(WT_SESSION_IMPL *session, const char *cfg[], WT_VSTUFF *vs)
{
    WT_CONFIG_ITEM cval;
    WT_TXN_GLOBAL *txn_global;

    txn_global = &S2C(session)->txn_global;

    WT_RET(__wt_config_gets(session, cfg, "do_not_clear_txn_id", &cval));
    if (cval.val)
        F_SET(session, WT_SESSION_DEBUG_DO_NOT_CLEAR_TXN_ID);
    else
        F_CLR(session, WT_SESSION_DEBUG_DO_NOT_CLEAR_TXN_ID);

    WT_RET(__wt_config_gets(session, cfg, "dump_address", &cval));
    vs->dump_address = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_all_data", &cval));
    vs->dump_all_data = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_key_data", &cval));
    vs->dump_key_data = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_blocks", &cval));
    vs->dump_blocks = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_layout", &cval));
    vs->dump_layout = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_tree_shape", &cval));
    vs->dump_tree_shape = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_pages", &cval));
    vs->dump_pages = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "read_corrupt", &cval));
    vs->read_corrupt = cval.val != 0;
    vs->verify_err = 0;

    WT_RET(__wt_config_gets(session, cfg, "stable_timestamp", &cval));
    vs->stable_timestamp = WT_TS_NONE; /* Ignored unless a value has been set */
    if (cval.val != 0) {
        if (!txn_global->has_stable_timestamp)
            WT_RET_MSG(session, ENOTSUP,
              "cannot verify against the stable timestamp if it has not been set");
        vs->stable_timestamp = txn_global->stable_timestamp;
    }
    if (vs->dump_all_data && vs->dump_key_data)
        WT_RET_MSG(session, ENOTSUP, "%s",
          "dump_all_data, which unredacts all data, should not be set to true "
          "simultaneously with dump_key_data, which unredacts only the keys");

#if !defined(HAVE_DIAGNOSTIC)
    if (vs->dump_blocks || vs->dump_pages)
        WT_RET_MSG(session, ENOTSUP, "the WiredTiger library was not built in diagnostic mode");
#endif

    return (0);
}

/*
 * __verify_config_offsets --
 *     Debugging: optionally dump specific blocks from the file.
 */
static int
__verify_config_offsets(WT_SESSION_IMPL *session, const char *cfg[], bool *quitp
#ifdef HAVE_DIAGNOSTIC
  ,
  WT_VSTUFF *vs)
#else
)
#endif
{
    WT_CONFIG list;
    WT_CONFIG_ITEM cval, k, v;
    WT_DECL_RET;
    uint64_t offset;

    *quitp = false;

    WT_RET(__wt_config_gets(session, cfg, "dump_offsets", &cval));
    __wt_config_subinit(session, &list, &cval);
    while ((ret = __wt_config_next(&list, &k, &v)) == 0) {
        /*
         * Quit after dumping the requested blocks. (That's hopefully what the user wanted, all of
         * this stuff is just hooked into verify because that's where we "dump blocks" for
         * debugging.)
         */
        *quitp = true;
        /* NOLINTNEXTLINE(cert-err34-c) */
        if (v.len != 0 || sscanf(k.str, "%" SCNu64, &offset) != 1)
            WT_RET_MSG(session, EINVAL, "unexpected dump offset format");
#if !defined(HAVE_DIAGNOSTIC)
        WT_RET_MSG(session, ENOTSUP, "the WiredTiger library was not built in diagnostic mode");
#else
        WT_TRET(__wti_debug_offset_blind(
          session, (wt_off_t)offset, NULL, vs->dump_all_data, vs->dump_key_data));
#endif
    }
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __dump_layout --
 *     Dump the tree shape.
 */
static int
__dump_layout(WT_SESSION_IMPL *session, WT_VSTUFF *vs)
{
    size_t i;
    uint64_t total;

    for (i = 0, total = 0; i < WT_ELEMENTS(vs->depth_internal); ++i)
        total += vs->depth_internal[i];
    WT_RET(__wt_msg(session, "Internal page tree-depth (total %" PRIu64 "):", total));
    for (i = 0; i < WT_ELEMENTS(vs->depth_internal); ++i)
        if (vs->depth_internal[i] != 0) {
            WT_RET(__wt_msg(session, "\t%03" WT_SIZET_FMT ": %" PRIu64, i, vs->depth_internal[i]));
            vs->depth_internal[i] = 0;
        }

    for (i = 0, total = 0; i < WT_ELEMENTS(vs->depth_leaf); ++i)
        total += vs->depth_leaf[i];
    WT_RET(__wt_msg(session, "Leaf page tree-depth (total %" PRIu64 "):", total));
    for (i = 0; i < WT_ELEMENTS(vs->depth_leaf); ++i)
        if (vs->depth_leaf[i] != 0) {
            WT_RET(__wt_msg(session, "\t%03" WT_SIZET_FMT ": %" PRIu64, i, vs->depth_leaf[i]));
            vs->depth_leaf[i] = 0;
        }
    return (0);
}

/*
 * __wt_verify --
 *     Verify a file.
 */
int
__wt_verify(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL_UNPACK_ADDR addr_unpack;
    WT_CKPT *ckptbase, *ckpt;
    WT_DECL_RET;
    WT_VSTUFF *vs, _vstuff;
    size_t root_addr_size;
    uint8_t root_addr[WT_ADDR_MAX_COOKIE];
    const char *name;
    bool bm_start, quit, skip_hs;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    btree = S2BT(session);
    bm = btree->bm;
    ckptbase = NULL;
    name = session->dhandle->name;
    bm_start = quit = false;
    WT_NOT_READ(skip_hs, false);

    WT_CLEAR(_vstuff);
    vs = &_vstuff;
    WT_ERR(__wt_scr_alloc(session, 0, &vs->max_key));
    WT_ERR(__wt_scr_alloc(session, 0, &vs->max_addr));
    WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &vs->tmp1));
    WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp2));
    WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp3));
    WT_ERR(__wt_scr_alloc(session, 0, &vs->tmp4));

    /* Check configuration strings. */
    WT_ERR(__verify_config(session, cfg, vs));

    /* Optionally dump specific block offsets. */
#ifdef HAVE_DIAGNOSTIC
    WT_ERR(__verify_config_offsets(session, cfg, &quit, vs));
#else
    WT_ERR(__verify_config_offsets(session, cfg, &quit));
#endif
    if (quit)
        goto done;

    /*
     * Get a list of the checkpoints for this file. Empty objects and ingest tables have no
     * checkpoints, in which case there's no work to do.
     */
    WT_ERR_NOTFOUND_OK(__wt_meta_ckptlist_get(session, name, false, &ckptbase, NULL), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    } else if (WT_SUFFIX_MATCH(name, ".wt_ingest"))
        WT_ERR_MSG(session, WT_ERROR,
          "verify (layered): ingest table %s unexpectedly has checkpoints. This is a fatal "
          "violation as the ingest table does not get checkpointed.",
          name);

    /* Inform the underlying block manager we're verifying. */
    WT_ERR(bm->verify_start(bm, session, ckptbase, cfg));
    bm_start = true;

    /*
     * Skip the history store explicit call if:
     * - we are performing a metadata verification. Indeed, the metadata file is verified
     * before we verify the history store, and it makes no sense to verify the history store against
     * itself.
     * - the debug flag is set where we do not clear the record's txn IDs. Visibility rules may not
     * work correctly when we do not clear the record's txn IDs.
     */
    skip_hs = strcmp(name, WT_METAFILE_URI) == 0 || WT_IS_URI_HS(name) ||
      F_ISSET(session, WT_SESSION_DEBUG_DO_NOT_CLEAR_TXN_ID);

    /* Loop through the file's checkpoints, verifying each one. */
    WT_CKPT_FOREACH (ckptbase, ckpt) {
        __wt_verbose(session, WT_VERB_VERIFY, "%s: checkpoint %s", name, ckpt->name);

        /* Fake checkpoints require no work. */
        if (F_ISSET(ckpt, WT_CKPT_FAKE))
            continue;

        /* House-keeping between checkpoints. */
        __verify_checkpoint_reset(vs);

        if (WT_VRFY_DUMP(vs)) {
            WT_ERR(__wt_msg(session, "%s", WT_DIVIDER));
            WT_ERR(__wt_msg(session, "%s, ckpt_name: %s", name, ckpt->name));
        }

        /* Load the checkpoint. */
        WT_ERR(bm->checkpoint_load(
          bm, session, ckpt->raw.data, ckpt->raw.size, root_addr, &root_addr_size, true));

        /* Skip trees with no root page. */
        if (root_addr_size != 0) {
            WT_ERR(__wti_btree_tree_open(session, root_addr, root_addr_size));

            if (WT_VRFY_DUMP(vs))
                WT_ERR(__wt_msg(session, "Root:\n\t> addr: %s",
                  __wt_addr_string(session, root_addr, root_addr_size, vs->tmp1)));

            __wt_evict_file_exclusive_off(session);

            /*
             * Create a fake, unpacked parent cell for the tree based on the checkpoint information.
             */
            memset(&addr_unpack, 0, sizeof(addr_unpack));
            WT_TIME_AGGREGATE_COPY(&addr_unpack.ta, &ckpt->ta);
            if (ckpt->ta.prepare)
                addr_unpack.ta.prepare = 1;
            addr_unpack.raw = WT_CELL_ADDR_INT;

            /* Verify the tree. */
            WT_WITH_PAGE_INDEX(
              session, ret = __verify_tree(session, &btree->root, &addr_unpack, vs));

            /*
             * The checkpoints are in time-order, so the last one in the list is the most recent. If
             * this is the most recent checkpoint, verify the history store against it, also verify
             * page discard function if we're in disagg mode.
             */
            if (ret == 0 && (ckpt + 1)->name == NULL) {
                if (F_ISSET(btree, WT_BTREE_DISAGGREGATED))
                    WT_TRET(__verify_page_discard(session, bm));

                if (!skip_hs)
                    WT_TRET(__wt_hs_verify_one(session, btree->id));
                /*
                 * We cannot error out here. If we got an error verifying the history store, we need
                 * to follow through with reacquiring the exclusive call below. We'll error out
                 * after that and unloading this checkpoint.
                 */
            }

            /*
             * If the read_corrupt mode was turned on, we may have continued traversing and
             * verifying the pages of the tree despite encountering an error. Set the error.
             */
            if (vs->verify_err != 0)
                ret = vs->verify_err;

            /*
             * We have an exclusive lock on the handle, but we're swapping root pages in-and-out of
             * that handle, and there's a race with eviction entering the tree and seeing an invalid
             * root page. Eviction must work on trees being verified (else we'd have to do our own
             * eviction), lock eviction out whenever we're loading a new root page. This loop works
             * because we are called with eviction locked out, so we release the lock at the top of
             * the loop and re-acquire it here.
             */
            WT_TRET(__wt_evict_file_exclusive_on(session));
            WT_TRET(__wt_evict_file(session, WT_SYNC_DISCARD));
        }

        /* Unload the checkpoint. */
        WT_TRET(bm->checkpoint_unload(bm, session));

        /*
         * We've finished one checkpoint's verification (verification, then eviction and checkpoint
         * unload): if any errors occurred, quit. Done this way because otherwise we'd need at least
         * two more state variables on error, one to know if we need to discard the tree from the
         * cache and one to know if we need to unload the checkpoint.
         */
        WT_ERR(ret);

        /* Display the tree shape. */
        if (vs->dump_layout)
            WT_ERR(__dump_layout(session, vs));
    }

done:
err:
    /* Inform the underlying block manager we're done. */
    if (bm_start)
        WT_TRET(bm->verify_end(bm, session, ret == 0));

    /* Discard the list of checkpoints. */
    if (ckptbase != NULL)
        __wt_ckptlist_free(session, &ckptbase);

    /* Free allocated memory. */
    __wt_scr_free(session, &vs->max_key);
    __wt_scr_free(session, &vs->max_addr);
    __wt_scr_free(session, &vs->tmp1);
    __wt_scr_free(session, &vs->tmp2);
    __wt_scr_free(session, &vs->tmp3);
    __wt_scr_free(session, &vs->tmp4);

    return (ret);
}

/*
 * __verify_checkpoint_reset --
 *     Reset anything needing to be reset for each new checkpoint verification.
 */
static void
__verify_checkpoint_reset(WT_VSTUFF *vs)
{
    /*
     * Key order is per checkpoint, reset the data length that serves as a flag value.
     */
    vs->max_addr->size = 0;

    /* Record total is per checkpoint, reset the record count. */
    vs->records_so_far = 0;

    /* Tree depth. */
    vs->depth = 1;
}

/*
 * __verify_addr_string --
 *     Figure out a page's "address" and load a buffer with a printable, nul-terminated
 *     representation of that address.
 */
static const char *
__verify_addr_string(WT_SESSION_IMPL *session, WT_REF *ref, WT_ITEM *buf)
{
    WT_ADDR_COPY addr;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    char time_string[WT_TIME_STRING_SIZE];

    WT_ENTER_GENERATION(session, WT_GEN_SPLIT);
    WT_ERR(__wt_scr_alloc(session, 0, &tmp));

    if (__wt_ref_addr_copy(session, ref, &addr)) {
        WT_ERR(
          __wt_buf_fmt(session, buf, "%s %s", __wt_addr_string(session, addr.addr, addr.size, tmp),
            __wt_time_aggregate_to_string(&addr.ta, time_string)));
    } else
        WT_ERR(__wt_buf_fmt(session, buf, "%s -/-,-/-", __wt_addr_string(session, NULL, 0, tmp)));

err:
    __wt_scr_free(session, &tmp);
    WT_LEAVE_GENERATION(session, WT_GEN_SPLIT);
    return (buf->data);
}

/*
 * __verify_addr_ts --
 *     Check an address block's timestamps.
 */
static int
__verify_addr_ts(WT_SESSION_IMPL *session, WT_REF *ref, WT_CELL_UNPACK_ADDR *unpack, WT_VSTUFF *vs)
{
    WT_DECL_RET;

    if ((ret = __wt_time_aggregate_validate(session, &unpack->ta, NULL, false)) == 0)
        return (0);

    WT_RET_MSG(session, ret, "internal page reference at %s failed timestamp validation",
      __verify_addr_string(session, ref, vs->tmp1));
}

static const char *__page_types[] = {
  "WT_PAGE_INVALID",
  "WT_PAGE_BLOCK_MANAGER",
  "WT_PAGE_COL_FIX",
  "WT_PAGE_COL_INT",
  "WT_PAGE_COL_VAR",
  "WT_PAGE_OVFL",
  "WT_PAGE_ROW_INT",
  "WT_PAGE_ROW_LEAF",
};

/*
 * __stat_row_leaf_entries --
 *     Get leaf stats.
 */
static int
__stat_row_leaf_entries(WT_SESSION_IMPL *session, const WT_PAGE_HEADER *dsk, uint64_t *entriesp,
  uint64_t *keys_sizep, uint64_t *values_sizep)
{
    WT_CELL_UNPACK_KV unpack;

    *entriesp = *keys_sizep = *values_sizep = 0;
    WT_CELL_FOREACH_KV (session, dsk, unpack) {
        switch (unpack.type) {
        case WT_CELL_KEY:
        case WT_CELL_KEY_OVFL:
            ++*entriesp;
            *keys_sizep += unpack.size;
            break;
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_OVFL:
            *values_sizep += unpack.size;
            break;
        default:
            return (__wt_illegal_value(session, unpack.type));
        }
    }
    WT_CELL_FOREACH_END;
    return (0);
}

/*
 * __tree_stack --
 *     Format page tree stack.
 */
static const char *
__tree_stack(WT_VSTUFF *vs)
{
    static char data[WT_ELEMENTS(vs->tree_stack) * 10];
    size_t i, len, strsz;
    int force_unused;

    for (strsz = 0, i = 0, len = WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1); i < len;
         ++i)
        force_unused = /* using plain WT_UNUSED(snprintf) is screwed on GCC */
          __wt_snprintf_len_incr(&data[strsz], 10, &strsz, "%" PRIu64 ".", vs->tree_stack[i]);
    WT_UNUSED(force_unused);
    if (strsz > 0)
        --strsz; /* remove last dot */
    data[strsz] = 0;
    return (data);
}

/*
 * __verify_tree --
 *     Verify a tree, recursively descending through it in depth-first fashion. The page argument
 *     was physically verified (so we know it's correctly formed), and the in-memory version built.
 *     Our job is to check logical relationships in the page and in the tree.
 */
static int
__verify_tree(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_CELL_UNPACK_ADDR *addr_unpack, WT_VSTUFF *vs)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_CELL_UNPACK_ADDR *unpack, _unpack;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_REF *child_ref;
    size_t my_stack_level, next_stack_level;
    uint32_t entry;

    btree = S2BT(session);
    bm = btree->bm;
    unpack = &_unpack;
    page = ref->page;

    /*
     * The verify operation does not go through the same tree walk flow as other operations
     * utilizing the regular tree walk function. Check for potential pages to pre-fetch here as
     * well.
     */
    if (__wt_session_prefetch_check(session, ref))
        WT_RET(__wti_btree_prefetch(session, ref));

    __wt_verbose(session, WT_VERB_VERIFY, "%s %s", __verify_addr_string(session, ref, vs->tmp1),
      __wt_page_type_string(page->type));

    /* Optionally dump address information. */
    if (vs->dump_address)
        WT_RET(__wt_msg(session, "%s %s write gen: %" PRIu64,
          __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(page->type),
          page->dsk->write_gen));

    my_stack_level = WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1);

    /* Track the shape of the tree. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        ++vs->depth_internal[my_stack_level];
    else
        ++vs->depth_leaf[my_stack_level];

    if (vs->dump_tree_shape) {
        printf("%s  %s%s", __tree_stack(vs), F_ISSET(ref, WT_REF_FLAG_INTERNAL) ? "INTERNAL" : "",
          F_ISSET(ref, WT_REF_FLAG_LEAF) ? "LEAF" : "");
    }

    /*
     * The page's physical structure was verified when it was read into memory by the read server
     * thread, and then the in-memory version of the page was built. Now we make sure the page and
     * tree are logically consistent.
     *
     * !!!
     * The problem: (1) the read server has to build the in-memory version of the page because the
     * read server is the thread that flags when any thread can access the page in the tree; (2) we
     * can't build the in-memory version of the page until the physical structure is known to be OK,
     * so the read server has to verify at least the physical structure of the page; (3) doing
     * complete page verification requires reading additional pages (for example, overflow keys
     * imply reading overflow pages in order to test the key's order in the page); (4) the read
     * server cannot read additional pages because it will hang waiting on itself. For this reason,
     * we split page verification into a physical verification, which allows the in-memory version
     * of the page to be built, and then a subsequent logical verification which happens here.
     *
     * Report progress occasionally.
     */
    if (++vs->fcnt % WT_VERIFY_PROGRESS_INTERVAL == 0)
        WT_RET(__wt_progress(session, NULL, vs->fcnt));

#ifdef HAVE_DIAGNOSTIC
    /* Optionally dump the blocks or page in debugging mode. */
    if (vs->dump_blocks)
        WT_RET(__wti_debug_disk(session, page->dsk, NULL, vs->dump_all_data, vs->dump_key_data));
    if (vs->dump_pages)
        WT_RET(__wti_debug_page(session, NULL, ref, NULL, vs->dump_all_data, vs->dump_key_data));
#endif

    /* Make sure the page we got belongs in this kind of tree. */
    switch (btree->type) {
    case BTREE_COL_FIX:
        if (page->type != WT_PAGE_COL_INT && page->type != WT_PAGE_COL_FIX)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s is a %s, which does not belong in a fixed-length column-store tree",
              __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(page->type));
        break;
    case BTREE_COL_VAR:
        if (page->type != WT_PAGE_COL_INT && page->type != WT_PAGE_COL_VAR)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s is a %s, which does not belong in a variable-length column-store tree",
              __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(page->type));
        break;
    case BTREE_ROW:
        if (page->type != WT_PAGE_ROW_INT && page->type != WT_PAGE_ROW_LEAF)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s is a %s, which does not belong in a row-store tree",
              __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(page->type));
        break;
    }

    /* Column-store key order checks: check the page's record number. */
    switch (page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        /*
         * FLCS trees can have WT_PAGE_COL_INT or WT_PAGE_COL_FIX pages, and gaps in the namespace
         * are not allowed; VLCS trees can have WT_PAGE_COL_INT or WT_PAGE_COL_VAR pages, and gaps
         * in the namespace *are* allowed. Use the tree type to pick the check logic.
         */
        if (btree->type == BTREE_COL_FIX && ref->ref_recno != vs->records_so_far + 1)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s has a starting record of %" PRIu64
              " when the expected starting record is %" PRIu64,
              __verify_addr_string(session, ref, vs->tmp1), ref->ref_recno, vs->records_so_far + 1);
        else if (btree->type == BTREE_COL_VAR && ref->ref_recno < vs->records_so_far + 1)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s has a starting record of %" PRIu64
              " when the expected starting record is at least %" PRIu64,
              __verify_addr_string(session, ref, vs->tmp1), ref->ref_recno, vs->records_so_far + 1);
        break;
    }

    /*
     * Row-store leaf page key order check: it's a depth-first traversal, the first key on this page
     * should be larger than any key previously seen.
     */
    switch (page->type) {
    case WT_PAGE_ROW_LEAF:
        WT_RET(__verify_row_leaf_key_order(session, ref, vs));
        break;
    }

    /*
     * Check page content, additionally updating the column-store record count. If a tree is empty
     * (just created), it won't have a disk image; if there is no disk image, there is no page
     * content to check.
     */
    if (page->dsk != NULL) {
        /*
         * Compare the write generation number on the page to the write generation number on the
         * parent. Since a parent page's reconciliation takes place once all of its child pages have
         * been completed, the parent page's write generation number must be higher than that of its
         * children.
         */
        if (!__wt_ref_is_root(ref) && page->dsk->write_gen >= ref->home->dsk->write_gen)
            WT_RET_MSG(session, EINVAL,
              "child write generation number %" PRIu64
              " is greater/equal to the parent page write generation number %" PRIu64,
              page->dsk->write_gen, ref->home->dsk->write_gen);

        switch (page->type) {
        case WT_PAGE_COL_FIX:
            WT_RET(__verify_page_content_fix(session, ref, addr_unpack, vs));
            break;
        case WT_PAGE_COL_INT:
        case WT_PAGE_ROW_INT:
            WT_RET(__verify_page_content_int(session, ref, addr_unpack, vs));
            break;
        case WT_PAGE_COL_VAR:
        case WT_PAGE_ROW_LEAF:
            WT_RET(__verify_page_content_leaf(session, ref, addr_unpack, vs));
            break;
        }

        if (vs->dump_tree_shape) {
            printf("  type: %s",
              page->dsk->type < WT_ELEMENTS(__page_types) ? __page_types[page->dsk->type] :
                                                            "UNKNOWN");
            /* see __wti_page_inmem */
            switch (page->dsk->type) {
            case WT_PAGE_COL_FIX:
            case WT_PAGE_COL_VAR:
            case WT_PAGE_COL_INT:
                printf("  entries: %" PRIu32, page->dsk->u.entries);
                break;
            case WT_PAGE_ROW_INT:
                printf("  entries: %" PRIu32, page->dsk->u.entries / 2);
                break;
            case WT_PAGE_ROW_LEAF:
                WT_RET(
                  __stat_row_leaf_entries(session, page->dsk, &vs->keys_count_stack[my_stack_level],
                    &vs->key_sz_stack[my_stack_level], &vs->val_sz_stack[my_stack_level]));
                printf("  keys: %" PRIu64 "  keys_size: %" PRIu64 "  values_size: %" PRIu64
                       "  total_size: %" PRIu64,
                  vs->keys_count_stack[my_stack_level], vs->key_sz_stack[my_stack_level],
                  vs->val_sz_stack[my_stack_level],
                  vs->total_sz_stack[my_stack_level] = page->dsk->mem_size);
                break;
            default:
                break; /* Shouldn't even be here */
            }
        }
    }

    if (vs->dump_tree_shape)
        printf("  mem_footprint: %" WT_SIZET_FMT, page->memory_footprint);

    /* Compare the address type against the page type. */
    switch (page->type) {
    case WT_PAGE_COL_FIX:
        if (addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
            goto celltype_err;
        break;
    case WT_PAGE_COL_VAR:
    case WT_PAGE_ROW_LEAF:
        if (addr_unpack->raw != WT_CELL_ADDR_DEL && addr_unpack->raw != WT_CELL_ADDR_LEAF &&
          addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
            goto celltype_err;
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        if (addr_unpack->raw != WT_CELL_ADDR_INT)
celltype_err:
            WT_RET_MSG(session, WT_ERROR,
              "page at %s, of type %s, is referenced in its parent by a cell of type %s",
              __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(page->type),
              __wti_cell_type_string(addr_unpack->raw));
        break;
    }

    if (vs->dump_tree_shape)
        printf("\n");

    /* Check tree connections and recursively descend the tree. */
    switch (page->type) {
    case WT_PAGE_COL_INT:
        /* For each entry in an internal page, verify the subtree. */
        entry = 0;
        next_stack_level = WT_MIN(vs->depth + 1, WT_ELEMENTS(vs->depth_internal) - 1);
        WT_INTL_FOREACH_BEGIN (session, page, child_ref) {
            vs->tree_stack[my_stack_level] = entry++;
            vs->keys_count_stack[next_stack_level] = vs->total_sz_stack[next_stack_level] =
              vs->key_sz_stack[next_stack_level] = vs->val_sz_stack[next_stack_level] = 0;

            /*
             * It's a depth-first traversal: this entry's starting record number should be 1 more
             * than the total records reviewed to this point. However, for VLCS fast-truncate can
             * introduce gaps; allow a gap but not overlapping ranges. For FLCS, gaps are not
             * permitted.
             */
            if (btree->type == BTREE_COL_FIX && child_ref->ref_recno != vs->records_so_far + 1) {
                WT_RET_MSG(session, WT_ERROR,
                  "the starting record number in entry %" PRIu32
                  " of the column internal page at %s is %" PRIu64
                  " and the expected starting record number is %" PRIu64,
                  entry, __verify_addr_string(session, child_ref, vs->tmp1), child_ref->ref_recno,
                  vs->records_so_far + 1);
            } else if (btree->type == BTREE_COL_VAR &&
              child_ref->ref_recno < vs->records_so_far + 1) {
                WT_RET_MSG(session, WT_ERROR,
                  "the starting record number in entry %" PRIu32
                  " of the column internal page at %s is %" PRIu64
                  " and the expected starting record number is at least %" PRIu64,
                  entry, __verify_addr_string(session, child_ref, vs->tmp1), child_ref->ref_recno,
                  vs->records_so_far + 1);
            }

            /*
             * If there is no address, it should be the first entry in the page. This is the case
             * where inmem inserts a blank page to fill a namespace gap on the left-hand side of the
             * tree. If the situation is what we expect, go to the next entry; otherwise complain.
             */
            if (child_ref->addr == NULL) {
                /* The entry number has already been incremented above, so 1 is the first. */
                if (entry == 1)
                    continue;
                WT_RET_MSG(session, WT_ERROR,
                  "found a page with no address in entry %" PRIu32
                  " of the column internal page at %s",
                  entry, __verify_addr_string(session, child_ref, vs->tmp1));
            }

            /* Unpack the address block and check timestamps */
            __wt_cell_unpack_addr(session, child_ref->home->dsk, child_ref->addr, unpack);
            WT_RET(__verify_addr_ts(session, child_ref, unpack, vs));

            /* Verify the subtree. */
            ++vs->depth;
            ret = __wt_page_in(session, child_ref, 0);

            if (ret != 0) {
                if (vs->dump_address)
                    WT_TRET(__wt_msg(session,
                      "%s Read failure while accessing a page from the column internal page (ret = "
                      "%d).",
                      __verify_addr_string(session, child_ref, vs->tmp1), ret));
                if (!vs->read_corrupt)
                    WT_RET(ret);
                /*
                 * If read_corrupt configured, continue traversing through the pages of the tree
                 * even after encountering errors reading in the page.
                 */
                if (vs->verify_err == 0)
                    vs->verify_err = ret;
                continue;
            }
            ret = __verify_tree(session, child_ref, unpack, vs);
            WT_TRET(__wt_page_release(session, child_ref, 0));
            --vs->depth;
            WT_RET(ret);

            WT_RET(bm->verify_addr(bm, session, unpack->data, unpack->size));

            vs->keys_count_stack[my_stack_level] += vs->keys_count_stack[next_stack_level];
            vs->total_sz_stack[my_stack_level] += vs->total_sz_stack[next_stack_level];
            vs->key_sz_stack[my_stack_level] += vs->key_sz_stack[next_stack_level];
            vs->val_sz_stack[my_stack_level] += vs->val_sz_stack[next_stack_level];
        }
        WT_INTL_FOREACH_END;
        if (vs->dump_tree_shape)
            printf("%s  =  children: %" PRIu64 "  keys: %" PRIu64 "  keys_size: %" PRIu64
                   "  values_size: %" PRIu64 "  total_size: %" PRIu64 "\n",
              __tree_stack(vs), vs->tree_stack[my_stack_level] + 1,
              vs->keys_count_stack[my_stack_level], vs->key_sz_stack[my_stack_level],
              vs->val_sz_stack[my_stack_level], vs->total_sz_stack[my_stack_level]);
        break;
    case WT_PAGE_ROW_INT:
        /* For each entry in an internal page, verify the subtree. */
        entry = 0;
        next_stack_level = WT_MIN(vs->depth + 1, WT_ELEMENTS(vs->depth_internal) - 1);
        WT_INTL_FOREACH_BEGIN (session, page, child_ref) {
            vs->tree_stack[my_stack_level] = entry++;
            vs->keys_count_stack[next_stack_level] = vs->total_sz_stack[next_stack_level] =
              vs->key_sz_stack[next_stack_level] = vs->val_sz_stack[next_stack_level] = 0;

            /*
             * It's a depth-first traversal: this entry's starting key should be larger than the
             * largest key previously reviewed.
             *
             * The 0th key of any internal page is magic, and we can't test against it.
             */
            if (entry != 1)
                WT_RET(__verify_row_int_key_order(session, page, child_ref, entry, vs));

            /* Unpack the address block and check timestamps */
            __wt_cell_unpack_addr(session, child_ref->home->dsk, child_ref->addr, unpack);
            WT_RET(__verify_addr_ts(session, child_ref, unpack, vs));

            /* Verify the subtree. */
            ++vs->depth;
            ret = __wt_page_in(session, child_ref, 0);

            if (ret != 0) {
                if (vs->dump_address)
                    WT_TRET(__wt_msg(session,
                      "%s Read failure while accessing a page from the row internal page (ret = "
                      "%d).",
                      __verify_addr_string(session, child_ref, vs->tmp1), ret));
                if (!vs->read_corrupt)
                    WT_RET(ret);
                /*
                 * If read_corrupt is configured, continue traversing through the pages of the tree
                 * even after encountering errors reading in the page.
                 */
                if (vs->verify_err == 0)
                    vs->verify_err = ret;
                continue;
            }
            ret = __verify_tree(session, child_ref, unpack, vs);
            WT_TRET(__wt_page_release(session, child_ref, 0));
            --vs->depth;
            WT_RET(ret);

            WT_RET(bm->verify_addr(bm, session, unpack->data, unpack->size));

            vs->keys_count_stack[my_stack_level] += vs->keys_count_stack[next_stack_level];
            vs->total_sz_stack[my_stack_level] += vs->total_sz_stack[next_stack_level];
            vs->key_sz_stack[my_stack_level] += vs->key_sz_stack[next_stack_level];
            vs->val_sz_stack[my_stack_level] += vs->val_sz_stack[next_stack_level];
        }
        WT_INTL_FOREACH_END;
        if (vs->dump_tree_shape)
            printf("%s  =  children: %" PRIu64 "  keys: %" PRIu64 "  keys_size: %" PRIu64
                   "  values_size: %" PRIu64 "  total_size: %" PRIu64 "\n",
              __tree_stack(vs), vs->tree_stack[my_stack_level] + 1,
              vs->keys_count_stack[my_stack_level], vs->key_sz_stack[my_stack_level],
              vs->val_sz_stack[my_stack_level], vs->total_sz_stack[my_stack_level]);
        break;
    }
    return (0);
}

/*
 * __verify_row_int_key_order --
 *     Compare a key on an internal page to the largest key we've seen so far; update the largest
 *     key we've seen so far to that key.
 */
static int
__verify_row_int_key_order(
  WT_SESSION_IMPL *session, WT_PAGE *parent, WT_REF *ref, uint32_t entry, WT_VSTUFF *vs)
{
    WT_BTREE *btree;
    WT_ITEM item;
    int cmp;

    btree = S2BT(session);

    /*
     * The maximum key is usually set from the leaf page first. If the first leaf page is corrupted,
     * it is possible that the key is not set. In that case skip this check.
     */
    if (!vs->verify_err)
        WT_ASSERT(session, vs->max_addr->size != 0);

    /* Get the parent page's internal key. */
    __wt_ref_key(parent, ref, &item.data, &item.size);

    /* There is an edge case where the maximum key is not set due the first leaf being corrupted. */
    if (vs->max_addr->size != 0) {
        /* Compare the key against the largest key we've seen so far. */
        WT_RET(__wt_compare(session, btree->collator, &item, vs->max_key, &cmp));
        if (cmp <= 0)
            WT_RET_MSG(session, WT_ERROR,
              "the internal key in entry %" PRIu32
              " on the page at %s sorts before the last key appearing on page %s, earlier in the "
              "tree: "
              "%s, %s",
              entry, __verify_addr_string(session, ref, vs->tmp1), (char *)vs->max_addr->data,
              __wt_buf_set_printable_format(
                session, item.data, item.size, btree->key_format, false, vs->tmp2),
              __wt_buf_set_printable_format(
                session, vs->max_key->data, vs->max_key->size, btree->key_format, false, vs->tmp3));
    }

    /* Update the largest key we've seen to the key just checked. */
    WT_RET(__wt_buf_set(session, vs->max_key, item.data, item.size));
    WT_IGNORE_RET(__verify_addr_string(session, ref, vs->max_addr));

    return (0);
}

/*
 * __verify_row_leaf_key_order --
 *     Compare the first key on a leaf page to the largest key we've seen so far; update the largest
 *     key we've seen so far to the last key on the page.
 */
static int
__verify_row_leaf_key_order(WT_SESSION_IMPL *session, WT_REF *ref, WT_VSTUFF *vs)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    int cmp;

    btree = S2BT(session);
    page = ref->page;

    /*
     * If a tree is empty (just created), it won't have keys; if there are no keys, we're done.
     */
    if (page->entries == 0)
        return (0);

    /*
     * We visit our first leaf page before setting the maximum key (the 0th keys on the internal
     * pages leading to the smallest leaf in the tree are all empty entries).
     */
    if (vs->max_addr->size != 0) {
        WT_RET(__wt_row_leaf_key_copy(session, page, page->pg_row, vs->tmp1));

        /*
         * Compare the key against the largest key we've seen so far.
         *
         * If we're comparing against a key taken from an internal page, we can compare equal (which
         * is an expected path, the internal page key is often a copy of the leaf page's first key).
         * But, in the case of the 0th slot on an internal page, the last key we've seen was a key
         * from a previous leaf page, and it's not OK to compare equally in that case.
         */
        WT_RET(__wt_compare(session, btree->collator, vs->tmp1, (WT_ITEM *)vs->max_key, &cmp));
        if (cmp < 0)
            WT_RET_MSG(session, WT_ERROR,
              "the first key on the page at %s sorts equal to or less than the last key appearing "
              "on the page at %s, earlier in the tree: %s, %s",
              __verify_addr_string(session, ref, vs->tmp2), (char *)vs->max_addr->data,
              __wt_buf_set_printable_format(
                session, vs->tmp1->data, vs->tmp1->size, btree->key_format, false, vs->tmp3),
              __wt_buf_set_printable_format(
                session, vs->max_key->data, vs->max_key->size, btree->key_format, false, vs->tmp4));
    }

    /* Update the largest key we've seen to the last key on this page. */
    WT_RET(__wt_row_leaf_key_copy(session, page, page->pg_row + (page->entries - 1), vs->max_key));
    WT_IGNORE_RET(__verify_addr_string(session, ref, vs->max_addr));

    return (0);
}

/*
 * __verify_overflow --
 *     Read in an overflow page and check it.
 */
static int
__verify_overflow(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, WT_VSTUFF *vs)
{
    WT_BM *bm;
    const WT_PAGE_HEADER *dsk;

    bm = S2BT(session)->bm;

    /* Read and verify the overflow item. */
    WT_RET(__wt_blkcache_read(session, vs->tmp1, NULL, addr, addr_size));

    /*
     * The physical page has already been verified, but we haven't confirmed it was an overflow
     * page, only that it was a valid page. Confirm it's the type of page we expected.
     */
    dsk = vs->tmp1->data;
    if (dsk->type != WT_PAGE_OVFL)
        WT_RET_MSG(session, WT_ERROR, "overflow referenced page at %s is not an overflow page",
          __wt_addr_string(session, addr, addr_size, vs->tmp1));

    WT_RET(bm->verify_addr(bm, session, addr, addr_size));
    return (0);
}

/*
 * __verify_ts_stable_cmp --
 *     Verify that a pair of start and stop timestamps are valid against the global stable
 *     timestamp. Takes in either a key for history store timestamps or a ref and cell number.
 */
static int
__verify_ts_stable_cmp(WT_SESSION_IMPL *session, WT_ITEM *key, WT_REF *ref, uint32_t cell_num,
  wt_timestamp_t start_ts, wt_timestamp_t stop_ts, WT_VSTUFF *vs)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    char tp_string[2][WT_TS_INT_STRING_SIZE];
    bool start;

    btree = S2BT(session);
    start = true;

    if (start_ts != WT_TS_NONE && start_ts > vs->stable_timestamp)
        goto msg;

    if (stop_ts != WT_TS_MAX && stop_ts > vs->stable_timestamp) {
        start = false;
        goto msg;
    }

    return (ret);

msg:
    WT_ASSERT(session, ref != NULL || key != NULL);
    if (ref != NULL)
        WT_RET(__wt_buf_fmt(session, vs->tmp1, "cell %" PRIu32 " on page at %s", cell_num,
          __verify_addr_string(session, ref, vs->tmp2)));
    else if (key != NULL)
        WT_RET(__wt_buf_fmt(session, vs->tmp1, "Value in history store for key {%s}",
          __wt_key_string(session, key->data, key->size, btree->key_format, vs->tmp2)));

    WT_RET_MSG(session, WT_ERROR,
      "%s has failed verification with a %s timestamp of %s greater than the stable_timestamp of "
      "%s",
      (char *)vs->tmp1->data, start ? "start" : "stop",
      __wt_timestamp_to_string(start ? start_ts : stop_ts, tp_string[0]),
      __wt_timestamp_to_string(vs->stable_timestamp, tp_string[1]));
}

/*
 * __verify_key_hs --
 *     Verify a key against the history store. The unpack denotes the data store's timestamp range
 *     information and is used for verifying timestamp range overlaps.
 */
static int
__verify_key_hs(
  WT_SESSION_IMPL *session, WT_ITEM *tmp1, wt_timestamp_t newer_start_ts, WT_VSTUFF *vs)
{
/* FIXME-WT-10779 - Enable the history store validation. */
#ifdef WT_VERIFY_VALIDATE_HISTORY_STORE
    WT_BTREE *btree;
    WT_CURSOR *hs_cursor;
    WT_DECL_RET;
    wt_timestamp_t older_start_ts, older_stop_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    char ts_string[2][WT_TS_INT_STRING_SIZE];

    btree = S2BT(session);
    hs_btree_id = btree->id;
    WT_RET(__wt_curhs_open(session, hs_btree_id, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    /*
     * Set the data store timestamp and transactions to initiate timestamp range verification. Since
     * transaction-ids are wiped out on start, we could possibly have a start txn-id of WT_TXN_NONE,
     * in which case we initialize our newest with the max txn-id.
     */
    older_stop_ts = 0;

    /*
     * Open a history store cursor positioned at the end of the data store key (the newest record)
     * and iterate backwards until we reach a different key or btree.
     */
    hs_cursor->set_key(hs_cursor, 4, hs_btree_id, tmp1, WT_TS_MAX, UINT64_MAX);
    ret = __wt_curhs_search_near_before(session, hs_cursor);

    for (; ret == 0; ret = hs_cursor->prev(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, vs->tmp2, &older_start_ts, &hs_counter));
        /* Verify the newer record's start is later than the older record's stop. */
        if (newer_start_ts < older_stop_ts) {
            WT_ERR_MSG(session, WT_ERROR,
              "key %s has a overlap of timestamp ranges between history store stop timestamp %s "
              "being newer than a more recent timestamp range having start timestamp %s",
              __wt_buf_set_printable_format(
                session, tmp1->data, tmp1->size, btree->key_format, false, vs->tmp2),
              __wt_timestamp_to_string(older_stop_ts, ts_string[0]),
              __wt_timestamp_to_string(newer_start_ts, ts_string[1]));
        }

        if (vs->stable_timestamp != WT_TS_NONE)
            WT_ERR(
              __verify_ts_stable_cmp(session, tmp1, NULL, 0, older_start_ts, older_stop_ts, vs));

        /*
         * Since we are iterating from newer to older, the current older record becomes the newer
         * for the next round of verification.
         */
        newer_start_ts = older_start_ts;
    }
err:
    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret == WT_NOTFOUND ? 0 : ret);
#else
    WT_UNUSED(session);
    WT_UNUSED(tmp1);
    WT_UNUSED(newer_start_ts);
    WT_UNUSED(vs);
    return (0);
#endif
}

/*
 * __verify_page_content_int --
 *     Verify an internal page's content.
 */
static int
__verify_page_content_int(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_CELL_UNPACK_ADDR *parent, WT_VSTUFF *vs)
{
    WT_CELL_UNPACK_ADDR unpack;
    WT_DECL_RET;
    WT_PAGE *page;
    const WT_PAGE_HEADER *dsk;
    WT_TIME_AGGREGATE *ta;
    uint32_t cell_num;

    page = ref->page;
    dsk = page->dsk;
    ta = &unpack.ta;

    /* Walk the page, verifying overflow pages and validating timestamps. */
    cell_num = 0;
    WT_CELL_FOREACH_ADDR (session, dsk, unpack) {
        ++cell_num;

        if (!__wti_cell_type_check(unpack.type, dsk->type))
            WT_RET_MSG(session, WT_ERROR,
              "illegal cell and page type combination: cell %" PRIu32
              " on page at %s is a %s cell on a %s page",
              cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
              __wti_cell_type_string(unpack.type), __wt_page_type_string(dsk->type));

        switch (unpack.type) {
        case WT_CELL_KEY_OVFL:
            if ((ret = __verify_overflow(session, unpack.data, unpack.size, vs)) != 0)
                WT_RET_MSG(session, ret,
                  "cell %" PRIu32
                  " on page at %s references an overflow item at %s that failed verification",
                  cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
                  __wt_addr_string(session, unpack.data, unpack.size, vs->tmp2));
            break;
        }

        switch (unpack.type) {
        case WT_CELL_ADDR_DEL:
        case WT_CELL_ADDR_INT:
        case WT_CELL_ADDR_LEAF:
        case WT_CELL_ADDR_LEAF_NO:
            if ((ret = __wt_time_aggregate_validate(session, ta, &parent->ta, false)) != 0)
                WT_RET_MSG(session, ret,
                  "cell %" PRIu32 " on page at %s failed timestamp validation", cell_num - 1,
                  __verify_addr_string(session, ref, vs->tmp1));

            if (vs->stable_timestamp != WT_TS_NONE)
                WT_RET(__verify_ts_stable_cmp(
                  session, NULL, ref, cell_num - 1, ta->oldest_start_ts, ta->newest_stop_ts, vs));
            break;
        }
    }
    WT_CELL_FOREACH_END;

    return (0);
}

/*
 * __verify_page_content_fix --
 *     Verify the page's content. Like __verify_page_content_leaf but for FLCS pages.
 */
static int
__verify_page_content_fix(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_CELL_UNPACK_ADDR *parent, WT_VSTUFF *vs)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_PAGE *page;
    uint64_t start_ts;
    uint32_t cell_num, numtws, recno_offset, tw;
    uint8_t *p;

    page = ref->page;

    /* Count the keys. */
    vs->records_so_far += page->entries;

    /* Examine each row; iterate the keys and time windows in parallel. */
    /* Walk the time windows, if there are any. */
    numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;
    for (recno_offset = 0, tw = 0; recno_offset < page->entries; recno_offset++) {
        if (tw < numtws && page->pg_fix_tws[tw].recno_offset == recno_offset) {
            /* This row has a time window. */

            /* The printable cell number for the value is 2x the entry number (tw) plus 1. */
            cell_num = tw * 2 + 1;

            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

            /* We are supposed to see only values (not keys) and only plain values belong. */
            if (unpack.type != WT_CELL_VALUE)
                WT_RET_MSG(session, EINVAL,
                  "cell %" PRIu32 " for key %" PRIu64 " on page at %s has wrong type %s", cell_num,
                  ref->ref_recno + page->pg_fix_tws[tw].recno_offset,
                  __verify_addr_string(session, ref, vs->tmp1),
                  __wti_cell_type_string(unpack.type));

            /* The value cell should contain only a time window. */
            if (unpack.size != 0)
                WT_RET_MSG(session, EINVAL,
                  "cell %" PRIu32 " for key %" PRIu64 " on page at %s has nonempty value", cell_num,
                  ref->ref_recno + page->pg_fix_tws[tw].recno_offset,
                  __verify_addr_string(session, ref, vs->tmp1));

            if ((ret = __wt_time_value_validate(session, &unpack.tw, &parent->ta, false)) != 0)
                WT_RET_MSG(session, ret,
                  "cell %" PRIu32 " for key %" PRIu64 " on page at %s failed timestamp validation",
                  cell_num, ref->ref_recno + page->pg_fix_tws[tw].recno_offset,
                  __verify_addr_string(session, ref, vs->tmp1));

            if (vs->stable_timestamp != WT_TS_NONE)
                WT_RET(__verify_ts_stable_cmp(
                  session, NULL, ref, cell_num, unpack.tw.start_ts, unpack.tw.stop_ts, vs));

            start_ts = unpack.tw.start_ts;
            tw++;
        } else
            start_ts = WT_TS_NONE;

        /*
         * Verify key-associated history-store entries. Note that while a WT_COL_FIX_VERSION_NIL
         * page written by a build that does not support FLCS timestamps and history will have no
         * history store entries, such pages can also be written by newer builds; so we should
         * always validate the history entries.
         */
        p = vs->tmp1->mem;
        WT_RET(__wt_vpack_uint(&p, 0, ref->ref_recno + recno_offset));
        vs->tmp1->size = WT_PTRDIFF(p, vs->tmp1->mem);
        WT_RET(__verify_key_hs(session, vs->tmp1, start_ts, vs));
    }

    /* The caller checks that the address cell pointing to us is no-overflow, so we needn't. */

    return (0);
}

/*
 * __verify_page_content_leaf --
 *     Verify the page's content.
 */
static int
__verify_page_content_leaf(
  WT_SESSION_IMPL *session, WT_REF *ref, WT_CELL_UNPACK_ADDR *parent, WT_VSTUFF *vs)
{
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_PAGE *page;
    const WT_PAGE_HEADER *dsk;
    WT_ROW *rip;
    WT_TIME_WINDOW *tw;
    uint64_t recno, rle;
    uint32_t cell_num;
    uint8_t *p;
    bool found_ovfl;

    page = ref->page;
    dsk = page->dsk;
    rip = page->pg_row;
    tw = &unpack.tw;
    recno = ref->ref_recno;
    found_ovfl = false;

    /* Walk the page, tracking timestamps and verifying overflow pages. */
    cell_num = 0;
    WT_CELL_FOREACH_KV (session, dsk, unpack) {
        ++cell_num;

        if (!__wti_cell_type_check(unpack.type, dsk->type))
            WT_RET_MSG(session, WT_ERROR,
              "illegal cell and page type combination: cell %" PRIu32
              " on page at %s is a %s cell on a %s page",
              cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
              __wti_cell_type_string(unpack.type), __wt_page_type_string(dsk->type));

        switch (unpack.type) {
        case WT_CELL_KEY_OVFL:
        case WT_CELL_VALUE_OVFL:
            found_ovfl = true;
            if ((ret = __verify_overflow(session, unpack.data, unpack.size, vs)) != 0)
                WT_RET_MSG(session, ret,
                  "cell %" PRIu32
                  " on page at %s references an overflow item at %s that failed verification",
                  cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
                  __wt_addr_string(session, unpack.data, unpack.size, vs->tmp2));
            break;
        }

        switch (unpack.type) {
        case WT_CELL_DEL:
        case WT_CELL_VALUE:
        case WT_CELL_VALUE_COPY:
        case WT_CELL_VALUE_OVFL:
        case WT_CELL_VALUE_SHORT:
            if ((ret = __wt_time_value_validate(session, tw, &parent->ta, false)) != 0)
                WT_RET_MSG(session, ret,
                  "cell %" PRIu32 " on page at %s failed timestamp validation", cell_num - 1,
                  __verify_addr_string(session, ref, vs->tmp1));

            if (vs->stable_timestamp != WT_TS_NONE)
                WT_RET(__verify_ts_stable_cmp(
                  session, NULL, ref, cell_num - 1, tw->start_ts, tw->stop_ts, vs));
            break;
        }

        /* Verify key-associated history-store entries. */
        if (page->type == WT_PAGE_ROW_LEAF) {
            if (unpack.type != WT_CELL_VALUE && unpack.type != WT_CELL_VALUE_COPY &&
              unpack.type != WT_CELL_VALUE_OVFL && unpack.type != WT_CELL_VALUE_SHORT)
                continue;

            WT_RET(__wt_row_leaf_key(session, page, rip++, vs->tmp1, false));
            WT_RET(__verify_key_hs(session, vs->tmp1, tw->start_ts, vs));
        } else if (page->type == WT_PAGE_COL_VAR) {
            rle = __wt_cell_rle(&unpack);
            p = vs->tmp1->mem;
            WT_RET(__wt_vpack_uint(&p, 0, recno));
            vs->tmp1->size = WT_PTRDIFF(p, vs->tmp1->mem);
            WT_RET(__verify_key_hs(session, vs->tmp1, tw->start_ts, vs));

            recno += rle;
            vs->records_so_far += rle;
        }
    }
    WT_CELL_FOREACH_END;

    /*
     * Object if a leaf-no-overflow address cell references a page with overflow keys, but don't
     * object if a leaf address cell references a page without overflow keys. Reconciliation doesn't
     * guarantee every leaf page without overflow items will be a leaf-no-overflow type.
     */
    if (found_ovfl && parent->raw == WT_CELL_ADDR_LEAF_NO)
        WT_RET_MSG(session, WT_ERROR,
          "page at %s, of type %s and referenced in its parent by a cell of type %s, contains "
          "overflow items",
          __verify_addr_string(session, ref, vs->tmp1), __wt_page_type_string(ref->page->type),
          __wti_cell_type_string(parent->raw));

    return (0);
}

/*
 * __verify_page_discard --
 *     Verify all live pages in disagg mode, ensuring that no pages were incorrectly discarded.
 */
static int
__verify_page_discard(WT_SESSION_IMPL *session, WT_BM *bm)
{
    WT_REF *ref = NULL;
    uint64_t num_pages_found_in_btree = 0;
    size_t capacity = 0;
    uint64_t *page_ids = NULL;
    int ret = 0;

    /*
     * Walk the btree to retrieve the page IDs for all pages in the btree at the loaded checkpoint
     * time.
     */
    while ((ret = (__wt_tree_walk(session, &ref, WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED))) == 0 &&
      ref != NULL) {
        WT_PAGE *page = ref->page;

        /*
         * Use dynamically allocated array to track page IDs as we don't know the number of pages
         *  here. Check if the array size needs to grow.
         */
        if (num_pages_found_in_btree == capacity) {
            uint64_t new_capacity = (capacity * 2 + 1) * sizeof(uint64_t);
            WT_RET(__wt_realloc_def(session, &capacity, new_capacity, &page_ids));
            capacity = new_capacity;
        }

        if (page != NULL) {
            WT_ASSERT(session, page->disagg_info != NULL);
            page_ids[num_pages_found_in_btree++] = page->disagg_info->block_meta.page_id;
        }
    }

    WT_RET_NOTFOUND_OK(ret);

    /*
     * Track the number of pages found in the PALM walk. This value is tracked separately because
     * WT_ITEM->size must match the allocated memory, while the actual number of pages found may be
     * smaller than that allocation.
     */
    size_t num_pages_found_in_palm = 0;
    uint64_t checkpoint_lsn;
    checkpoint_lsn = S2C(session)->disaggregated_storage.last_checkpoint_meta_lsn;
    WT_DECL_ITEM(item);
    WT_RET(__wt_scr_alloc(session, num_pages_found_in_palm, &item));

    WT_ASSERT(session, bm->get_page_ids != NULL);
    /* Get page IDs from PALM. */
    WT_RET(bm->get_page_ids(bm, session, item, &num_pages_found_in_palm, checkpoint_lsn));

    if ((uint64_t)num_pages_found_in_palm != num_pages_found_in_btree) {
        /*
         * FIXME-WT-14700: Investigate whether we need to do anything special when freeing a root
         * page. Change below warning to an error after root page discard is implemented, if a
         * mismatch is found this function will return the corresponding error code.
         */
        __wt_verbose_level(session, WT_VERB_DISAGGREGATED_STORAGE, WT_VERBOSE_DEBUG_5,
          "Mismatch in the number of page IDs found from PALM and btree walk: PALM %" PRIu64
          " Btree walk %" PRIu64,
          (uint64_t)num_pages_found_in_palm, num_pages_found_in_btree);
    }

    /*
     * Sort the btree walk array by page ID in ascending order to match the order used in the PALM
     * walk.
     */
    __wt_qsort(page_ids, num_pages_found_in_btree, sizeof(uint64_t), __verify_compare_page_id);

    for (uint32_t index_in_palm = 0, index_in_btree = 0;
         index_in_palm <= num_pages_found_in_palm && index_in_btree <= num_pages_found_in_btree;) {
        if (index_in_palm == num_pages_found_in_palm && index_in_btree == num_pages_found_in_btree)
            break;
        uint64_t id_in_palm =
          index_in_palm < num_pages_found_in_palm ? ((uint64_t *)item->data)[index_in_palm] : 0;
        uint64_t id_in_btree =
          index_in_btree < num_pages_found_in_btree ? page_ids[index_in_btree] : 0;
        /*
         * FIXME-WT-14700: Investigate whether we need to do anything special when freeing a root
         * page. Change below warning to an error after root page discard is implemented, if a
         * mismatch is found this function will return the corresponding error code.
         */
        if (index_in_btree == num_pages_found_in_btree || id_in_palm < id_in_btree) {
            __wt_verbose_level(session, WT_VERB_DISAGGREGATED_STORAGE, WT_VERBOSE_DEBUG_5,
              "Unreferenced page was not discarded: PALM[%" PRIu32 "] %" PRIu64, index_in_palm,
              id_in_palm);
            index_in_palm++;
        } else if (index_in_palm == num_pages_found_in_palm || id_in_palm > id_in_btree) {
            __wt_verbose_level(session, WT_VERB_DISAGGREGATED_STORAGE, WT_VERBOSE_DEBUG_5,
              "Discarded page is still in use: BTREE[%" PRIu32 "] %" PRIu64, index_in_btree,
              id_in_btree);
            index_in_btree++;
        } else {
            index_in_palm++;
            index_in_btree++;
        }
    }

    __wt_free(session, page_ids);
    __wt_scr_free(session, &item);

    return (ret);
}

/*
 * __verify_compare_page_id --
 *     Compare two page IDs for qsort, sorts in ascending order.
 */
static int
__verify_compare_page_id(const void *a, const void *b)
{
    const uint64_t *id_a = (const uint64_t *)a;
    const uint64_t *id_b = (const uint64_t *)b;

    if (*id_a < *id_b)
        return (-1);
    if (*id_a > *id_b)
        return (1);

    return (0);
}
