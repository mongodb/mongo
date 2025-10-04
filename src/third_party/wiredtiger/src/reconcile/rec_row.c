/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "reconcile_private.h"
#include "reconcile_inline.h"

/*
 * __rec_key_state_update --
 *     Update prefix and suffix compression based on the last key.
 */
static WT_INLINE void
__rec_key_state_update(WTI_RECONCILE *r, bool ovfl_key)
{
    WT_ITEM *a;

    /*
     * If writing an overflow key onto the page, don't update the "last key" value, and leave the
     * state of prefix compression alone. (If we are currently doing prefix compression, we have a
     * key state which will continue to work, we're just skipping the key just created because it's
     * an overflow key and doesn't participate in prefix compression. If we are not currently doing
     * prefix compression, we can't start, an overflow key doesn't give us any state.)
     *
     * Additionally, if we wrote an overflow key onto the page, turn off the suffix compression of
     * row-store internal node keys. (When we split, "last key" is the largest key on the previous
     * page, and "cur key" is the first key on the next page, which is being promoted. In some cases
     * we can discard bytes from the "cur key" that are not needed to distinguish between the "last
     * key" and "cur key", compressing the size of keys on internal nodes. If we just built an
     * overflow key, we're not going to update the "last key", making suffix compression impossible
     * for the next key. Alternatively, we could remember where the last key was on the page, detect
     * it's an overflow key, read it from disk and do suffix compression, but that's too much work
     * for an unlikely event.)
     *
     * If we're not writing an overflow key on the page, update the last-key value and turn on both
     * prefix and suffix compression.
     */
    if (ovfl_key)
        r->key_sfx_compress = false;
    else {
        a = r->cur;
        r->cur = r->last;
        r->last = a;

        r->key_pfx_compress = r->key_pfx_compress_conf;
        r->key_sfx_compress = r->key_sfx_compress_conf;
    }
}

/*
 * __rec_cell_build_int_key --
 *     Process a key and return a WT_CELL structure and byte string to be stored on a row-store
 *     internal page.
 */
static int
__rec_cell_build_int_key(WT_SESSION_IMPL *session, WTI_RECONCILE *r, const void *data, size_t size)
{
    WTI_REC_KV *key;

    key = &r->k;

    /* Copy the bytes into the "current" and key buffers. */
    WT_RET(__wt_buf_set(session, r->cur, data, size));
    WT_RET(__wt_buf_set(session, &key->buf, data, size));

    key->cell_len = __wt_cell_pack_int_key(&key->cell, key->buf.size);
    key->len = key->cell_len + key->buf.size;

    return (0);
}

/*
 * __rec_cell_build_leaf_key --
 *     Process a key and return a WT_CELL structure and byte string to be stored on a row-store leaf
 *     page.
 */
static int
__rec_cell_build_leaf_key(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, const void *data, size_t size, bool *is_ovflp)
{
    WT_BTREE *btree;
    WTI_REC_KV *key;
    size_t pfx_max;
    uint8_t pfx;
    const uint8_t *a, *b;

    *is_ovflp = false;

    btree = S2BT(session);
    key = &r->k;

    pfx = 0;
    if (data == NULL)
        /*
         * When data is NULL, our caller has a prefix compressed key they can't use (probably
         * because they just crossed a split point). Use the full key saved when last called,
         * instead.
         */
        WT_RET(__wt_buf_set(session, &key->buf, r->cur->data, r->cur->size));
    else {
        /*
         * Save a copy of the key for later reference: we use the full key for prefix-compression
         * comparisons, and if we are, for any reason, unable to use the compressed key we generate.
         */
        WT_RET(__wt_buf_set(session, r->cur, data, size));

        /*
         * Do prefix compression on the key. We know by definition the previous key sorts before the
         * current key, which means the keys must differ and we just need to compare up to the
         * shorter of the two keys.
         */
        if (r->key_pfx_compress) {
            /*
             * We can't compress out more than 256 bytes, limit the comparison to that.
             */
            pfx_max = UINT8_MAX;
            if (size < pfx_max)
                pfx_max = size;
            if (r->last->size < pfx_max)
                pfx_max = r->last->size;
            for (a = data, b = r->last->data; pfx < pfx_max; ++pfx)
                if (*a++ != *b++)
                    break;

            /*
             * Prefix compression costs CPU and memory when the page is re-loaded, skip unless
             * there's a reasonable gain. Also, if the previous key was prefix compressed, don't
             * increase the prefix compression if we aren't getting a reasonable gain. (Groups of
             * keys with the same prefix can be quickly built without needing to roll forward
             * through intermediate keys or allocating memory so they can be built faster in the
             * future, for that reason try and create big groups of keys with the same prefix.)
             */
            if (pfx < btree->prefix_compression_min)
                pfx = 0;
            else if (r->key_pfx_last != 0 && pfx > r->key_pfx_last &&
              pfx < r->key_pfx_last + WTI_KEY_PREFIX_PREVIOUS_MINIMUM)
                pfx = r->key_pfx_last;

            if (pfx != 0)
                WT_STAT_DSRC_INCRV(session, rec_prefix_compression, pfx);
        }

        /* Copy the non-prefix bytes into the key buffer. */
        WT_RET(__wt_buf_set(session, &key->buf, (uint8_t *)data + pfx, size - pfx));
    }
    r->key_pfx_last = pfx;

    /* Create an overflow object if the data won't fit. */
    if (key->buf.size > btree->maxleafkey) {
        /*
         * Overflow objects aren't prefix compressed -- rebuild any object that was prefix
         * compressed.
         */
        if (pfx == 0) {
            WT_STAT_CONN_DSRC_INCR(session, rec_overflow_key_leaf);

            *is_ovflp = true;
            return (__wti_rec_cell_build_ovfl(session, r, key, WT_CELL_KEY_OVFL, NULL, 0));
        }
        return (__rec_cell_build_leaf_key(session, r, NULL, 0, is_ovflp));
    }

    key->cell_len = __wt_cell_pack_leaf_key(&key->cell, pfx, key->buf.size);
    key->len = key->cell_len + key->buf.size;

    return (0);
}

/*
 * __wt_bulk_insert_row --
 *     Row-store bulk insert.
 */
int
__wt_bulk_insert_row(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WTI_RECONCILE *r;
    WTI_REC_KV *key, *val;
    WT_TIME_WINDOW tw;
    bool ovfl_key;

    r = cbulk->reconcile;
    btree = S2BT(session);
    cursor = &cbulk->cbt.iface;
    WT_TIME_WINDOW_INIT(&tw);

    key = &r->k;
    val = &r->v;
    WT_RET(__rec_cell_build_leaf_key(session, r, /* Build key cell */
      cursor->key.data, cursor->key.size, &ovfl_key));
    if (cursor->value.size == 0)
        val->len = 0;
    else
        WT_RET(__wti_rec_cell_build_val(session, r, cursor->value.data, /* Build value cell */
          cursor->value.size, &tw, 0));

    /* Boundary: split or write the page. */
    if (WTI_CROSSING_SPLIT_BND(r, key->len + val->len)) {
        /*
         * Turn off prefix compression until a full key written to the new page, and (unless already
         * working with an overflow key), rebuild the key without compression.
         */
        if (r->key_pfx_compress_conf) {
            r->key_pfx_compress = false;
            r->key_pfx_last = 0;
            if (!ovfl_key)
                WT_RET(__rec_cell_build_leaf_key(session, r, NULL, 0, &ovfl_key));
        }
        WT_RET(__wti_rec_split_crossing_bnd(session, r, key->len + val->len));
    }

    /* Copy the key/value pair onto the page. */
    __wti_rec_image_copy(session, r, key);
    if (val->len == 0)
        r->any_empty_value = true;
    else {
        r->all_empty_value = false;
        if (btree->dictionary)
            WT_RET(__wti_rec_dict_replace(session, r, &tw, 0, val));
        __wti_rec_image_copy(session, r, val);
    }
    WTI_REC_CHUNK_TA_UPDATE(session, r->cur_ptr, &tw);

    /* Update compression state. */
    __rec_key_state_update(r, ovfl_key);

    return (0);
}

/*
 * __rec_row_merge --
 *     Merge in a split page.
 */
static int
__rec_row_merge(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *ref, uint8_t ref_changes, bool *build_delta)
{
    WT_ADDR *addr;
    WT_MULTI *multi;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WTI_REC_KV *key, *val;
    size_t old_key_size;
    uint32_t i;
    void *old_key;

    page = ref->page;
    mod = page->modify;

    key = &r->k;
    val = &r->v;

    /* FIXME-WT-14880: build delta for split pages. */
    if (mod->mod_multi_entries > 1) {
        /*
         * We need to remember what has been written for this ref in this internal page
         * reconciliation to be able to build the internal page delta in the future. We have to
         * remember it on the ref otherwise the information is lost if the child page is evicted.
         */
        F_SET(ref, WT_REF_FLAG_REC_MULTIPLE);
        if (*build_delta && ref_changes > 0) {
            *build_delta = false;
            r->delta.size = 0;
        }
    } else
        F_CLR(ref, WT_REF_FLAG_REC_MULTIPLE);

    /* For each entry in the split array... */
    for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
        /*
         * Build the key and value cells. We should inherit the old key for the first page if we
         * want to build a delta. Otherwise, it is difficult to build the delta for that key as it
         * can change.
         */
        if (i == 0) {
            if (*build_delta) {
                __wt_ref_key(ref->home, ref, &old_key, &old_key_size);
                WT_RET(__rec_cell_build_int_key(session, r, old_key, old_key_size));
            } else
                WT_RET(__rec_cell_build_int_key(session, r, WT_IKEY_DATA(multi->key.ikey),
                  r->cell_zero ? 1 : multi->key.ikey->size));
        } else
            WT_RET(__rec_cell_build_int_key(
              session, r, WT_IKEY_DATA(multi->key.ikey), multi->key.ikey->size));
        r->cell_zero = false;

        addr = &multi->addr;
        __wti_rec_cell_build_addr(session, r, addr, NULL, WT_RECNO_OOB, NULL);

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, key->len + val->len))
            WT_RET(__wti_rec_split_crossing_bnd(session, r, key->len + val->len));

        /* Copy the key and value onto the page. */
        __wti_rec_image_copy(session, r, key);
        __wti_rec_image_copy(session, r, val);
        WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &addr->ta);

        /* Update compression state. */
        __rec_key_state_update(r, false);

        if (*build_delta && ref_changes > 0) {
            WT_ASSERT(session, mod->mod_multi_entries == 1);
            WT_RET(__wti_rec_pack_delta_internal(session, r, key, val));
        }
    }
    return (0);
}

/*
 * __rec_build_delta_int --
 *     Build delta for Internal pages.
 */
static int
__rec_build_delta_int(WT_SESSION_IMPL *session, WTI_RECONCILE *r, bool build_delta)
{
    WT_PAGE_HEADER *header;

    if (!build_delta) {
        r->delta.size = 0;
        return (0);
    }

    WT_RET(__wti_rec_build_delta_init(session, r));
    header = (WT_PAGE_HEADER *)r->delta.data;
    header->type = r->ref->page->type;
    return (0);
}

/*
 * __wti_rec_row_int --
 *     Reconcile a row-store internal page.
 */
int
__wti_rec_row_int(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_PAGE *page)
{
    WT_ADDR *addr;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_ADDR *kpack, _kpack, *vpack, _vpack;
    WTI_CHILD_MODIFY_STATE cms;
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_PAGE *child;
    WT_PAGE_DELETED *page_del;
    WTI_REC_KV *key, *val;
    WT_REF *ref;
    WT_TIME_AGGREGATE ft_ta, *source_ta, ta;
    size_t size;
    uint8_t prev_ref_changes;
    bool build_delta, retain_onpage;
    const void *p;

    btree = S2BT(session);
    child = NULL;
    ref = NULL;
    WT_TIME_AGGREGATE_INIT_MERGE(&ft_ta);

    key = &r->k;
    kpack = &_kpack;
    WT_CLEAR(*kpack); /* -Wuninitialized */
    val = &r->v;
    vpack = &_vpack;
    WT_CLEAR(*vpack); /* -Wuninitialized */

    ikey = NULL; /* -Wuninitialized */
    cell = NULL;
    build_delta = WT_BUILD_DELTA_INT(session, r);

    WT_RET(__wti_rec_split_init(session, r, page, 0, btree->maxintlpage_precomp, 0));
    WT_RET(__rec_build_delta_int(session, r, build_delta));

    /*
     * Ideally, we'd never store the 0th key on row-store internal pages because it's never used
     * during tree search and there's no reason to waste the space. The problem is how we do splits:
     * when we split, we've potentially picked out several "split points" in the buffer which is
     * overflowing the maximum page size, and when the overflow happens, we go back and physically
     * split the buffer, at those split points, into new pages. It would be both difficult and
     * expensive to re-process the 0th key at each split point to be an empty key, so we don't do
     * that. However, we are reconciling an internal page for whatever reason, and the 0th key is
     * known to be useless. We truncate the key to a single byte, instead of removing it entirely,
     * it simplifies various things in other parts of the code (we don't have to special case
     * transforming the page from its disk image to its in-memory version, for example).
     */
    r->cell_zero = true;

    /* For each entry in the in-memory page... */
    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        /*
         * FIXME-WT-14880: build delta for split pages.
         *
         * Stop building the delta if the page has ever been split.
         *
         * We write the child as multiple keys in the previous reconciliation. In this case, we
         * cannot delete the keys written in the previous reconciliation if we build a delta. Stop
         * building a delta.
         */
        if (build_delta && (r->multi_next > 0 || F_ISSET(ref, WT_REF_FLAG_REC_MULTIPLE))) {
            build_delta = false;
            r->delta.size = 0;
        }

        WT_ACQUIRE_READ(prev_ref_changes, ref->ref_changes);
        retain_onpage = false;

        /*
         * There are different paths if the key is an overflow item vs. a straight-forward on-page
         * value. If an overflow item, we would have instantiated it, and we can use that fact to
         * set things up.
         *
         * Note the cell reference and unpacked key cell are available only in the case of an
         * instantiated, off-page key, we don't bother setting them if that's not possible.
         */
        cell = NULL;
        ikey = __wt_ref_key_instantiated(ref);
        if (ikey != NULL && ikey->cell_offset != 0) {
            cell = WT_PAGE_REF_OFFSET(page, ikey->cell_offset);
            __wt_cell_unpack_addr(session, page->dsk, cell, kpack);

            /*
             * Historically, we stored overflow cookies on internal pages, discard any underlying
             * blocks. We have a copy to build the key (the key was instantiated when we read the
             * page into memory), they won't be needed in the future as we're rewriting the page.
             */
            if (F_ISSET(kpack, WT_CELL_UNPACK_OVERFLOW) && kpack->raw != WT_CELL_KEY_OVFL_RM)
                WT_ERR(__wt_ovfl_discard_add(session, page, kpack->cell));
        }

        WT_ERR(__wti_rec_child_modify(session, r, ref, &cms, &build_delta));
        addr = ref->addr;
        child = ref->page;

        switch (cms.state) {
        case WTI_CHILD_IGNORE:
            /*
             * Cannot build delta if we decide to delete the first key. The first key on the
             * internal page is a random value. If we delete that, the next key will become the new
             * first key, which is a random value. We cannot reconstruct the delta in this case as
             * the key has changed.
             */
            if (build_delta && r->cell_zero) {
                build_delta = false;
                r->delta.size = 0;
            }

            if (build_delta && prev_ref_changes > 0) {
                __wt_ref_key(page, ref, &p, &size);
                WT_ERR(__rec_cell_build_int_key(session, r, p, size));
                WT_ERR(__wti_rec_pack_delta_internal(session, r, key, NULL));
            }

            /*
             * Set the ref_changes state to zero if there were no concurrent changes while
             * reconciling the internal page.
             */
            if (WT_DELTA_INT_ENABLED(btree, S2C(session)))
                __wt_atomic_casv8(&ref->ref_changes, prev_ref_changes, 0);

            F_CLR(ref, WT_REF_FLAG_REC_MULTIPLE);
            /*
             * Ignored child.
             */
            WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
            continue;

        case WTI_CHILD_MODIFIED:
            /*
             * Modified child. Empty pages are merged into the parent and discarded.
             */
            switch (child->modify->rec_result) {
            case WT_PM_REC_EMPTY:
                /* Cannot build delta if we decide to delete the first key. */
                if (build_delta && r->cell_zero) {
                    build_delta = false;
                    r->delta.size = 0;
                }

                if (build_delta && prev_ref_changes > 0) {
                    __wt_ref_key(page, ref, &p, &size);
                    WT_ERR(__rec_cell_build_int_key(session, r, p, size));
                    WT_ERR(__wti_rec_pack_delta_internal(session, r, key, NULL));
                }

                /*
                 * Set the ref_changes state to zero if there were no concurrent changes while
                 * reconciling the internal page.
                 */
                if (WT_DELTA_INT_ENABLED(btree, S2C(session)))
                    __wt_atomic_casv8(&ref->ref_changes, prev_ref_changes, 0);

                F_CLR(ref, WT_REF_FLAG_REC_MULTIPLE);
                WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
                continue;
            case WT_PM_REC_MULTIBLOCK:
                WT_ERR(__rec_row_merge(session, r, ref, prev_ref_changes, &build_delta));

                /*
                 * Set the ref_changes state to zero if there were no concurrent changes while
                 * reconciling the internal page.
                 */
                if (WT_DELTA_INT_ENABLED(btree, S2C(session)))
                    __wt_atomic_casv8(&ref->ref_changes, prev_ref_changes, 0);

                WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
                continue;
            case WT_PM_REC_REPLACE:
                /*
                 * If the page is replaced, the page's modify structure has the page's address. If
                 * we skipped writing an empty delta, we write the current address.
                 */
                if (child->modify->mod_replace.block_cookie != NULL)
                    addr = &child->modify->mod_replace;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, child->modify->rec_result));
            }
            break;
        case WTI_CHILD_ORIGINAL:
            /* Original child. */
            break;
        case WTI_CHILD_PROXY:
            /* Fast-delete child where we write a proxy cell. */
            break;
        }

        /*
         * Build the value cell, the child page's address. Addr points to an on-page cell or an
         * off-page WT_ADDR structure.
         */
        page_del = NULL;
        if (__wt_off_page(page, addr)) {
            page_del = cms.state == WTI_CHILD_PROXY ? &cms.del : NULL;
            __wti_rec_cell_build_addr(session, r, addr, NULL, WT_RECNO_OOB, page_del);
            source_ta = &addr->ta;
        } else if (cms.state == WTI_CHILD_PROXY) {
            /* Proxy cells require additional information in the address cell. */
            __wt_cell_unpack_addr(session, page->dsk, ref->addr, vpack);
            page_del = &cms.del;
            __wti_rec_cell_build_addr(session, r, NULL, vpack, WT_RECNO_OOB, page_del);
            source_ta = &vpack->ta;
        } else {
            retain_onpage = true;

            /*
             * We may see a changed state here if the child reconciliation skipped writing an empty
             * delta.
             */
            WT_ASSERT_ALWAYS(session,
              cms.state == WTI_CHILD_ORIGINAL || WT_DELTA_ENABLED_FOR_PAGE(session, page->type),
              "Not propagating the original fast-truncate information");
            /*
             * The transaction ids are cleared after restart. Repack the cell with new validity
             * information to flush cleared transaction ids.
             */
            __wt_cell_unpack_addr(session, page->dsk, ref->addr, vpack);

            /* The proxy cells of fast truncate pages must be handled in the above flows. */
            WT_ASSERT_ALWAYS(session, vpack->type != WT_CELL_ADDR_DEL,
              "Proxy cell is selected with original child image");

            if (F_ISSET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED)) {
                __wti_rec_cell_build_addr(session, r, NULL, vpack, WT_RECNO_OOB, page_del);
            } else {
                val->buf.data = ref->addr;
                val->buf.size = __wt_cell_total_len(vpack);
                val->cell_len = 0;
                val->len = val->buf.size;
            }
            source_ta = &vpack->ta;
        }

        /*
         * Track the time window. The fast-truncate is a stop time window and has to be considered
         * in the internal page's aggregate information for RTS to find it.
         */
        WT_TIME_AGGREGATE_COPY(&ta, source_ta);
        if (page_del != NULL)
            WT_TIME_AGGREGATE_UPDATE_PAGE_DEL(session, &ft_ta, page_del);

        F_CLR(ref, WT_REF_FLAG_REC_MULTIPLE);
        WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);

        /* Build key cell. Truncate any 0th key, internal pages don't need 0th keys. */
        __wt_ref_key(page, ref, &p, &size);

        /*
         * Modifying keys when building delta can get us into trouble so it's best not to truncate
         * the first key when building delta.
         */
        if (r->cell_zero && !build_delta)
            size = 1;
        WT_ERR(__rec_cell_build_int_key(session, r, p, size));
        r->cell_zero = false;

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, key->len + val->len))
            WT_ERR(__wti_rec_split_crossing_bnd(session, r, key->len + val->len));

        /* Copy the key and value onto the page. */
        __wti_rec_image_copy(session, r, key);
        __wti_rec_image_copy(session, r, val);
        if (page_del != NULL)
            WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &ft_ta);
        WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &ta);

        /* Update compression state. */
        __rec_key_state_update(r, false);

        if (build_delta && prev_ref_changes > 0 && !retain_onpage)
            WT_ERR(__wti_rec_pack_delta_internal(session, r, key, val));

        /*
         * Set the ref_changes state to zero if there were no concurrent changes while reconciling
         * the internal page.
         */
        if (WT_DELTA_INT_ENABLED(btree, S2C(session)))
            __wt_atomic_casv8(&ref->ref_changes, prev_ref_changes, 0);
    }
    WT_INTL_FOREACH_END;

    /* Write the remnant page. */
    return (__wti_rec_split_finish(session, r));

err:
    WTI_CHILD_RELEASE(session, cms.hazard, ref);
    return (ret);
}

/*
 * __rec_row_zero_len --
 *     Return if a zero-length item can be written.
 */
static bool
__rec_row_zero_len(WT_SESSION_IMPL *session, WT_TIME_WINDOW *tw)
{
    /*
     * The item must be globally visible because we're not writing anything on the page. Don't be
     * tempted to check the time window against the default here - the check is subtly different due
     * to the grouping.
     *
     * tw->start_ts == WT_TS_NONE && tw->start_txn == WT_TXN_NONE is a simpler check and can bypass
     * evaluating the more expensive __wt_txn_tw_start_visible_all check.
     */
    return (!WT_TIME_WINDOW_HAS_STOP(tw) &&
      ((tw->start_ts == WT_TS_NONE && tw->start_txn == WT_TXN_NONE) ||
        __wt_txn_tw_start_visible_all(session, tw)));
}

/*
 * __rec_row_garbage_collect_fixup_update_list --
 *     Insert a tombstone at the start of an update list if all entries are eligible for garbage
 *     collection. There is duplication between the update list and insert list versions of these
 *     functions but my head explodes trying to keep the data structures involved mapped in my head,
 *     so the duplication feels warranted. Don't bother tracking the additional memory associated
 *     with these tombstones - it is about to be freed anyway.
 */
static int
__rec_row_garbage_collect_fixup_update_list(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_ROW *rip)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;
    WT_UPDATE *first_upd, *tombstone, *upd, **upd_entry;

    btree = S2BT(session);
    page = r->page;
    mod = page->modify;

    if (!F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT) || !F_ISSET(r, WT_REC_EVICT))
        return (0);

    if ((first_upd = WT_ROW_UPDATE(page, rip)) == NULL)
        return (0);

    for (upd = first_upd; upd != NULL && upd->txnid == WT_TXN_ABORTED; upd = upd->next)
        ;

    if (upd == NULL)
        return (0);

    if (upd->type == WT_UPDATE_TOMBSTONE)
        return (0);

    if (upd->txnid < r->rec_start_oldest_id && r->rec_prune_timestamp != WT_TS_NONE &&
      upd->upd_durable_ts <= r->rec_prune_timestamp) {
        WT_RET(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
        tombstone->next = first_upd;
        upd_entry = &mod->mod_row_update[WT_ROW_SLOT(page, rip)];
        *upd_entry = tombstone;

        WT_STAT_CONN_DSRC_INCR(session, rec_ingest_garbage_collection_keys);
    }

    return (0);
}

/*
 * __rec_row_garbage_collect_fixup_insert_list --
 *     Insert a tombstone at the start of an insert list if all entries are eligible for garbage
 *     collection.
 */
static int
__rec_row_garbage_collect_fixup_insert_list(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins)
{
    WT_BTREE *btree;
    WT_UPDATE *first_upd, *tombstone, *upd;

    btree = S2BT(session);

    if (!F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT) || !F_ISSET(r, WT_REC_EVICT))
        return (0);

    /* The insert list should have an update, but be paranoid */
    if ((first_upd = ins->upd) == NULL)
        return (0);

    for (upd = first_upd; upd != NULL && upd->txnid == WT_TXN_ABORTED; upd = upd->next)
        ;

    if (upd == NULL)
        return (0);

    if (upd->type == WT_UPDATE_TOMBSTONE)
        return (0);

    if (upd->txnid < r->rec_start_oldest_id && r->rec_prune_timestamp != WT_TS_NONE &&
      upd->upd_durable_ts <= r->rec_prune_timestamp) {
        WT_RET(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
        tombstone->next = first_upd;
        ins->upd = tombstone;

        WT_STAT_CONN_DSRC_INCR(session, rec_ingest_garbage_collection_keys);
    }

    return (0);
}

/*
 * __rec_row_leaf_insert --
 *     Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_INSERT *ins)
{
    WT_BTREE *btree;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(tmpkey);
    WT_DECL_RET;
    WTI_REC_KV *key, *val;
    WT_TIME_WINDOW tw;
    WT_UPDATE *upd;
    WTI_UPDATE_SELECT upd_select;
    bool ovfl_key;

    btree = S2BT(session);

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    key = &r->k;
    val = &r->v;

    upd = NULL;

    /* Temporary buffer in which to instantiate any uninstantiated keys or value items we need. */
    WT_RET(__wt_scr_alloc(session, 0, &tmpkey));

    for (; ins != NULL; ins = WT_SKIP_NEXT(ins)) {
        WT_ERR(__rec_row_garbage_collect_fixup_insert_list(session, r, ins));
        WT_ERR(__wti_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
        if ((upd = upd_select.upd) == NULL) {
            /*
             * In cases where a page has grown so large we are trying to force evict it (there is
             * content, but none of the content can be evicted), we set up fake split points, to
             * allow the page to use update restore eviction and be split into multiple reasonably
             * sized pages. Check if we are in this situation. The call to split with zero
             * additional size is odd, but split takes into account saved updates in a special way
             * for this case already.
             */
            if (!upd_select.upd_saved || !__wti_rec_need_split(r, 0))
                continue;

            WT_ERR(__wt_buf_set(session, r->cur, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins)));
            WT_ERR(__wti_rec_split_crossing_bnd(session, r, 0));

            /*
             * Turn off prefix and suffix compression until a full key is written into the new page.
             */
            r->key_pfx_compress = r->key_sfx_compress = false;
            r->key_pfx_last = 0;
            continue;
        }

        /*
         * If we've selected an update, it should be flagged as being destined for the data store.
         *
         * If not, it's either because we're not doing a history store reconciliation or because the
         * update is globally visible (in which case, subsequent updates become irrelevant for
         * reconciliation).
         */
        WT_ASSERT(session,
          F_ISSET(upd, WT_UPDATE_DS) || !F_ISSET(r, WT_REC_HS) ||
            __wt_txn_tw_start_visible_all(session, &upd_select.tw));

        WT_TIME_WINDOW_COPY(&tw, &upd_select.tw);

        switch (upd->type) {
        case WT_UPDATE_MODIFY:
            /*
             * Impossible slot, there's no backing on-page item.
             */
            cbt->slot = UINT32_MAX;
            WT_ERR(__wt_modify_reconstruct_from_upd_list(
              session, cbt, upd, cbt->upd_value, WT_OPCTX_RECONCILATION));
            __wt_value_return(cbt, cbt->upd_value);
            WT_ERR(__wti_rec_cell_build_val(
              session, r, cbt->iface.value.data, cbt->iface.value.size, &tw, 0));
            break;
        case WT_UPDATE_STANDARD:
            if (upd->size == 0 && WT_TIME_WINDOW_IS_EMPTY(&tw))
                val->len = 0;
            else
                /* Take the value from the update. */
                WT_ERR(__wti_rec_cell_build_val(session, r, upd->data, upd->size, &tw, 0));
            break;
        case WT_UPDATE_TOMBSTONE:
            break;
        default:
            WT_ERR(__wt_illegal_value(session, upd->type));
        }

        /*
         * When a tombstone without a timestamp is written to disk, remove any historical versions
         * that are greater in the history store for this key.
         */
        if (upd_select.no_ts_tombstone && r->hs_clear_on_tombstone) {
            tmpkey->data = WT_INSERT_KEY(ins);
            tmpkey->size = WT_INSERT_KEY_SIZE(ins);
            WT_ERR(__wti_rec_hs_clear_on_tombstone(
              session, r, WT_RECNO_OOB, tmpkey, upd->type == WT_UPDATE_TOMBSTONE ? false : true));
        }

        if (upd->type == WT_UPDATE_TOMBSTONE)
            continue;

        /* Build key cell. */
        WT_ERR(__rec_cell_build_leaf_key(
          session, r, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), &ovfl_key));

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, key->len + val->len)) {
            /*
             * Turn off prefix compression until a full key written to the new page, and (unless
             * already working with an overflow key), rebuild the key without compression.
             */
            if (r->key_pfx_compress_conf) {
                r->key_pfx_compress = false;
                r->key_pfx_last = 0;
                if (!ovfl_key)
                    WT_ERR(__rec_cell_build_leaf_key(session, r, NULL, 0, &ovfl_key));
            }

            WT_ERR(__wti_rec_split_crossing_bnd(session, r, key->len + val->len));
        }

        /* Copy the key/value pair onto the page. */
        __wti_rec_image_copy(session, r, key);
        if (val->len == 0 && __rec_row_zero_len(session, &tw))
            r->any_empty_value = true;
        else {
            r->all_empty_value = false;
            if (btree->dictionary)
                WT_ERR(__wti_rec_dict_replace(session, r, &tw, 0, val));
            __wti_rec_image_copy(session, r, val);
        }
        WTI_REC_CHUNK_TA_UPDATE(session, r->cur_ptr, &tw);

        /* Update compression state. */
        __rec_key_state_update(r, ovfl_key);
    }

err:
    __wt_scr_free(session, &tmpkey);
    return (ret);
}

/*
 * __rec_cell_repack --
 *     Repack a cell.
 */
static WT_INLINE int
__rec_cell_repack(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_CELL_UNPACK_KV *vpack, WT_TIME_WINDOW *tw)
{
    WT_DECL_ITEM(tmpval);
    WT_DECL_RET;
    size_t size;
    const void *p;

    WT_ERR(__wt_scr_alloc(session, 0, &tmpval));

    p = vpack->data;
    size = vpack->size;
    WT_ERR(__wti_rec_cell_build_val(session, r, p, size, tw, 0));

err:
    __wt_scr_free(session, &tmpval);
    return (ret);
}

/*
 * __wti_rec_row_leaf --
 *     Reconcile a row-store leaf page.
 */
int
__wti_rec_row_leaf(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    static WT_UPDATE upd_tombstone = {.txnid = WT_TXN_NONE, .type = WT_UPDATE_TOMBSTONE};
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *kpack, _kpack, *vpack, _vpack;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(lastkey);
    WT_DECL_ITEM(tmpkey);
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_INSERT *ins;
    WT_PAGE *page;
    WTI_REC_KV *key, *val;
    WT_ROW *rip;
    WT_TIME_WINDOW *twp;
    WT_UPDATE *upd;
    WTI_UPDATE_SELECT upd_select;
    size_t key_size;
    uint64_t slvg_skip;
    uint32_t i;
    uint8_t key_prefix;
    bool dictionary, key_onpage_ovfl, ovfl_key;
    void *copy;
    const void *key_data;

    btree = S2BT(session);
    conn = S2C(session);
    page = pageref->page;
    twp = NULL;
    upd = NULL;
    slvg_skip = salvage == NULL ? 0 : salvage->skip;

    key = &r->k;
    val = &r->v;
    vpack = &_vpack;

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    WT_RET(__wti_rec_split_init(session, r, page, 0, btree->maxleafpage_precomp, 0));

    /*
     * Write any K/V pairs inserted into the page before the first from-disk key on the page.
     */
    if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT_SMALLEST(page))) != NULL)
        WT_RET(__rec_row_leaf_insert(session, r, ins));

    /*
     * When we walk the page, we store each key we're building for the disk image in the last-key
     * buffer. There's trickiness because it's significantly faster to use a previously built key
     * plus the next key's prefix count to build the next key (rather than to call some underlying
     * function to do it from scratch). In other words, we put each key into the last-key buffer,
     * then use it to create the next key, again storing the result into the last-key buffer. If we
     * don't build a key for any reason (imagine we skip a key because the value was deleted), clear
     * the last-key buffer size so it's not used to fast-path building the next key.
     */
    WT_ERR(__wt_scr_alloc(session, 0, &lastkey));

    /* Temporary buffer in which to instantiate any uninstantiated keys or value items we need. */
    WT_ERR(__wt_scr_alloc(session, 0, &tmpkey));

    /* For each entry in the page... */
    WT_ROW_FOREACH (page, rip, i) {
        /*
         * The salvage code, on some rare occasions, wants to reconcile a page but skip some leading
         * records on the page. Because the row-store leaf reconciliation function copies keys from
         * the original disk page, this is non-trivial -- just changing the in-memory pointers isn't
         * sufficient, we have to change the WT_CELL structures on the disk page, too. It's ugly,
         * but we pass in a value that tells us how many records to skip in this case.
         */
        if (slvg_skip != 0) {
            --slvg_skip;
            continue;
        }
        dictionary = false;

        /*
         * Figure out if the key is an overflow key, and in that case unpack the cell, we'll need it
         * later.
         */
        copy = WT_ROW_KEY_COPY(rip);
        __wt_row_leaf_key_info(page, copy, &ikey, &cell, &key_data, &key_size, &key_prefix);
        kpack = NULL;
        if (__wt_cell_type(cell) == WT_CELL_KEY_OVFL) {
            kpack = &_kpack;
            __wt_cell_unpack_kv(session, page->dsk, cell, kpack);
        }

        /* Unpack the on-page value cell. */
        __wt_row_leaf_value_cell(session, page, rip, vpack);

        /* Give garbage collected tables a change to mark obsolete content for cleanup */
        WT_ERR(__rec_row_garbage_collect_fixup_update_list(session, r, rip));

        /* Look for an update. */
        WT_ERR(__wti_rec_upd_select(session, r, NULL, rip, vpack, &upd_select));
        upd = upd_select.upd;

        /* Take the timestamp from the update or the cell. */
        if (upd == NULL) {
            twp = &vpack->tw;
            /*
             * If preserve prepared update is enabled, we must select an update to replace the
             * onpage prepared update. Otherwise, we leak the prepared update.
             */
            WT_ASSERT_ALWAYS(session,
              !F_ISSET(conn, WT_CONN_PRESERVE_PREPARED) || !WT_TIME_WINDOW_HAS_PREPARE(twp),
              "leaked prepared update.");
        } else
            twp = &upd_select.tw;

        /*
         * If we reconcile an on disk key with a globally visible stop time point and there are no
         * new updates for that key, skip writing that key. Or if garbage collection is enabled for
         * the table, and the value has become obsolete.
         */
        if (upd == NULL) {
            if (__wt_txn_tw_stop_visible_all(session, twp))
                upd = &upd_tombstone;
            else if (F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT)) {
                if (WT_TIME_WINDOW_HAS_STOP(twp)) {
                    if (twp->stop_txn < r->rec_start_oldest_id &&
                      r->rec_prune_timestamp != WT_TS_NONE &&
                      twp->durable_stop_ts <= r->rec_prune_timestamp) {
                        upd = &upd_tombstone;
                        WT_STAT_CONN_DSRC_INCR(session, rec_ingest_garbage_collection_keys);
                    }
                } else {
                    if (twp->start_txn < r->rec_start_oldest_id &&
                      r->rec_prune_timestamp != WT_TS_NONE &&
                      twp->durable_start_ts <= r->rec_prune_timestamp) {
                        upd = &upd_tombstone;
                        WT_STAT_CONN_DSRC_INCR(session, rec_ingest_garbage_collection_keys);
                    }
                }
            }
        }

        /* Build value cell. */
        if (upd == NULL) {
            /* Clear the on-disk cell time window if it is obsolete. */
            __wti_rec_time_window_clear_obsolete(session, NULL, vpack, r);

            /*
             * When the page was read into memory, there may not have been a value item.
             *
             * If there was a value item, check if it's a dictionary cell (a copy of another item on
             * the page). If it's a copy, we have to create a new value item as the old item might
             * have been discarded from the page.
             *
             * Repack the cell if we clear the transaction ids in the cell.
             */
            if (vpack->raw == WT_CELL_VALUE_COPY) {
                WT_ERR(__rec_cell_repack(session, r, vpack, twp));

                dictionary = true;
            } else if (F_ISSET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED)) {
                /*
                 * The transaction ids are cleared after restart. Repack the cell to flush the
                 * cleared transaction ids.
                 */
                if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)) {
                    r->ovfl_items = true;

                    val->buf.data = vpack->data;
                    val->buf.size = vpack->size;

                    /* Rebuild the cell. */
                    val->cell_len =
                      __wt_cell_pack_ovfl(session, &val->cell, vpack->raw, twp, 0, val->buf.size);
                    val->len = val->cell_len + val->buf.size;
                } else
                    WT_ERR(__rec_cell_repack(session, r, vpack, twp));

                dictionary = true;
            } else {
                val->buf.data = vpack->cell;
                val->buf.size = __wt_cell_total_len(vpack);
                val->cell_len = 0;
                val->len = val->buf.size;

                /* Track if page has overflow items. */
                if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW))
                    r->ovfl_items = true;
            }
        } else {
            /*
             * If we've selected an update, it should be flagged as being destined for the data
             * store.
             *
             * If not, it's either because we're not doing a history store reconciliation or because
             * the update is globally visible (in which case, subsequent updates become irrelevant
             * for reconciliation).
             */
            WT_ASSERT(session,
              F_ISSET(upd, WT_UPDATE_DS) || !F_ISSET(r, WT_REC_HS) ||
                __wt_txn_tw_start_visible_all(session, twp) ||
                (F_ISSET(btree, WT_BTREE_GARBAGE_COLLECT) &&
                  twp->start_txn < r->rec_start_oldest_id && r->rec_prune_timestamp != WT_TS_NONE &&
                  twp->durable_start_ts <= r->rec_prune_timestamp));

            /* The first time we find an overflow record, discard the underlying blocks. */
            if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW) && vpack->raw != WT_CELL_VALUE_OVFL_RM)
                WT_ERR(__wt_ovfl_remove(session, page, vpack));

            switch (upd->type) {
            case WT_UPDATE_MODIFY:
                cbt->slot = WT_ROW_SLOT(page, rip);
                WT_ERR(__wt_modify_reconstruct_from_upd_list(
                  session, cbt, upd, cbt->upd_value, WT_OPCTX_RECONCILATION));
                __wt_value_return(cbt, cbt->upd_value);
                WT_ERR(__wti_rec_cell_build_val(
                  session, r, cbt->iface.value.data, cbt->iface.value.size, twp, 0));
                dictionary = true;
                break;
            case WT_UPDATE_STANDARD:
                /* Take the value from the update. */
                WT_ERR(__wti_rec_cell_build_val(session, r, upd->data, upd->size, twp, 0));
                dictionary = true;
                break;
            case WT_UPDATE_TOMBSTONE:
                /*
                 * If this key/value pair was deleted, we're done.
                 *
                 * Overflow keys referencing discarded values are no longer useful, discard the
                 * backing blocks. Don't worry about reuse, reusing keys from a row-store page
                 * reconciliation seems unlikely enough to ignore.
                 */
                if (kpack != NULL && F_ISSET(kpack, WT_CELL_UNPACK_OVERFLOW) &&
                  kpack->raw != WT_CELL_KEY_OVFL_RM) {
                    /*
                     * Keys are part of the name-space, we can't remove them. If an overflow key was
                     * deleted without ever having been instantiated, instantiate it now so future
                     * searches aren't surprised when it's marked as cleared in the on-disk image.
                     */
                    if (ikey == NULL)
                        WT_ERR(__wt_row_leaf_key(session, page, rip, tmpkey, true));

                    WT_ERR(__wt_ovfl_discard_add(session, page, kpack->cell));
                }

                /* Not creating a key so we can't use last-key as a prefix for a subsequent key. */
                lastkey->size = 0;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, upd->type));
            }

            /*
             * When a tombstone without a timestamp is written to disk, remove any historical
             * versions that are greater in the history store for this key.
             */
            if (upd_select.no_ts_tombstone && r->hs_clear_on_tombstone) {
                WT_ERR(__wt_row_leaf_key(session, page, rip, tmpkey, true));
                WT_ERR(__wti_rec_hs_clear_on_tombstone(session, r, WT_RECNO_OOB, tmpkey,
                  upd->type == WT_UPDATE_TOMBSTONE ? false : true));
            }

            /* Proceed with appended key/value pairs. */
            if (upd->type == WT_UPDATE_TOMBSTONE)
                goto leaf_insert;
        }

        /*
         * Build key cell.
         *
         * If the key is an overflow key that hasn't been removed, use the original backing blocks.
         */
        key_onpage_ovfl = kpack != NULL && F_ISSET(kpack, WT_CELL_UNPACK_OVERFLOW) &&
          kpack->raw != WT_CELL_KEY_OVFL_RM;
        if (key_onpage_ovfl) {
            key->buf.data = cell;
            key->buf.size = __wt_cell_total_len(kpack);
            key->cell_len = 0;
            key->len = key->buf.size;
            ovfl_key = true;

            /* Not creating a key so we can't use last-key as a prefix for a subsequent key. */
            lastkey->size = 0;

            /* Track if page has overflow items. */
            r->ovfl_items = true;
        } else {
            /*
             * Get the key from the page or an instantiated key, or inline building the key from a
             * previous key (it's a fast path for simple, prefix-compressed keys), or by building
             * the key from scratch.
             */
            __wt_row_leaf_key_info(page, copy, NULL, &cell, &key_data, &key_size, &key_prefix);
            if (key_data == NULL) {
                if (__wt_cell_type(cell) != WT_CELL_KEY)
                    goto slow;
                kpack = &_kpack;
                __wt_cell_unpack_kv(session, page->dsk, cell, kpack);
                key_data = kpack->data;
                key_size = kpack->size;
                key_prefix = kpack->prefix;
            }

            /*
             * If the key has no prefix count, no prefix compression work is needed; else check for
             * a previously built key big enough cover this key's prefix count, else build from
             * scratch.
             */
            if (key_prefix == 0) {
                lastkey->data = key_data;
                lastkey->size = key_size;
            } else if (lastkey->size >= key_prefix) {
                /*
                 * Grow the buffer as necessary as well as ensure data has been copied into local
                 * buffer space, then append the suffix to the prefix already in the buffer. Don't
                 * grow the buffer unnecessarily or copy data we don't need, truncate the item's
                 * CURRENT data length to the prefix bytes before growing the buffer.
                 */
                lastkey->size = key_prefix;
                WT_ERR(__wt_buf_grow(session, lastkey, key_prefix + key_size));
                memcpy((uint8_t *)lastkey->mem + key_prefix, key_data, key_size);
                lastkey->size = key_prefix + key_size;
            } else {
slow:
                WT_ERR(__wt_row_leaf_key_copy(session, page, rip, lastkey));
            }

            WT_ERR(__rec_cell_build_leaf_key(session, r, lastkey->data, lastkey->size, &ovfl_key));
        }

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, key->len + val->len)) {
            /*
             * If we copied address blocks from the page rather than building the actual key, we
             * have to build the key now because we are about to promote it.
             */
            if (key_onpage_ovfl) {
                WT_ERR(__wt_dsk_cell_data_ref_kv(session, kpack, r->cur));
                WT_NOT_READ(key_onpage_ovfl, false);
            }

            /*
             * Turn off prefix compression until a full key written to the new page, and (unless
             * already working with an overflow key), rebuild the key without compression.
             */
            if (r->key_pfx_compress_conf) {
                r->key_pfx_compress = false;
                r->key_pfx_last = 0;
                if (!ovfl_key)
                    WT_ERR(__rec_cell_build_leaf_key(session, r, NULL, 0, &ovfl_key));
            }

            WT_ERR(__wti_rec_split_crossing_bnd(session, r, key->len + val->len));
        }

        /* Copy the key/value pair onto the page. */
        __wti_rec_image_copy(session, r, key);
        if (val->len == 0 && __rec_row_zero_len(session, twp))
            r->any_empty_value = true;
        else {
            r->all_empty_value = false;
            if (dictionary && btree->dictionary)
                WT_ERR(__wti_rec_dict_replace(session, r, twp, 0, val));
            __wti_rec_image_copy(session, r, val);
        }
        WTI_REC_CHUNK_TA_UPDATE(session, r->cur_ptr, twp);

        /* Update compression state. */
        __rec_key_state_update(r, ovfl_key);

leaf_insert:
        /* Write any K/V pairs inserted into the page after this key. */
        if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT(page, rip))) != NULL)
            WT_ERR(__rec_row_leaf_insert(session, r, ins));
    }

    /* Write the remnant page. */
    ret = __wti_rec_split_finish(session, r);

err:
    __wt_scr_free(session, &lastkey);
    __wt_scr_free(session, &tmpkey);
    return (ret);
}
