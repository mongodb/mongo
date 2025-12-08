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
        __wti_rec_cell_build_addr(session, r, addr, NULL, r->recno, NULL);

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
        __wt_atomic_cas_uint8_v(&ref->rec_state, WT_REF_REC_DIRTY, WT_REF_REC_CLEAN);

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
            __wti_rec_cell_build_addr(session, r, addr, NULL, ref->ref_recno, page_del);
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
                __wti_rec_cell_build_addr(session, r, NULL, vpack, ref->ref_recno, page_del);
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
        WTI_CHILD_RELEASE_ERR(session, cms.hazard, ref);

        /* Boundary: split or write the page. */
        if (__wti_rec_need_split(r, val->len))
            WT_ERR(__wti_rec_split_crossing_bnd(session, r, val->len));

        /* Copy the value (which is in val, val == r->v) onto the page. */
        __wti_rec_image_copy(session, r, val);
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
 * __wti_rec_col_var --
 *     Reconcile a variable-width column-store leaf page.
 */
int
__wti_rec_col_var(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    enum { OVFL_IGNORE, OVFL_UNUSED, OVFL_USED } ovfl_state;
    struct {
        WT_ITEM *value; /* Value */
        WT_TIME_WINDOW tw;
        bool deleted;    /* If deleted */
        bool dictionary; /* If dictionary is in use */
    } last;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *vpack, _vpack;
    WT_COL *cip;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(orig);
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_TIME_WINDOW clear_tw, *twp;
    WT_UPDATE *upd;
    WTI_UPDATE_SELECT upd_select;
    uint64_t n, nrepeat, repeat_count, rle, skip, src_recno;
    uint32_t i, size;
    bool deleted, dictionary, orig_deleted, orig_stale, ovfl_used, update_no_copy,
      wrote_real_values;
    const void *data;

    btree = S2BT(session);
    conn = S2C(session);
    vpack = &_vpack;
    page = pageref->page;
    WT_TIME_WINDOW_INIT(&clear_tw);
    twp = NULL;
    upd = NULL;
    size = 0;
    orig_stale = false;
    wrote_real_values = false;
    dictionary = false;
    data = NULL;

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    /* Set the "last" values to cause failure if they're not set. */
    last.value = r->last;
    WT_TIME_WINDOW_INIT(&last.tw);
    last.deleted = false;
    last.dictionary = false;

    WT_RET(__wti_rec_split_init(session, r, pageref->ref_recno, btree->maxleafpage_precomp));

    WT_RET(__wt_scr_alloc(session, 0, &orig));

    /*
     * The salvage code may be calling us to reconcile a page where there were missing records in
     * the column-store name space. If taking the first record from on the page, it might be a
     * deleted record, so we have to give the RLE code a chance to figure that out. Else, if not
     * taking the first record from the page, write a single element representing the missing
     * records onto a new page. (Don't pass the salvage cookie to our helper function in this case,
     * we're handling one of the salvage cookie fields on our own, and we don't need the helper
     * function's assistance.)
     */
    rle = 0;
    if (salvage != NULL && salvage->missing != 0) {
        if (salvage->skip == 0) {
            rle = salvage->missing;
            last.deleted = true;
            last.dictionary = false;

            /*
             * Correct the number of records we're going to "take", pretending the missing records
             * were on the page.
             */
            salvage->take += salvage->missing;
        } else
            WT_ERR(__rec_col_var_helper(
              session, r, NULL, NULL, &clear_tw, salvage->missing, true, last.dictionary, NULL));
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
    src_recno = r->recno + rle;

    /* For each entry in the in-memory page... */
    WT_COL_FOREACH (page, cip, i) {
        ovfl_state = OVFL_IGNORE;
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(session, page->dsk, cell, vpack);
        nrepeat = __wt_cell_rle(vpack);
        ins = WT_SKIP_FIRST(WT_COL_UPDATE(page, cip));
        dictionary = false;

        /*
         * If the original value is "deleted", there's no value to compare, we're done.
         */
        orig_deleted = vpack->type == WT_CELL_DEL;
        if (orig_deleted)
            goto record_loop;

        /* If the original value is stale, delete it when no updates are found. */
        orig_stale = __wt_txn_tw_stop_visible_all(session, &vpack->tw);

        /*
         * Overflow items are tricky: we don't know until we're finished processing the set of
         * values if we need the overflow value or not. If we don't use the overflow item at all, we
         * have to discard it from the backing file, otherwise we'll leak blocks on the checkpoint.
         * That's safe because if the backing overflow value is still needed by any running
         * transaction, we'll cache a copy in the update list.
         *
         * Regardless, we avoid copying in overflow records: if there's a WT_INSERT entry that
         * modifies a reference counted overflow record, we may have to write copies of the overflow
         * record, and in that case we'll do the comparisons, but we don't read overflow items just
         * to see if they match records on either side.
         */
        if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)) {
            ovfl_state = OVFL_UNUSED;
            goto record_loop;
        }

record_loop:
        /*
         * Generate on-page entries: loop repeat records, looking for WT_INSERT entries matching the
         * record number. The WT_INSERT lists are in sorted order, so only need check the next one.
         */
        for (n = 0; n < nrepeat; n += repeat_count, src_recno += repeat_count) {
            upd = NULL;
            if (ins != NULL && WT_INSERT_RECNO(ins) == src_recno) {
                WT_ERR(__wti_rec_upd_select(session, r, ins, NULL, vpack, &upd_select));
                upd = upd_select.upd;
                ins = WT_SKIP_NEXT(ins);
            }

            update_no_copy = true; /* No data copy */
            repeat_count = 1;      /* Single record */
            deleted = false;

            if (upd == NULL && orig_stale) {
                /* The on-disk value is stale and there was no update. Treat it as deleted. */
                deleted = true;
                r->key_removed_from_disk_image = true;
                twp = &clear_tw;
            } else if (upd == NULL) {
                update_no_copy = false; /* Maybe data copy */

                /*
                 * The repeat count is the number of records up to the next WT_INSERT record, or up
                 * to the end of the entry if we have no more WT_INSERT records.
                 */
                if (ins == NULL)
                    repeat_count = nrepeat - n;
                else
                    repeat_count = WT_INSERT_RECNO(ins) - src_recno;

                /*
                 * The key on the old disk image is unchanged. Clear the time window information if
                 * it's a deleted record, else take the time window from the cell.
                 */
                deleted = orig_deleted;
                if (deleted) {
                    twp = &clear_tw;
                    r->key_removed_from_disk_image = true;
                    goto compare;
                }
                twp = &vpack->tw;
                /*
                 * If preserve prepared update is enabled, we must select an update to replace the
                 * onpage prepared update. Otherwise, we leak the prepared update.
                 */
                WT_ASSERT_ALWAYS(session,
                  !F_ISSET(conn, WT_CONN_PRESERVE_PREPARED) || !WT_TIME_WINDOW_HAS_PREPARE(twp),
                  "leaked prepared update.");

                /* Clear the on-disk cell time window if it is obsolete. */
                __wti_rec_time_window_clear_obsolete(session, NULL, vpack, r);

                /*
                 * Check if we are dealing with a dictionary cell (a copy of another item on the
                 * page).
                 */
                if (vpack->raw == WT_CELL_VALUE_COPY)
                    dictionary = btree->dictionary;

                /*
                 * If we are handling overflow items, use the overflow item itself exactly once,
                 * after which we have to copy it into a buffer and from then on use a complete copy
                 * because we are re-creating a new overflow record each time.
                 */
                switch (ovfl_state) {
                case OVFL_UNUSED:
                    /*
                     * An as-yet-unused overflow item.
                     *
                     * We're going to copy the on-page cell, write out any record we're tracking.
                     */
                    if (rle != 0) {
                        WT_ERR(__rec_col_var_helper(session, r, salvage, last.value, &last.tw, rle,
                          last.deleted, last.dictionary, NULL));
                        rle = 0;
                    }

                    last.value->data = vpack->data;
                    last.value->size = vpack->size;
                    WT_ERR(__rec_col_var_helper(session, r, salvage, last.value, twp, repeat_count,
                      false, last.dictionary, &ovfl_used));

                    wrote_real_values = true;

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
                     * Original is an overflow item; we used it for a key and now we need another
                     * copy; read it into memory.
                     */
                    WT_ERR(__wt_dsk_cell_data_ref_kv(session, vpack, orig));
                    data = orig->data;
                    size = (uint32_t)orig->size;

                    ovfl_state = OVFL_IGNORE;
                    break;
                case OVFL_IGNORE:
                    /*
                     * Use the copied original value if the on-page value is an overflow value.
                     * Otherwise, use the on-page value.
                     */
                    if (F_ISSET(vpack, WT_CELL_UNPACK_OVERFLOW)) {
                        data = orig->data;
                        size = (uint32_t)orig->size;
                    } else {
                        data = vpack->data;
                        size = vpack->size;
                    }
                    break;
                }
            } else {
                twp = &upd_select.tw;

                switch (upd->type) {
                case WT_UPDATE_MODIFY:
                    cbt->slot = WT_COL_SLOT(page, cip);
                    WT_ERR(__wt_modify_reconstruct_from_upd_list(
                      session, cbt, upd, cbt->upd_value, WT_OPCTX_RECONCILATION));
                    __wt_value_return(cbt, cbt->upd_value);
                    data = cbt->iface.value.data;
                    size = (uint32_t)cbt->iface.value.size;
                    update_no_copy = false;
                    dictionary = btree->dictionary;
                    break;
                case WT_UPDATE_STANDARD:
                    data = upd->data;
                    size = upd->size;
                    dictionary = btree->dictionary;
                    break;
                case WT_UPDATE_TOMBSTONE:
                    deleted = true;
                    twp = &clear_tw;
                    r->key_removed_from_disk_image = true;
                    break;
                default:
                    WT_ERR(__wt_illegal_value(session, upd->type));
                }

                /*
                 * When a tombstone without a timestamp is written to disk, remove any historical
                 * versions that are greater in the history store for this key.
                 */
                if (upd_select.no_ts_tombstone && r->hs_clear_on_tombstone)
                    WT_ERR(__wti_rec_hs_clear_on_tombstone(session, r, src_recno, NULL,
                      upd->type == WT_UPDATE_TOMBSTONE ? false : true));
            }

compare:
            /*
             * If we have a record against which to compare and the records compare equal, increment
             * the RLE and continue. If the records don't compare equal, output the last record and
             * swap the last and current buffers: do NOT update the starting record number, we've
             * been doing that all along.
             */
            if (rle != 0) {
                if (WT_TIME_WINDOWS_EQUAL(&last.tw, twp) &&
                  ((deleted && last.deleted) ||
                    (!deleted && !last.deleted && last.value->size == size &&
                      (size == 0 || memcmp(last.value->data, data, size) == 0)))) {

                    /* The time window for deleted keys must be empty. */
                    WT_ASSERT(
                      session, (!deleted && !last.deleted) || WT_TIME_WINDOW_IS_EMPTY(&last.tw));

                    rle += repeat_count;
                    continue;
                }
                if (!last.deleted)
                    wrote_real_values = true;
                WT_ERR(__rec_col_var_helper(session, r, salvage, last.value, &last.tw, rle,
                  last.deleted, last.dictionary, NULL));
            }

            /*
             * Swap the current/last state.
             *
             * Reset RLE counter and turn on comparisons.
             */
            if (!deleted) {
                /*
                 * We can't simply assign the data values into the last buffer because they may have
                 * come from a copy built from an encoded/overflow cell and creating the next record
                 * is going to overwrite that memory. Check, because encoded/overflow cells aren't
                 * that common and we'd like to avoid the copy. If data was taken from the current
                 * unpack structure (which points into the page), or was taken from an update
                 * structure, we can just use the pointers, they're not moving.
                 */
                if (data == vpack->data || update_no_copy) {
                    last.value->data = data;
                    last.value->size = size;
                } else
                    WT_ERR(__wt_buf_set(session, last.value, data, size));
            }

            WT_TIME_WINDOW_COPY(&last.tw, twp);
            last.deleted = deleted;
            last.dictionary = dictionary;
            rle = repeat_count;
        }

        /*
         * The first time we find an overflow record we never used, discard the underlying blocks,
         * they're no longer useful.
         */
        if (ovfl_state == OVFL_UNUSED && vpack->raw != WT_CELL_VALUE_OVFL_RM)
            WT_ERR(__wt_ovfl_remove(session, page, vpack));
    }

    /* Walk any append list. */
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
        else {
            WT_ERR(__wti_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
            upd = upd_select.upd;
            n = WT_INSERT_RECNO(ins);
        }

        while (src_recno <= n) {
            update_no_copy = true; /* No data copy */
            deleted = false;

            /*
             * The application may have inserted records which left gaps in the name space, and
             * these gaps can be huge. If we're in a set of deleted records, skip the boring part.
             */
            if (src_recno < n) {
                deleted = true;
                if (last.deleted) {
                    /* The time window for deleted keys must be empty. */
                    WT_ASSERT(session, WT_TIME_WINDOW_IS_EMPTY(&last.tw));
                    /*
                     * The record adjustment is decremented by one so we can naturally fall into the
                     * RLE accounting below, where we increment rle by one, then continue in the
                     * outer loop, where we increment src_recno by one.
                     */
                    skip = (n - src_recno) - 1;
                    rle += skip;
                    src_recno += skip;
                } else
                    /* Set time window for the first deleted key in a deleted range. */
                    twp = &clear_tw;
            } else if (upd == NULL) {
                /* The updates on the key are all uncommitted so we write a deleted key to disk. */
                twp = &clear_tw;
                deleted = true;
            } else {
                /* Set time window for the key. */
                twp = &upd_select.tw;

                switch (upd->type) {
                case WT_UPDATE_MODIFY:
                    /*
                     * Impossible slot, there's no backing on-page item.
                     */
                    cbt->slot = UINT32_MAX;
                    WT_ERR(__wt_modify_reconstruct_from_upd_list(
                      session, cbt, upd, cbt->upd_value, WT_OPCTX_RECONCILATION));
                    __wt_value_return(cbt, cbt->upd_value);
                    data = cbt->iface.value.data;
                    size = (uint32_t)cbt->iface.value.size;
                    update_no_copy = false;
                    dictionary = btree->dictionary;
                    break;
                case WT_UPDATE_STANDARD:
                    data = upd->data;
                    size = upd->size;
                    dictionary = btree->dictionary;
                    break;
                case WT_UPDATE_TOMBSTONE:
                    twp = &clear_tw;
                    deleted = true;
                    break;
                default:
                    WT_ERR(__wt_illegal_value(session, upd->type));
                }

                /*
                 * When a tombstone without a timestamp is written to disk, remove any historical
                 * versions that are greater in the history store for this key.
                 */
                if (upd_select.no_ts_tombstone && r->hs_clear_on_tombstone)
                    WT_ERR(__wti_rec_hs_clear_on_tombstone(session, r, src_recno, NULL,
                      upd->type == WT_UPDATE_TOMBSTONE ? false : true));
            }

            /*
             * Handle RLE accounting and comparisons -- see comment above, this code fragment does
             * the same thing.
             */
            if (rle != 0) {
                if (WT_TIME_WINDOWS_EQUAL(&last.tw, twp) &&
                  ((deleted && last.deleted) ||
                    (!deleted && !last.deleted && last.value->size == size &&
                      (size == 0 || memcmp(last.value->data, data, size) == 0)))) {

                    /* The time window for deleted keys must be empty. */
                    WT_ASSERT(
                      session, (!deleted && !last.deleted) || WT_TIME_WINDOW_IS_EMPTY(&last.tw));

                    ++rle;
                    goto next;
                }
                if (!last.deleted)
                    wrote_real_values = true;
                WT_ERR(__rec_col_var_helper(session, r, salvage, last.value, &last.tw, rle,
                  last.deleted, last.dictionary, NULL));
            }

            /*
             * Swap the current/last state. We can't simply assign the data values into the last
             * buffer because they may be a temporary copy built from a chain of modified updates
             * and creating the next record will overwrite that memory. Check, we'd like to avoid
             * the copy. If data was taken from an update structure, we can just use the pointers,
             * they're not moving.
             */
            if (!deleted) {
                if (update_no_copy) {
                    last.value->data = data;
                    last.value->size = size;
                } else
                    WT_ERR(__wt_buf_set(session, last.value, data, size));
            }

            /* Ready for the next loop, reset the RLE counter. */
            WT_TIME_WINDOW_COPY(&last.tw, twp);
            last.deleted = deleted;
            last.dictionary = dictionary;
            rle = 1;

            /*
             * Move to the next record. It's not a simple increment because if it's the maximum
             * record, incrementing it wraps to 0 and this turns into an infinite loop.
             */
next:
            if (src_recno == UINT64_MAX)
                break;
            ++src_recno;
        }

        /*
         * Execute this loop once without an insert item to catch any missing records due to a
         * split, then quit.
         */
        if (ins == NULL)
            break;
    }

    /* If we were tracking a record, write it. */
    if (rle != 0) {
        if (!last.deleted)
            wrote_real_values = true;
        WT_ERR(__rec_col_var_helper(
          session, r, salvage, last.value, &last.tw, rle, last.deleted, last.dictionary, NULL));
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
    if (!wrote_real_values && salvage == NULL && r->leave_dirty == false && r->entries > 0) {
        WT_ASSERT(session, r->entries == 1);
        r->entries = 0;
        WT_STAT_CONN_DSRC_INCR(session, rec_vlcs_emptied_pages);
        /* Don't bother adjusting r->space_avail or r->first_free. */
    }

    /* Write the remnant page. */
    WT_ERR(__wti_rec_split_finish(session, r));

err:
    __wt_scr_free(session, &orig);
    return (ret);
}
