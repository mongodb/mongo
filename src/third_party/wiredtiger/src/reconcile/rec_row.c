/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_key_state_update --
 *     Update prefix and suffix compression based on the last key.
 */
static inline void
__rec_key_state_update(WT_RECONCILE *r, bool ovfl_key)
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
__rec_cell_build_int_key(WT_SESSION_IMPL *session, WT_RECONCILE *r, const void *data, size_t size)
{
    WT_REC_KV *key;

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
  WT_SESSION_IMPL *session, WT_RECONCILE *r, const void *data, size_t size, bool *is_ovflp)
{
    WT_BTREE *btree;
    WT_REC_KV *key;
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
              pfx < r->key_pfx_last + WT_KEY_PREFIX_PREVIOUS_MINIMUM)
                pfx = r->key_pfx_last;

            if (pfx != 0)
                WT_STAT_DATA_INCRV(session, rec_prefix_compression, pfx);
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
            WT_STAT_CONN_DATA_INCR(session, rec_overflow_key_leaf);

            *is_ovflp = true;
            return (__wt_rec_cell_build_ovfl(session, r, key, WT_CELL_KEY_OVFL, NULL, 0));
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
    WT_RECONCILE *r;
    WT_REC_KV *key, *val;
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
        WT_RET(__wt_rec_cell_build_val(session, r, cursor->value.data, /* Build value cell */
          cursor->value.size, &tw, 0));

    /* Boundary: split or write the page. */
    if (WT_CROSSING_SPLIT_BND(r, key->len + val->len)) {
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
        WT_RET(__wt_rec_split_crossing_bnd(session, r, key->len + val->len));
    }

    /* Copy the key/value pair onto the page. */
    __wt_rec_image_copy(session, r, key);
    if (val->len == 0)
        r->any_empty_value = true;
    else {
        r->all_empty_value = false;
        if (btree->dictionary)
            WT_RET(__wt_rec_dict_replace(session, r, &tw, 0, val));
        __wt_rec_image_copy(session, r, val);
    }
    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &tw);

    /* Update compression state. */
    __rec_key_state_update(r, ovfl_key);

    return (0);
}

/*
 * __rec_row_merge --
 *     Merge in a split page.
 */
static int
__rec_row_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
    WT_ADDR *addr;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_REC_KV *key, *val;
    uint32_t i;

    mod = page->modify;

    key = &r->k;
    val = &r->v;

    /* For each entry in the split array... */
    for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
        /* Build the key and value cells. */
        WT_RET(__rec_cell_build_int_key(
          session, r, WT_IKEY_DATA(multi->key.ikey), r->cell_zero ? 1 : multi->key.ikey->size));
        r->cell_zero = false;

        addr = &multi->addr;
        __wt_rec_cell_build_addr(session, r, addr, NULL, false, WT_RECNO_OOB);

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, key->len + val->len))
            WT_RET(__wt_rec_split_crossing_bnd(session, r, key->len + val->len));

        /* Copy the key and value onto the page. */
        __wt_rec_image_copy(session, r, key);
        __wt_rec_image_copy(session, r, val);
        WT_TIME_AGGREGATE_MERGE(session, &r->cur_ptr->ta, &addr->ta);

        /* Update compression state. */
        __rec_key_state_update(r, false);
    }
    return (0);
}

/*
 * __wt_rec_row_int --
 *     Reconcile a row-store internal page.
 */
int
__wt_rec_row_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
    WT_ADDR *addr;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_ADDR *kpack, _kpack, *vpack, _vpack;
    WT_CHILD_STATE state;
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_PAGE *child;
    WT_REC_KV *key, *val;
    WT_REF *ref;
    WT_TIME_AGGREGATE ta;
    size_t size;
    bool hazard;
    const void *p;

    btree = S2BT(session);
    child = NULL;
    hazard = false;

    key = &r->k;
    kpack = &_kpack;
    WT_CLEAR(*kpack); /* -Wuninitialized */
    val = &r->v;
    vpack = &_vpack;
    WT_CLEAR(*vpack); /* -Wuninitialized */

    ikey = NULL; /* -Wuninitialized */
    cell = NULL;

    WT_RET(__wt_rec_split_init(session, r, page, 0, btree->maxintlpage_precomp, 0));

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

        WT_ERR(__wt_rec_child_modify(session, r, ref, &hazard, &state));
        addr = ref->addr;
        child = ref->page;

        switch (state) {
        case WT_CHILD_IGNORE:
            /*
             * Ignored child.
             */
            WT_CHILD_RELEASE_ERR(session, hazard, ref);
            continue;

        case WT_CHILD_MODIFIED:
            /*
             * Modified child. Empty pages are merged into the parent and discarded.
             */
            switch (child->modify->rec_result) {
            case WT_PM_REC_EMPTY:
                WT_CHILD_RELEASE_ERR(session, hazard, ref);
                continue;
            case WT_PM_REC_MULTIBLOCK:
                WT_ERR(__rec_row_merge(session, r, child));
                WT_CHILD_RELEASE_ERR(session, hazard, ref);
                continue;
            case WT_PM_REC_REPLACE:
                /*
                 * If the page is replaced, the page's modify structure has the page's address.
                 */
                addr = &child->modify->mod_replace;
                break;
            default:
                WT_ERR(__wt_illegal_value(session, child->modify->rec_result));
            }
            break;
        case WT_CHILD_ORIGINAL:
            /* Original child. */
            break;
        case WT_CHILD_PROXY:
            /* Deleted child where we write a proxy cell. */
            break;
        }

        /*
         * Build the value cell, the child page's address. Addr points to an on-page cell or an
         * off-page WT_ADDR structure. There's a special cell type in the case of page deletion
         * requiring a proxy cell, otherwise use the information from the addr or original cell.
         */
        if (__wt_off_page(page, addr)) {
            __wt_rec_cell_build_addr(session, r, addr, NULL, state == WT_CHILD_PROXY, WT_RECNO_OOB);
            WT_TIME_AGGREGATE_COPY(&ta, &addr->ta);
        } else {
            __wt_cell_unpack_addr(session, page->dsk, ref->addr, vpack);
            if (F_ISSET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED)) {
                /*
                 * The transaction ids are cleared after restart. Repack the cell with new validity
                 * to flush the cleared transaction ids.
                 */
                __wt_rec_cell_build_addr(
                  session, r, NULL, vpack, state == WT_CHILD_PROXY, WT_RECNO_OOB);
            } else if (state == WT_CHILD_PROXY) {
                WT_ERR(__wt_buf_set(session, &val->buf, ref->addr, __wt_cell_total_len(vpack)));
                __wt_cell_type_reset(session, val->buf.mem, 0, WT_CELL_ADDR_DEL);
                val->cell_len = 0;
                val->len = val->buf.size;
            } else {
                val->buf.data = ref->addr;
                val->buf.size = __wt_cell_total_len(vpack);
                val->cell_len = 0;
                val->len = val->buf.size;
            }
            WT_TIME_AGGREGATE_COPY(&ta, &vpack->ta);
        }
        WT_CHILD_RELEASE_ERR(session, hazard, ref);

        /* Build key cell. Truncate any 0th key, internal pages don't need 0th keys. */
        __wt_ref_key(page, ref, &p, &size);
        if (r->cell_zero)
            size = 1;
        WT_ERR(__rec_cell_build_int_key(session, r, p, size));
        r->cell_zero = false;

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, key->len + val->len))
            WT_ERR(__wt_rec_split_crossing_bnd(session, r, key->len + val->len));

        /* Copy the key and value onto the page. */
        __wt_rec_image_copy(session, r, key);
        __wt_rec_image_copy(session, r, val);
        WT_TIME_AGGREGATE_MERGE(session, &r->cur_ptr->ta, &ta);

        /* Update compression state. */
        __rec_key_state_update(r, false);
    }
    WT_INTL_FOREACH_END;

    /* Write the remnant page. */
    return (__wt_rec_split_finish(session, r));

err:
    WT_CHILD_RELEASE(session, hazard, ref);
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
     */
    return (!WT_TIME_WINDOW_HAS_STOP(tw) &&
      ((tw->start_ts == WT_TS_NONE && tw->start_txn == WT_TXN_NONE) ||
        __wt_txn_tw_start_visible_all(session, tw)));
}

/*
 * __rec_row_leaf_insert --
 *     Walk an insert chain, writing K/V pairs.
 */
static int
__rec_row_leaf_insert(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_INSERT *ins)
{
    WT_BTREE *btree;
    WT_CURSOR_BTREE *cbt;
    WT_REC_KV *key, *val;
    WT_TIME_WINDOW tw;
    WT_UPDATE *upd;
    WT_UPDATE_SELECT upd_select;
    bool ovfl_key;

    btree = S2BT(session);

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    key = &r->k;
    val = &r->v;

    upd = NULL;

    for (; ins != NULL; ins = WT_SKIP_NEXT(ins)) {
        WT_RET(__wt_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
        if ((upd = upd_select.upd) == NULL) {
            /*
             * In cases where a page has grown so large we are trying to force evict it (there is
             * content, but none of the content can be evicted), we set up fake split points, to
             * allow the page to use update restore eviction and be split into multiple reasonably
             * sized pages. Check if we are in this situation. The call to split with zero
             * additional size is odd, but split takes into account saved updates in a special way
             * for this case already.
             */
            if (!upd_select.upd_saved || !__wt_rec_need_split(r, 0))
                continue;

            WT_RET(__wt_buf_set(session, r->cur, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins)));
            WT_RET(__wt_rec_split_crossing_bnd(session, r, 0));

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
            WT_RET(__wt_modify_reconstruct_from_upd_list(session, cbt, upd, cbt->upd_value));
            __wt_value_return(cbt, cbt->upd_value);
            WT_RET(__wt_rec_cell_build_val(
              session, r, cbt->iface.value.data, cbt->iface.value.size, &tw, 0));
            break;
        case WT_UPDATE_STANDARD:
            if (upd->size == 0 && WT_TIME_WINDOW_IS_EMPTY(&tw))
                val->len = 0;
            else
                /* Take the value from the update. */
                WT_RET(__wt_rec_cell_build_val(session, r, upd->data, upd->size, &tw, 0));
            break;
        case WT_UPDATE_TOMBSTONE:
            continue;
        default:
            WT_RET(__wt_illegal_value(session, upd->type));
        }
        /* Build key cell. */
        WT_RET(__rec_cell_build_leaf_key(
          session, r, WT_INSERT_KEY(ins), WT_INSERT_KEY_SIZE(ins), &ovfl_key));

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, key->len + val->len)) {
            /*
             * Turn off prefix compression until a full key written to the new page, and (unless
             * already working with an overflow key), rebuild the key without compression.
             */
            if (r->key_pfx_compress_conf) {
                r->key_pfx_compress = false;
                r->key_pfx_last = 0;
                if (!ovfl_key)
                    WT_RET(__rec_cell_build_leaf_key(session, r, NULL, 0, &ovfl_key));
            }

            WT_RET(__wt_rec_split_crossing_bnd(session, r, key->len + val->len));
        }

        /* Copy the key/value pair onto the page. */
        __wt_rec_image_copy(session, r, key);
        if (val->len == 0 && __rec_row_zero_len(session, &tw))
            r->any_empty_value = true;
        else {
            r->all_empty_value = false;
            if (btree->dictionary)
                WT_RET(__wt_rec_dict_replace(session, r, &tw, 0, val));
            __wt_rec_image_copy(session, r, val);
        }
        WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &tw);

        /* Update compression state. */
        __rec_key_state_update(r, ovfl_key);
    }

    return (0);
}

/*
 * __rec_cell_repack --
 *     Repack a cell.
 */
static inline int
__rec_cell_repack(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_RECONCILE *r,
  WT_CELL_UNPACK_KV *vpack, WT_TIME_WINDOW *tw)
{
    WT_DECL_ITEM(tmpval);
    WT_DECL_RET;
    size_t size;
    const void *p;

    WT_ERR(__wt_scr_alloc(session, 0, &tmpval));

    /* If the item is Huffman encoded, decode it. */
    if (btree->huffman_value == NULL) {
        p = vpack->data;
        size = vpack->size;
    } else {
        WT_ERR(
          __wt_huffman_decode(session, btree->huffman_value, vpack->data, vpack->size, tmpval));
        p = tmpval->data;
        size = tmpval->size;
    }
    WT_ERR(__wt_rec_cell_build_val(session, r, p, size, tw, 0));

err:
    __wt_scr_free(session, &tmpval);
    return (ret);
}

/*
 * __wt_rec_row_leaf --
 *     Reconcile a row-store leaf page.
 */
int
__wt_rec_row_leaf(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    static WT_UPDATE upd_tombstone = {.txnid = WT_TXN_NONE, .type = WT_UPDATE_TOMBSTONE};
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *kpack, _kpack, *vpack, _vpack;
    WT_CURSOR *hs_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(tmpkey);
    WT_DECL_RET;
    WT_IKEY *ikey;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_REC_KV *key, *val;
    WT_ROW *rip;
    WT_TIME_WINDOW *twp;
    WT_UPDATE *upd;
    WT_UPDATE_SELECT upd_select;
    size_t key_size;
    uint64_t slvg_skip;
    uint32_t i;
    uint8_t key_prefix;
    bool dictionary, hs_clear, key_onpage_ovfl, ovfl_key;
    void *copy;
    const void *key_data;

    btree = S2BT(session);
    page = pageref->page;
    twp = NULL;
    upd = NULL;
    slvg_skip = salvage == NULL ? 0 : salvage->skip;

    key = &r->k;
    val = &r->v;
    vpack = &_vpack;

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    /*
     * When removing a key due to a tombstone with a durable timestamp of "none", also remove the
     * history store contents associated with that key. It's safe to do even if we fail
     * reconciliation after the removal, the history store content must be obsolete in order for us
     * to consider removing the key.
     *
     * Ignore if this is metadata, as metadata doesn't have any history.
     *
     * Some code paths, such as schema removal, involve deleting keys in metadata and assert that
     * they shouldn't open new dhandles. In those cases we won't ever need to blow away history
     * store content, so we can skip this.
     */
    hs_cursor = NULL;
    hs_clear = F_ISSET(S2C(session), WT_CONN_HS_OPEN) &&
      !F_ISSET(session, WT_SESSION_NO_DATA_HANDLES) && !WT_IS_HS(btree->dhandle) &&
      !WT_IS_METADATA(btree->dhandle);

    WT_RET(__wt_rec_split_init(session, r, page, 0, btree->maxleafpage_precomp, 0));

    /*
     * Write any K/V pairs inserted into the page before the first from-disk key on the page.
     */
    if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT_SMALLEST(page))) != NULL)
        WT_RET(__rec_row_leaf_insert(session, r, ins));

    /*
     * Temporary buffers in which to instantiate any uninstantiated keys or value items we need.
     */
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

        /* Look for an update. */
        WT_ERR(__wt_rec_upd_select(session, r, NULL, rip, vpack, &upd_select));
        upd = upd_select.upd;

        /* Take the timestamp from the update or the cell. */
        if (upd == NULL)
            twp = &vpack->tw;
        else
            twp = &upd_select.tw;

        /*
         * If we reconcile an on disk key with a globally visible stop time point and there are no
         * new updates for that key, skip writing that key.
         */
        if (upd == NULL && __wt_txn_tw_stop_visible_all(session, twp))
            upd = &upd_tombstone;

        /* Build value cell. */
        if (upd == NULL) {
            /* Clear the on-disk cell time window if it is obsolete. */
            __wt_rec_time_window_clear_obsolete(session, NULL, vpack, r);

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
                WT_ERR(__rec_cell_repack(session, btree, r, vpack, twp));

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
                    WT_ERR(__rec_cell_repack(session, btree, r, vpack, twp));

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
                __wt_txn_tw_start_visible_all(session, twp));

            /* The first time we find an overflow record, discard the underlying blocks. */
            if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW) && vpack->raw != WT_CELL_VALUE_OVFL_RM)
                WT_ERR(__wt_ovfl_remove(session, page, vpack));

            switch (upd->type) {
            case WT_UPDATE_MODIFY:
                cbt->slot = WT_ROW_SLOT(page, rip);
                WT_ERR(__wt_modify_reconstruct_from_upd_list(session, cbt, upd, cbt->upd_value));
                __wt_value_return(cbt, cbt->upd_value);
                WT_ERR(__wt_rec_cell_build_val(
                  session, r, cbt->iface.value.data, cbt->iface.value.size, twp, 0));
                dictionary = true;
                break;
            case WT_UPDATE_STANDARD:
                /* Take the value from the update. */
                WT_ERR(__wt_rec_cell_build_val(session, r, upd->data, upd->size, twp, 0));
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
                     * Keys are part of the name-space, we can't remove them from the in-memory
                     * tree; if an overflow key was deleted without being instantiated (for example,
                     * cursor-based truncation), instantiate it now.
                     */
                    if (ikey == NULL)
                        WT_ERR(__wt_row_leaf_key(session, page, rip, tmpkey, true));

                    WT_ERR(__wt_ovfl_discard_add(session, page, kpack->cell));
                }

                /*
                 * When removing a key due to a tombstone with a durable timestamp of "none", also
                 * remove the history store contents associated with that key.
                 */
                if (twp->durable_stop_ts == WT_TS_NONE && hs_clear) {
                    WT_ERR(__wt_row_leaf_key(session, page, rip, tmpkey, true));

                    /* Open a history store cursor if we don't yet have one. */
                    if (hs_cursor == NULL)
                        WT_ERR(__wt_curhs_open(session, NULL, &hs_cursor));

                    /*
                     * From WT_TS_NONE delete all the history store content of the key. This path
                     * will never be taken for a mixed-mode deletion being evicted and with a
                     * checkpoint that started prior to the eviction starting its reconciliation as
                     * previous checks done while selecting an update will detect that.
                     */
                    WT_ERR(__wt_hs_delete_key_from_ts(
                      session, hs_cursor, btree->id, tmpkey, WT_TS_NONE, false, false));

                    /* Fail 0.01% of the time. */
                    if (F_ISSET(r, WT_REC_EVICT) &&
                      __wt_failpoint(
                        session, WT_TIMING_STRESS_FAILPOINT_HISTORY_STORE_DELETE_KEY_FROM_TS, 0.01))
                        WT_ERR(EBUSY);
                    WT_STAT_CONN_INCR(session, cache_hs_key_truncate_onpage_removal);
                    WT_STAT_DATA_INCR(session, cache_hs_key_truncate_onpage_removal);
                }

                /*
                 * We aren't creating a key so we can't use bytes from this key to provide prefix
                 * information for a subsequent key.
                 */
                tmpkey->size = 0;

                /* Proceed with appended key/value pairs. */
                goto leaf_insert;
            default:
                WT_ERR(__wt_illegal_value(session, upd->type));
            }
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

            /*
             * We aren't creating a key so we can't use this key as a prefix for a subsequent key.
             */
            tmpkey->size = 0;

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
            if (key_prefix == 0) {
                tmpkey->data = key_data;
                tmpkey->size = key_size;
                goto build;
            }

            if (tmpkey->size == 0 || tmpkey->size < key_prefix)
                goto slow;

            /*
             * Grow the buffer as necessary as well as ensure data has been copied into local buffer
             * space, then append the suffix to the prefix already in the buffer. Don't grow the
             * buffer unnecessarily or copy data we don't need, truncate the item's CURRENT data
             * length to the prefix bytes before growing the buffer.
             */
            tmpkey->size = key_prefix;
            WT_ERR(__wt_buf_grow(session, tmpkey, key_prefix + key_size));
            memcpy((uint8_t *)tmpkey->mem + key_prefix, key_data, key_size);
            tmpkey->size = key_prefix + key_size;

            if (0) {
slow:
                WT_ERR(__wt_row_leaf_key_copy(session, page, rip, tmpkey));
            }

build:
            WT_ERR(__rec_cell_build_leaf_key(session, r, tmpkey->data, tmpkey->size, &ovfl_key));
        }

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, key->len + val->len)) {
            /*
             * If we copied address blocks from the page rather than building the actual key, we
             * have to build the key now because we are about to promote it.
             */
            if (key_onpage_ovfl) {
                WT_ERR(__wt_dsk_cell_data_ref(session, WT_PAGE_ROW_LEAF, kpack, r->cur));
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

            WT_ERR(__wt_rec_split_crossing_bnd(session, r, key->len + val->len));
        }

        /* Copy the key/value pair onto the page. */
        __wt_rec_image_copy(session, r, key);
        if (val->len == 0 && __rec_row_zero_len(session, twp))
            r->any_empty_value = true;
        else {
            r->all_empty_value = false;
            if (dictionary && btree->dictionary)
                WT_ERR(__wt_rec_dict_replace(session, r, twp, 0, val));
            __wt_rec_image_copy(session, r, val);
        }
        WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, twp);

        /* Update compression state. */
        __rec_key_state_update(r, ovfl_key);

leaf_insert:
        /* Write any K/V pairs inserted into the page after this key. */
        if ((ins = WT_SKIP_FIRST(WT_ROW_INSERT(page, rip))) != NULL)
            WT_ERR(__rec_row_leaf_insert(session, r, ins));
    }

    /* Write the remnant page. */
    ret = __wt_rec_split_finish(session, r);

err:
    if (hs_cursor != NULL)
        WT_TRET(hs_cursor->close(hs_cursor));
    __wt_scr_free(session, &tmpkey);
    return (ret);
}
