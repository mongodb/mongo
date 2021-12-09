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
    bool dump_blocks;
    bool dump_layout;
    bool dump_pages;

    /* Page layout information. */
    uint64_t depth, depth_internal[100], depth_leaf[100];

    WT_ITEM *tmp1, *tmp2, *tmp3, *tmp4; /* Temporary buffers */
} WT_VSTUFF;

static void __verify_checkpoint_reset(WT_VSTUFF *);
static int __verify_page_content_int(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
static int __verify_page_content_fix(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
static int __verify_page_content_leaf(
  WT_SESSION_IMPL *, WT_REF *, WT_CELL_UNPACK_ADDR *, WT_VSTUFF *);
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

    WT_RET(__wt_config_gets(session, cfg, "dump_address", &cval));
    vs->dump_address = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_blocks", &cval));
    vs->dump_blocks = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_layout", &cval));
    vs->dump_layout = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "dump_pages", &cval));
    vs->dump_pages = cval.val != 0;

    WT_RET(__wt_config_gets(session, cfg, "stable_timestamp", &cval));
    vs->stable_timestamp = WT_TS_NONE; /* Ignored unless a value has been set */
    if (cval.val != 0) {
        if (!txn_global->has_stable_timestamp)
            WT_RET_MSG(session, ENOTSUP,
              "cannot verify against the stable timestamp if it has not been set");
        vs->stable_timestamp = txn_global->stable_timestamp;
    }

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
__verify_config_offsets(WT_SESSION_IMPL *session, const char *cfg[], bool *quitp)
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
        WT_TRET(__wt_debug_offset_blind(session, (wt_off_t)offset, NULL));
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
    uint8_t root_addr[WT_BTREE_MAX_ADDR_COOKIE];
    const char *name;
    bool bm_start, quit;

#if 0
    /* FIXME-WT-6682: temporarily disable history store verification. */
    bool skip_hs;
#endif

    btree = S2BT(session);
    bm = btree->bm;
    ckptbase = NULL;
    name = session->dhandle->name;
    bm_start = false;

#if 0
    /*
     * FIXME-WT-6682: temporarily disable history store verification.
     *
     * Skip the history store explicit call if we're performing a metadata verification. The
     * metadata file is verified before we verify the history store, and it makes no sense to verify
     * the history store against itself.
     */
    skip_hs = strcmp(name, WT_METAFILE_URI) == 0 || strcmp(name, WT_HS_URI) == 0;
#endif

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
    WT_ERR(__verify_config_offsets(session, cfg, &quit));
    if (quit)
        goto done;

    /*
     * Get a list of the checkpoints for this file. Empty objects have no checkpoints, in which case
     * there's no work to do.
     */
    WT_ERR_NOTFOUND_OK(__wt_meta_ckptlist_get(session, name, false, &ckptbase, NULL), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }

    /* Inform the underlying block manager we're verifying. */
    WT_ERR(bm->verify_start(bm, session, ckptbase, cfg));
    bm_start = true;

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
            WT_ERR(__wt_msg(session, "%s: checkpoint %s", name, ckpt->name));
        }

        /* Load the checkpoint. */
        WT_ERR(bm->checkpoint_load(
          bm, session, ckpt->raw.data, ckpt->raw.size, root_addr, &root_addr_size, true));

        /* Skip trees with no root page. */
        if (root_addr_size != 0) {
            WT_ERR(__wt_btree_tree_open(session, root_addr, root_addr_size));

            if (WT_VRFY_DUMP(vs))
                WT_ERR(__wt_msg(session, "Root: %s %s",
                  __wt_addr_string(session, root_addr, root_addr_size, vs->tmp1),
                  __wt_page_type_string(btree->root.page->type)));

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

#if 0
            /*
             * FIXME-WT-6682: temporarily disable history store verification.
             *
             * The checkpoints are in time-order, so the last one in the list is the most recent. If
             * this is the most recent checkpoint, verify the history store against it.
             *
             * FIXME-WT-6263: Temporarily disable history store verification.
             */
            if (ret == 0 && (ckpt + 1)->name == NULL && !skip_hs) {
                /* Open a history store cursor. */
                WT_TRET(__wt_hs_verify_one(session));
                /*
                 * We cannot error out here. If we got an error verifying the history store, we need
                 * to follow through with reacquiring the exclusive call below. We'll error out
                 * after that and unloading this checkpoint.
                 */
            }
#endif

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
         * We've finished one checkpoint's verification (verification, then cache eviction and
         * checkpoint unload): if any errors occurred, quit. Done this way because otherwise we'd
         * need at least two more state variables on error, one to know if we need to discard the
         * tree from the cache and one to know if we need to unload the checkpoint.
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
        WT_TRET(bm->verify_end(bm, session));

    /* Discard the list of checkpoints. */
    if (ckptbase != NULL)
        __wt_meta_ckptlist_free(session, &ckptbase);

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

    WT_ERR(__wt_scr_alloc(session, 0, &tmp));

    if (__wt_ref_addr_copy(session, ref, &addr)) {
        WT_ERR(
          __wt_buf_fmt(session, buf, "%s %s", __wt_addr_string(session, addr.addr, addr.size, tmp),
            __wt_time_aggregate_to_string(&addr.ta, time_string)));
    } else
        WT_ERR(__wt_buf_fmt(session, buf, "%s -/-,-/-", __wt_addr_string(session, NULL, 0, tmp)));

err:
    __wt_scr_free(session, &tmp);
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
    WT_CELL_UNPACK_ADDR *unpack, _unpack;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_REF *child_ref;
    uint32_t entry;

    bm = S2BT(session)->bm;
    unpack = &_unpack;
    page = ref->page;

    __wt_verbose(session, WT_VERB_VERIFY, "%s %s", __verify_addr_string(session, ref, vs->tmp1),
      __wt_page_type_string(page->type));

    /* Optionally dump address information. */
    if (vs->dump_address)
        WT_RET(__wt_msg(session, "%s %s", __verify_addr_string(session, ref, vs->tmp1),
          __wt_page_type_string(page->type)));

    /* Track the shape of the tree. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        ++vs->depth_internal[WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1)];
    else
        ++vs->depth_leaf[WT_MIN(vs->depth, WT_ELEMENTS(vs->depth_internal) - 1)];

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
        WT_RET(__wt_debug_disk(session, page->dsk, NULL));
    if (vs->dump_pages)
        WT_RET(__wt_debug_page(session, NULL, ref, NULL));
#endif

    /* Column-store key order checks: check the page's record number. */
    switch (page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_INT:
    case WT_PAGE_COL_VAR:
        if (ref->ref_recno != vs->records_so_far + 1)
            WT_RET_MSG(session, WT_ERROR,
              "page at %s has a starting record of %" PRIu64
              " when the expected starting record is %" PRIu64,
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
    if (page->dsk != NULL)
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

    /* Compare the address type against the page type. */
    switch (page->type) {
    case WT_PAGE_COL_FIX:
        if (addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
            goto celltype_err;
        break;
    case WT_PAGE_COL_VAR:
        if (addr_unpack->raw != WT_CELL_ADDR_LEAF && addr_unpack->raw != WT_CELL_ADDR_LEAF_NO)
            goto celltype_err;
        break;
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
              __wt_cell_type_string(addr_unpack->raw));
        break;
    }

    /* Check tree connections and recursively descend the tree. */
    switch (page->type) {
    case WT_PAGE_COL_INT:
        /* For each entry in an internal page, verify the subtree. */
        entry = 0;
        WT_INTL_FOREACH_BEGIN (session, page, child_ref) {
            /*
             * It's a depth-first traversal: this entry's starting record number should be 1 more
             * than the total records reviewed to this point.
             */
            ++entry;
            if (child_ref->ref_recno != vs->records_so_far + 1) {
                WT_RET_MSG(session, WT_ERROR,
                  "the starting record number in entry %" PRIu32
                  " of the column internal page at %s is %" PRIu64
                  " and the expected starting record number is %" PRIu64,
                  entry, __verify_addr_string(session, child_ref, vs->tmp1), child_ref->ref_recno,
                  vs->records_so_far + 1);
            }

            /* Unpack the address block and check timestamps */
            __wt_cell_unpack_addr(session, child_ref->home->dsk, child_ref->addr, unpack);
            WT_RET(__verify_addr_ts(session, child_ref, unpack, vs));

            /* Verify the subtree. */
            ++vs->depth;
            WT_RET(__wt_page_in(session, child_ref, 0));
            ret = __verify_tree(session, child_ref, unpack, vs);
            WT_TRET(__wt_page_release(session, child_ref, 0));
            --vs->depth;
            WT_RET(ret);

            WT_RET(bm->verify_addr(bm, session, unpack->data, unpack->size));
        }
        WT_INTL_FOREACH_END;
        break;
    case WT_PAGE_ROW_INT:
        /* For each entry in an internal page, verify the subtree. */
        entry = 0;
        WT_INTL_FOREACH_BEGIN (session, page, child_ref) {
            /*
             * It's a depth-first traversal: this entry's starting key should be larger than the
             * largest key previously reviewed.
             *
             * The 0th key of any internal page is magic, and we can't test against it.
             */
            ++entry;
            if (entry != 1)
                WT_RET(__verify_row_int_key_order(session, page, child_ref, entry, vs));

            /* Unpack the address block and check timestamps */
            __wt_cell_unpack_addr(session, child_ref->home->dsk, child_ref->addr, unpack);
            WT_RET(__verify_addr_ts(session, child_ref, unpack, vs));

            /* Verify the subtree. */
            ++vs->depth;
            WT_RET(__wt_page_in(session, child_ref, 0));
            ret = __verify_tree(session, child_ref, unpack, vs);
            WT_TRET(__wt_page_release(session, child_ref, 0));
            --vs->depth;
            WT_RET(ret);

            WT_RET(bm->verify_addr(bm, session, unpack->data, unpack->size));
        }
        WT_INTL_FOREACH_END;
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

    /* The maximum key is set, we updated it from a leaf page first. */
    WT_ASSERT(session, vs->max_addr->size != 0);

    /* Get the parent page's internal key. */
    __wt_ref_key(parent, ref, &item.data, &item.size);

    /* Compare the key against the largest key we've seen so far. */
    WT_RET(__wt_compare(session, btree->collator, &item, vs->max_key, &cmp));
    if (cmp <= 0)
        WT_RET_MSG(session, WT_ERROR,
          "the internal key in entry %" PRIu32
          " on the page at %s sorts before the last key appearing on page %s, earlier in the tree: "
          "%s, %s",
          entry, __verify_addr_string(session, ref, vs->tmp1), (char *)vs->max_addr->data,
          __wt_buf_set_printable(session, item.data, item.size, false, vs->tmp2),
          __wt_buf_set_printable(session, vs->max_key->data, vs->max_key->size, false, vs->tmp3));

    /* Update the largest key we've seen to the key just checked. */
    WT_RET(__wt_buf_set(session, vs->max_key, item.data, item.size));
    WT_IGNORE_RET_PTR(__verify_addr_string(session, ref, vs->max_addr));

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
              __wt_buf_set_printable(session, vs->tmp1->data, vs->tmp1->size, false, vs->tmp3),
              __wt_buf_set_printable(
                session, vs->max_key->data, vs->max_key->size, false, vs->tmp4));
    }

    /* Update the largest key we've seen to the last key on this page. */
    WT_RET(__wt_row_leaf_key_copy(session, page, page->pg_row + (page->entries - 1), vs->max_key));
    WT_IGNORE_RET_PTR(__verify_addr_string(session, ref, vs->max_addr));

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
    WT_RET(__wt_blkcache_read(session, vs->tmp1, addr, addr_size));

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
    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
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
              __wt_buf_set_printable(session, tmp1->data, tmp1->size, false, vs->tmp2),
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

        if (!__wt_cell_type_check(unpack.type, dsk->type))
            WT_RET_MSG(session, WT_ERROR,
              "illegal cell and page type combination: cell %" PRIu32
              " on page at %s is a %s cell on a %s page",
              cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
              __wt_cell_type_string(unpack.type), __wt_page_type_string(dsk->type));

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

    /* Walk the time windows, if there are any. */
    numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;
    for (tw = 0; tw < numtws; tw++) {
        /* The printable cell number for the value is 2x the entry number (tw) plus 1. */
        cell_num = tw * 2 + 1;

        cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
        __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

        /* We are only supposed to see the values (not the keys) and only plain values belong. */
        if (unpack.type != WT_CELL_VALUE)
            WT_RET_MSG(session, EINVAL,
              "cell %" PRIu32 " for key %" PRIu64 " on page at %s has wrong type %s", cell_num,
              ref->ref_recno + page->pg_fix_tws[tw].recno_offset,
              __verify_addr_string(session, ref, vs->tmp1), __wt_cell_type_string(unpack.type));

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
    }

    /*
     * Verify key-associated history-store entries. Note that while a WT_COL_FIX_VERSION_NIL page
     * written by a build that does not support FLCS timestamps and history will have no history
     * store entries, such pages can also be written by newer builds; so we should always validate
     * the history entries.
     */
    for (recno_offset = 0, tw = 0; recno_offset < page->entries; recno_offset++) {
        p = vs->tmp1->mem;
        WT_RET(__wt_vpack_uint(&p, 0, ref->ref_recno + recno_offset));
        vs->tmp1->size = WT_PTRDIFF(p, vs->tmp1->mem);
        if (tw < numtws && page->pg_fix_tws[tw].recno_offset == recno_offset) {
            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
            start_ts = unpack.tw.start_ts;
            tw++;
        } else
            start_ts = WT_TS_NONE;
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

        if (!__wt_cell_type_check(unpack.type, dsk->type))
            WT_RET_MSG(session, WT_ERROR,
              "illegal cell and page type combination: cell %" PRIu32
              " on page at %s is a %s cell on a %s page",
              cell_num - 1, __verify_addr_string(session, ref, vs->tmp1),
              __wt_cell_type_string(unpack.type), __wt_page_type_string(dsk->type));

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
          __wt_cell_type_string(parent->raw));

    return (0);
}
