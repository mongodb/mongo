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
 * Per-iteration state built while processing each entry in the variable-length column-store
 * reconciliation loops.
 */
typedef struct {
    const void *data;
    uint32_t size;
    WT_TIME_WINDOW tw;
    uint64_t repeat_count;
    bool deleted;
    bool dictionary;
    bool update_no_copy;
} WTI_COL_VAR_CUR;

/*
 * State carried across all loop iterations in variable-length column-store reconciliation for
 * run-length accounting.
 */
typedef struct {
    WT_ITEM *last_value;
    WT_TIME_WINDOW last_tw;
    bool last_deleted;
    bool last_dictionary;
    uint64_t rle;
    uint64_t src_recno;
    bool wrote_real_values;
} WTI_COL_VAR_STATE;

/*
 * __wt_bulk_insert_var --
 *     Variable-length column-store bulk insert.
 */
int
__wt_bulk_insert_var(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
{
    WT_BTREE *btree;
    WTI_RECONCILE *r;
    WTI_REC_KV *val;
    WT_TIME_WINDOW tw;

    r = cbulk->reconcile;
    btree = S2BT(session);
    WT_TIME_WINDOW_INIT(&tw);

    val = &r->v;
    if (deleted) {
        val->cell_len = __wt_cell_pack_del(session, &val->cell, &tw, cbulk->rle);
        val->buf.data = NULL;
        val->buf.size = 0;
        val->len = val->cell_len;
    } else
        /*
         * Store the bulk cursor's last buffer, not the current value, we're tracking duplicates,
         * which means we want the previous value seen, not the current value.
         */
        WT_RET(__wti_rec_cell_build_val(
          session, r, cbulk->last->data, cbulk->last->size, &tw, cbulk->rle, NULL));

    /* Boundary: split or write the page. */
    if (WTI_CROSSING_SPLIT_BND(r, val->len))
        WT_RET(__wti_rec_split_crossing_bnd(session, r, val->len));

    /* Copy the value onto the page. */
    if (btree->dictionary)
        WT_RET(__wti_rec_dict_replace(session, r, &tw, cbulk->rle, val));
    __wti_rec_image_copy(session, r, val);

    /* Initialize the time aggregate that's going into the parent page. See note above. */
    WTI_REC_CHUNK_TA_UPDATE(session, r->cur_ptr, &tw);

    /* Update the starting record number in case we split. */
    r->recno += cbulk->rle;

    return (0);
}

/*
 * __rec_col_merge --
 *     Merge in a split page.
 */
static int
__rec_col_merge(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_PAGE *page)
{
    WT_ADDR *addr;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WTI_REC_KV *val;
    uint32_t i;

    mod = page->modify;

    val = &r->v;

    /* For each entry in the split array... */
    for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
        /* Update the starting record number in case we split. */
        r->recno = multi->key.recno;

        /* Build the value cell. */
        addr = &multi->addr;
        __wti_rec_cell_build_addr(session, r, addr, NULL, r->recno, NULL, false);

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, val->len))
            WT_RET(__wti_rec_split_crossing_bnd(session, r, val->len));

        /* Copy the value onto the page. */
        __wti_rec_image_copy(session, r, val);
        WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &addr->ta);
    }
    return (0);
}

/*
 * __wti_rec_col_int --
 *     Reconcile a column-store internal page.
 */
int
__wti_rec_col_int(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref)
{
    WT_ADDR *addr;
    WT_BTREE *btree;
    WT_CELL_UNPACK_ADDR *vpack, _vpack;
    WTI_CHILD_MODIFY_STATE cms;
    WT_DECL_RET;
    WT_PAGE *child, *page;
    WT_PAGE_DELETED *page_del;
    WTI_REC_KV *val;
    WT_REF *ref;
    WT_TIME_AGGREGATE ft_ta, ta;

    btree = S2BT(session);
    page = pageref->page;
    child = NULL;
    WT_TIME_AGGREGATE_INIT(&ta);
    WT_TIME_AGGREGATE_INIT_MERGE(&ft_ta);

    val = &r->v;
    vpack = &_vpack;

    WT_RET(__wti_rec_split_init(session, r, pageref->ref_recno, btree->maxintlpage_precomp));

    /* For each entry in the in-memory page... */
    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        __wt_atomic_cas_uint8_v(&ref->dirty_state, WT_REF_DIRTY, WT_REF_CLEAN);

        /* Update the starting record number in case we split. */
        r->recno = ref->ref_recno;

        /*
         * Modified child. The page may be emptied or internally created during a split.
         * Deleted/split pages are merged into the parent and discarded.
         */
        WT_ERR(__wti_rec_child_modify(session, r, ref, &cms, NULL));
        addr = NULL;
        child = ref->page;
        page_del = NULL;

        switch (cms.state) {
        case WTI_CHILD_IGNORE:
            /* Ignored child. */
            WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
            continue;

        case WTI_CHILD_MODIFIED:
            /*
             * Modified child. Empty pages are merged into the parent and discarded.
             */
            switch (child->modify->rec_result) {
            case WT_PM_REC_EMPTY:
                WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
                continue;
            case WT_PM_REC_MULTIBLOCK:
                WT_ERR(__rec_col_merge(session, r, child));
                WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
                continue;
            case WT_PM_REC_REPLACE:
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
            /* Deleted child where we write a proxy cell. */
            page_del = &cms.del;
            break;
        }

        /*
         * Build the value cell. The child page address is in one of 3 places: if the page was
         * replaced, the page's modify structure references it and we assigned it just above in the
         * switch statement. Otherwise, the WT_REF->addr reference points to either an on-page cell
         * or an off-page WT_ADDR structure: if it's an on-page cell we copy it from the page, else
         * build a new cell.
         */
        if (addr == NULL && __wt_off_page(page, ref->addr))
            addr = ref->addr;
        if (addr != NULL) {
            /* FIXME-WT-17663: pass the correct is_prepared_fast_truncate from the caller. */
            __wti_rec_cell_build_addr(session, r, addr, NULL, ref->ref_recno, page_del, false);
            WT_TIME_AGGREGATE_COPY(&ta, &addr->ta);
        } else {
            __wt_cell_unpack_addr(session, page->dsk, ref->addr, vpack);
            if (cms.state == WTI_CHILD_PROXY ||
              F_ISSET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED)) {
                /*
                 * Need to build a proxy (page-deleted) cell or rebuild the cell with updated time
                 * info.
                 */
                WT_ASSERT(session, vpack->type != WT_CELL_ADDR_DEL || page_del != NULL);
                /* FIXME-WT-17663: pass the correct is_prepared_fast_truncate from the caller. */
                __wti_rec_cell_build_addr(session, r, NULL, vpack, ref->ref_recno, page_del, false);
            } else {
                /* Copy the entire existing cell, including any page-delete information. */
                val->buf.data = ref->addr;
                val->buf.size = __wt_cell_total_len(vpack);
                val->cell_len = 0;
                val->len = val->buf.size;
            }
            WT_TIME_AGGREGATE_COPY(&ta, &vpack->ta);
        }
        if (page_del != NULL)
            WT_TIME_AGGREGATE_UPDATE_PAGE_DEL(session, &ft_ta, page_del);

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, val->len))
            WT_ERR(__wti_rec_split_crossing_bnd(session, r, val->len));

        /*
         * Copy the value onto the page. val->buf.data may point directly into ref's WT_ADDR
         * block_cookie; hold the hazard pointer until after the copy.
         */
        __wti_rec_image_copy(session, r, val);
        WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);
        if (page_del != NULL)
            WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &ft_ta);
        WTI_REC_CHUNK_TA_MERGE(session, r->cur_ptr, &ta);
    }
    WT_INTL_FOREACH_END;

    /* Write the remnant page. */
    return (__wti_rec_split_finish(session, r));

err:
    WTI_CHILD_RELEASE(session, cms.hazard, ref);
    return (ret);
}

/*
 * __rec_col_var_helper --
 *     Create a column-store variable length record cell and write it onto a page.
 */
static int
__rec_col_var_helper(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_SALVAGE_COOKIE *salvage,
  WT_ITEM *value, WT_TIME_WINDOW *tw, uint64_t rle, bool deleted, bool dictionary, bool *ovfl_usedp)
{
    WTI_REC_KV *val;

    if (ovfl_usedp != NULL)
        *ovfl_usedp = false;

    val = &r->v;

    /*
     * Occasionally, salvage needs to discard records from the beginning or end of the page, and
     * because the items may be part of a RLE cell, do the adjustments here. It's not a mistake we
     * don't bother telling our caller we've handled all the records from the page we care about,
     * and can quit processing the page: salvage is a rare operation and I don't want to complicate
     * our caller's loop.
     */
    if (salvage != NULL) {
        if (salvage->done)
            return (0);
        if (salvage->skip != 0) {
            if (rle <= salvage->skip) {
                salvage->skip -= rle;
                return (0);
            }
            rle -= salvage->skip;
            salvage->skip = 0;
        }
        if (salvage->take != 0) {
            if (rle <= salvage->take)
                salvage->take -= rle;
            else {
                rle = salvage->take;
                salvage->take = 0;
            }
            if (salvage->take == 0)
                salvage->done = true;
        }
    }

    if (deleted) {
        val->cell_len = __wt_cell_pack_del(session, &val->cell, tw, rle);
        val->buf.data = NULL;
        val->buf.size = 0;
        val->len = val->cell_len;
    } else if (ovfl_usedp != NULL) {
        val->cell_len =
          __wt_cell_pack_ovfl(session, &val->cell, WT_CELL_VALUE_OVFL, tw, rle, value->size);
        val->buf.data = value->data;
        val->buf.size = value->size;
        val->len = val->cell_len + value->size;
        *ovfl_usedp = true;
    } else
        WT_RET(__wti_rec_cell_build_val(session, r, value->data, value->size, tw, rle, NULL));

    /* Boundary: split or write the page. */
    if (__wti_rec_need_split(r, val->len))
        WT_RET(__wti_rec_split_crossing_bnd(session, r, val->len));

    /* Copy the value onto the page. Use the dictionary whenever requested. */
    if (dictionary && !deleted && ovfl_usedp == NULL)
        WT_RET(__wti_rec_dict_replace(session, r, tw, rle, val));
    __wti_rec_image_copy(session, r, val);
    WTI_REC_CHUNK_TA_UPDATE(session, r->cur_ptr, tw);

    /* Update the starting record number in case we split. */
    r->recno += rle;

    return (0);
}

/*
 * __rec_col_var_upd_apply --
 *     Apply a selected update to produce the current-record state. The caller sets cbt->slot to the
 *     appropriate column slot (or UINT32_MAX for append-list entries) before calling.
 */
static int
__rec_col_var_upd_apply(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_CURSOR_BTREE *cbt,
  WTI_UPDATE_SELECT *upd_select, uint64_t src_recno, WTI_COL_VAR_CUR *cur)
{
    WT_UPDATE *upd;

    upd = upd_select->upd;
    WT_TIME_WINDOW_COPY(&cur->tw, &upd_select->tw);

    switch (upd->type) {
    case WT_UPDATE_MODIFY:
        WT_RET(__wt_modify_reconstruct_from_upd_list(
          session, cbt, upd, cbt->upd_value, WT_OPCTX_RECONCILATION));
        __wt_value_return(cbt, cbt->upd_value);
        cur->data = cbt->iface.value.data;
        cur->size = (uint32_t)cbt->iface.value.size;
        cur->update_no_copy = false;
        cur->dictionary = S2BT(session)->dictionary;
        break;
    case WT_UPDATE_STANDARD:
        cur->data = upd->data;
        cur->size = upd->size;
        cur->dictionary = S2BT(session)->dictionary;
        break;
    case WT_UPDATE_TOMBSTONE:
        cur->deleted = true;
        WT_TIME_WINDOW_INIT(&cur->tw);
        break;
    default:
        WT_RET(__wt_illegal_value(session, upd->type));
    }

    if (upd_select->no_ts_tombstone && r->hs_clear_on_tombstone)
        WT_RET(__wti_rec_hs_clear_on_tombstone(
          session, r, src_recno, NULL, upd->type == WT_UPDATE_TOMBSTONE ? false : true));

    return (0);
}

/*
 * __rec_col_var_rle_extend --
 *     Attempt to extend the current run by comparing against the accumulated state. Sets *extendedp
 *     on a match; otherwise emits the pending run (if any) and prepares for a new one.
 */
static int
__rec_col_var_rle_extend(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_SALVAGE_COOKIE *salvage,
  WTI_COL_VAR_STATE *st, WTI_COL_VAR_CUR *cur, bool *extendedp)
{
    *extendedp = false;

    if (st->rle != 0) {
        if (WT_TIME_WINDOWS_EQUAL(&st->last_tw, &cur->tw) &&
          ((cur->deleted && st->last_deleted) ||
            (!cur->deleted && !st->last_deleted && st->last_value->size == cur->size &&
              (cur->size == 0 || memcmp(st->last_value->data, cur->data, cur->size) == 0)))) {

            /* The time window for a pair of deleted keys must be empty. */
            WT_ASSERT(session,
              (!cur->deleted && !st->last_deleted) || WT_TIME_WINDOW_IS_EMPTY(&st->last_tw));

            st->rle += cur->repeat_count;
            *extendedp = true;
            return (0);
        }
        if (!st->last_deleted)
            st->wrote_real_values = true;
        WT_RET(__rec_col_var_helper(session, r, salvage, st->last_value, &st->last_tw, st->rle,
          st->last_deleted, st->last_dictionary, NULL));
    }
    return (0);
}

/*
 * __rec_col_var_page_loop --
 *     Walk the on-page variable-length column-store entries, building per-record state and
 *     accumulating RLE runs in *st.
 */
static int
__rec_col_var_page_loop(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_PAGE *page,
  WT_SALVAGE_COOKIE *salvage, WTI_COL_VAR_STATE *st)
{
    enum { OVFL_IGNORE, OVFL_UNUSED, OVFL_USED } ovfl_state;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *vpack, _vpack;
    WT_COL *cip;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(orig);
    WT_DECL_RET;
    WT_INSERT *ins;
    WTI_UPDATE_SELECT upd_select;
    WTI_COL_VAR_CUR cur;
    WT_UPDATE *upd;
    uint64_t n, nrepeat;
    uint32_t i;
    bool extended, orig_deleted, orig_stale, ovfl_used;

    orig_stale = false;

    btree = S2BT(session);
    conn = S2C(session);
    vpack = &_vpack;
    cbt = &r->update_modify_cbt;

    WT_RET(__wt_scr_alloc(session, 0, &orig));

    WT_COL_FOREACH (page, cip, i) {
        ovfl_state = OVFL_IGNORE;
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(session, page->dsk, cell, vpack);
        nrepeat = __wt_cell_rle(vpack);
        ins = WT_SKIP_FIRST(WT_COL_UPDATE(page, cip));

        orig_deleted = vpack->type == WT_CELL_DEL;
        if (orig_deleted)
            goto record_loop;

        orig_stale = __wt_txn_tw_stop_visible_all(session, &vpack->tw);

        if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)) {
            ovfl_state = OVFL_UNUSED;
            goto record_loop;
        }

record_loop:
        for (n = 0; n < nrepeat; n += cur.repeat_count, st->src_recno += cur.repeat_count) {
            upd = NULL;
            if (ins != NULL && WT_INSERT_RECNO(ins) == st->src_recno) {
                WT_ERR(__wti_rec_upd_select(session, r, ins, NULL, vpack, &upd_select));
                upd = upd_select.upd;
                ins = WT_SKIP_NEXT(ins);
            } else
                upd_select.skip_aborted_prepared_value = false;

            WT_CLEAR(cur);
            cur.update_no_copy = true;
            cur.repeat_count = 1;

            if (upd == NULL && orig_stale &&
              (!F_ISSET(conn, WT_CONN_PRESERVE_PREPARED) || !F_ISSET(r, WT_REC_EVICT) ||
                !upd_select.skip_aborted_prepared_value)) {
                /*
                 * The on-disk value is stale and no update exists. Drop it unless the chain holds
                 * an unstable aborted prepared update that needs this cell as its rollback target.
                 */
                cur.deleted = true;
                WT_TIME_WINDOW_INIT(&cur.tw);
                r->key_removed_from_disk_image = true;
            } else if (upd == NULL) {
                cur.update_no_copy = false;

                /*
                 * The repeat count is the number of records up to the next WT_INSERT record, or up
                 * to the end of the entry if we have no more WT_INSERT records.
                 */
                if (ins == NULL)
                    cur.repeat_count = nrepeat - n;
                else
                    cur.repeat_count = WT_INSERT_RECNO(ins) - st->src_recno;

                cur.deleted = orig_deleted;
                if (cur.deleted) {
                    WT_TIME_WINDOW_INIT(&cur.tw);
                    r->key_removed_from_disk_image = true;
                    goto compare;
                }

                /*
                 * If preserve prepared update is enabled, we must select an update to replace the
                 * onpage prepared update. Otherwise, we leak the prepared update.
                 */
                WT_ASSERT_ALWAYS(session,
                  !F_ISSET(conn, WT_CONN_PRESERVE_PREPARED) ||
                    !WT_TIME_WINDOW_HAS_PREPARE(&vpack->tw),
                  "leaked prepared update.");

                /* Clear the on-disk cell time window if it is obsolete. */
                __wti_rec_time_window_clear_obsolete(session, NULL, vpack, r);
                WT_TIME_WINDOW_COPY(&cur.tw, &vpack->tw);

                /*
                 * Check if we are dealing with a dictionary cell (a copy of another item on the
                 * page).
                 */
                if (vpack->raw == WT_CELL_VALUE_COPY)
                    cur.dictionary = btree->dictionary;

                /*
                 * If we are handling overflow items, use the overflow item itself exactly once,
                 * after which we have to copy it into a buffer and from then on use a complete copy
                 * because we are re-creating a new overflow record each time.
                 */
                switch (ovfl_state) {
                case OVFL_UNUSED:
                    /*
                     * An as-yet-unused overflow item: emit the pending run, write this cell
                     * directly once, then skip the normal compare/swap path.
                     */
                    if (st->rle != 0) {
                        WT_ERR(__rec_col_var_helper(session, r, salvage, st->last_value,
                          &st->last_tw, st->rle, st->last_deleted, st->last_dictionary, NULL));
                        st->rle = 0;
                    }
                    st->last_value->data = vpack->data;
                    st->last_value->size = vpack->size;
                    WT_ERR(__rec_col_var_helper(session, r, salvage, st->last_value, &cur.tw,
                      cur.repeat_count, false, st->last_dictionary, &ovfl_used));
                    st->wrote_real_values = true;
                    /*
                     * Salvage may have caused us to skip the overflow item, only update overflow
                     * items we use.
                     */
                    if (ovfl_used) {
                        r->ovfl_items = true; /* Track if page has overflow items. */
                        ovfl_state = OVFL_USED;
                    }
                    continue;
                case OVFL_USED:
                    /*
                     * Overflow already emitted once; read the value into a scratch buffer so
                     * subsequent copies work.
                     */
                    WT_ERR(__wt_dsk_cell_data_ref_kv(session, vpack, orig));
                    cur.data = orig->data;
                    cur.size = (uint32_t)orig->size;
                    ovfl_state = OVFL_IGNORE;
                    break;
                case OVFL_IGNORE:
                    /*
                     * Use the copied original value if the on-page value is an overflow value.
                     * Otherwise, use the on-page value.
                     */
                    if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)) {
                        cur.data = orig->data;
                        cur.size = (uint32_t)orig->size;
                    } else {
                        cur.data = vpack->data;
                        cur.size = vpack->size;
                    }
                    break;
                }
            } else {
                cbt->slot = WT_COL_SLOT(page, cip);
                WT_ERR(__rec_col_var_upd_apply(session, r, cbt, &upd_select, st->src_recno, &cur));
                if (cur.deleted)
                    r->key_removed_from_disk_image = true;
            }

compare:
            /*
             * If we have a record against which to compare and the records compare equal, increment
             * the RLE and continue. If the records don't compare equal, output the last record and
             * swap the last and current buffers: do NOT update the starting record number, we've
             * been doing that all along.
             */
            WT_ERR(__rec_col_var_rle_extend(session, r, salvage, st, &cur, &extended));
            if (extended)
                continue;

            /*
             * Swap the current/last state.
             *
             * Reset RLE counter and turn on comparisons.
             */
            if (!cur.deleted) {
                /*
                 * We can't simply assign the data values into the last buffer because they may have
                 * come from a copy built from an encoded/overflow cell and creating the next record
                 * is going to overwrite that memory. Check, because encoded/overflow cells aren't
                 * that common and we'd like to avoid the copy. If data was taken from the current
                 * unpack structure (which points into the page), or was taken from an update
                 * structure, we can just use the pointers, they're not moving.
                 */
                if (cur.data == vpack->data || cur.update_no_copy) {
                    st->last_value->data = cur.data;
                    st->last_value->size = cur.size;
                } else
                    WT_ERR(__wt_buf_set(session, st->last_value, cur.data, cur.size));
            }
            WT_TIME_WINDOW_COPY(&st->last_tw, &cur.tw);
            st->last_deleted = cur.deleted;
            st->last_dictionary = cur.dictionary;
            st->rle = cur.repeat_count;
        }

        /*
         * The first time we find an overflow record we never used, discard the underlying blocks,
         * they're no longer useful.
         */
        if (ovfl_state == OVFL_UNUSED && vpack->raw != WT_CELL_VALUE_OVFL_RM)
            WT_ERR(__wt_ovfl_remove(session, page, vpack));
    }

err:
    __wt_scr_free(session, &orig);
    return (ret);
}

/*
 * __rec_col_var_append_loop --
 *     Walk the column-store append list, building per-record state and accumulating RLE runs in
 *     *st.
 */
static int
__rec_col_var_append_loop(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_PAGE *page,
  WT_SALVAGE_COOKIE *salvage, WTI_COL_VAR_STATE *st)
{
    WT_CURSOR_BTREE *cbt;
    WT_INSERT *ins;
    WTI_UPDATE_SELECT upd_select;
    WTI_COL_VAR_CUR cur;
    WT_UPDATE *upd;
    uint64_t n, skip;
    bool extended;

    cbt = &r->update_modify_cbt;

    for (ins = WT_SKIP_FIRST(WT_COL_APPEND(page));; ins = WT_SKIP_NEXT(ins)) {
        if (ins == NULL)
            /*
             * Stop when we reach the end of the append list. There might be a gap between that and
             * the beginning of the next page. (Imagine record 98 is written, then record 100 is
             * written, then the page splits and record 100 moves to another page. There is no entry
             * for record 99 and we don't write one out.) In VLCS we (now) tolerate such gaps as
             * they are, though likely smaller, equivalent to gaps created by fast-truncate.
             */
            break;
        WT_RET(__wti_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
        upd = upd_select.upd;
        n = WT_INSERT_RECNO(ins);

        while (st->src_recno <= n) {
            WT_CLEAR(cur);
            cur.update_no_copy = true;
            cur.repeat_count = 1;

            if (st->src_recno < n) {
                cur.deleted = true;
                WT_TIME_WINDOW_INIT(&cur.tw);
                if (st->last_deleted) {
                    /* The time window for deleted keys must be empty. */
                    WT_ASSERT(session, WT_TIME_WINDOW_IS_EMPTY(&st->last_tw));
                    /*
                     * The record adjustment is decremented by one so we can naturally fall into the
                     * RLE accounting below, where we increment rle by one, then continue in the
                     * outer loop, where we increment src_recno by one.
                     */
                    skip = (n - st->src_recno) - 1;
                    st->rle += skip;
                    st->src_recno += skip;
                }
            } else if (upd == NULL) {
                /* The updates on the key are all uncommitted so we write a deleted key to disk. */
                cur.deleted = true;
                WT_TIME_WINDOW_INIT(&cur.tw);
            } else {
                cbt->slot = UINT32_MAX;
                WT_RET(__rec_col_var_upd_apply(session, r, cbt, &upd_select, st->src_recno, &cur));
            }

            /*
             * Handle RLE accounting and comparisons -- see comment above, this code fragment does
             * the same thing.
             */
            WT_RET(__rec_col_var_rle_extend(session, r, salvage, st, &cur, &extended));
            if (extended)
                goto next;

            /*
             * Swap the current/last state. We can't simply assign the data values into the last
             * buffer because they may be a temporary copy built from a chain of modified updates
             * and creating the next record will overwrite that memory. Check, we'd like to avoid
             * the copy. If data was taken from an update structure, we can just use the pointers,
             * they're not moving.
             */
            if (!cur.deleted) {
                if (cur.update_no_copy) {
                    st->last_value->data = cur.data;
                    st->last_value->size = cur.size;
                } else
                    WT_RET(__wt_buf_set(session, st->last_value, cur.data, cur.size));
            }

            /* Ready for the next loop, reset the RLE counter. */
            WT_TIME_WINDOW_COPY(&st->last_tw, &cur.tw);
            st->last_deleted = cur.deleted;
            st->last_dictionary = cur.dictionary;
            st->rle = 1;

            /*
             * Move to the next record. It's not a simple increment because if it's the maximum
             * record, incrementing it wraps to 0 and this turns into an infinite loop.
             */
next:
            if (st->src_recno == UINT64_MAX)
                break;
            ++st->src_recno;
        }

        /*
         * Execute this loop once without an insert item to catch any missing records due to a
         * split, then quit.
         */
        if (ins == NULL)
            break;
    }

    return (0);
}

/*
 * __wti_rec_col_var --
 *     Reconcile a variable-width column-store leaf page.
 */
int
__wti_rec_col_var(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    WT_BTREE *btree;
    WTI_COL_VAR_STATE st;
    WT_PAGE *page;
    WT_TIME_WINDOW clear_tw;

    btree = S2BT(session);
    page = pageref->page;
    WT_TIME_WINDOW_INIT(&clear_tw);

    r->update_modify_cbt.iface.session = (WT_SESSION *)session;

    /* Set the "last" values to cause failure if they're not set before use. */
    WT_CLEAR(st);
    st.last_value = r->last;
    WT_TIME_WINDOW_INIT(&st.last_tw);

    WT_RET(__wti_rec_split_init(session, r, pageref->ref_recno, btree->maxleafpage_precomp));

    /*
     * The salvage code may be calling us to reconcile a page where there were missing records in
     * the column-store name space. If taking the first record from on the page, it might be a
     * deleted record, so we have to give the RLE code a chance to figure that out. Else, if not
     * taking the first record from the page, write a single element representing the missing
     * records onto a new page. (Don't pass the salvage cookie to our helper function in this case,
     * we're handling one of the salvage cookie fields on our own, and we don't need the helper
     * function's assistance.)
     */
    if (salvage != NULL && salvage->missing != 0) {
        if (salvage->skip == 0) {
            st.rle = salvage->missing;
            st.last_deleted = true;

            /*
             * Correct the number of records we're going to "take", pretending the missing records
             * were on the page.
             */
            salvage->take += salvage->missing;
        } else
            WT_RET(__rec_col_var_helper(
              session, r, NULL, NULL, &clear_tw, salvage->missing, true, false, NULL));
    }

    /*
     * We track two data items through this loop: the previous (last) item and the current item: if
     * the last item is the same as the current item, we increment the RLE count for the last item;
     * if the last item is different from the current item, we write the last item onto the page,
     * and replace it with the current item. The r->recno counter tracks records written to the
     * page, and is incremented by the helper function immediately after writing records to the
     * page. The record number of our source record, that is, the current item, is maintained in
     * src_recno.
     */
    st.src_recno = r->recno + st.rle;

    WT_RET(__rec_col_var_page_loop(session, r, page, salvage, &st));
    WT_RET(__rec_col_var_append_loop(session, r, page, salvage, &st));

    /* If we were tracking a record, write it. */
    if (st.rle != 0) {
        if (!st.last_deleted)
            st.wrote_real_values = true;
        WT_RET(__rec_col_var_helper(session, r, salvage, st.last_value, &st.last_tw, st.rle,
          st.last_deleted, st.last_dictionary, NULL));
    }

    /*
     * If we have not generated any real values but only deleted-value cells, bail out and call the
     * page empty instead. (Note that this should always be exactly one deleted-value cell, because
     * of the RLE handling, so we can't have split.) This will create a namespace gap like those
     * produced by truncate.
     *
     * Skip this step if:
     *     - We're in salvage, to avoid any odd interactions with the way salvage reconstructs the
     *       namespace.
     *     - There were invisible updates, because then the page isn't really empty. Also, at least
     *       for now if we try to restore updates to an empty page col_modify will trip on its
     *       shoelaces.
     *     - We wrote no cells at all. This can happen if a page with no cells and no append list
     *       entries at all (not just one with no or only aborted updates) gets marked dirty somehow
     *       and reconciled; this is apparently possible in some circumstances.
     */
    if (!st.wrote_real_values && salvage == NULL && r->leave_dirty == false && r->entries > 0) {
        WT_ASSERT(session, r->entries == 1);
        r->entries = 0;
        WT_STAT_CONN_DSRC_INCR(session, rec_vlcs_emptied_pages);
        /* Don't bother adjusting r->space_avail or r->first_free. */
    }

    /* Write the remnant page. */
    return (__wti_rec_split_finish(session, r));
}
