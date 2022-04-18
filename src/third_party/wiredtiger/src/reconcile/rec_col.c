/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_col_fix_bulk_insert_split_check --
 *     Check if a bulk-loaded fixed-length column store page needs to split.
 */
static inline int
__rec_col_fix_bulk_insert_split_check(WT_CURSOR_BULK *cbulk)
{
    WT_BTREE *btree;
    WT_RECONCILE *r;
    WT_SESSION_IMPL *session;

    session = CUR2S(cbulk);
    r = cbulk->reconcile;
    btree = S2BT(session);

    if (cbulk->entry == cbulk->nrecs) {
        if (cbulk->entry != 0) {
            /*
             * If everything didn't fit, update the counters and split.
             *
             * Boundary: split or write the page.
             *
             * No need to have a minimum split size boundary, all pages are filled 100% except the
             * last, allowing it to grow in the future.
             */
            __wt_rec_incr(
              session, r, cbulk->entry, __bitstr_size((size_t)cbulk->entry * btree->bitcnt));
            WT_RET(__wt_rec_split(session, r, 0, false));
        }
        cbulk->entry = 0;
        cbulk->nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);
    }
    return (0);
}

/*
 * __wt_bulk_insert_fix --
 *     Fixed-length column-store bulk insert.
 */
int
__wt_bulk_insert_fix(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WT_RECONCILE *r;

    r = cbulk->reconcile;
    btree = S2BT(session);
    cursor = &cbulk->cbt.iface;

    WT_RET(__rec_col_fix_bulk_insert_split_check(cbulk));
    __bit_setv(
      r->first_free, cbulk->entry, btree->bitcnt, deleted ? 0 : ((uint8_t *)cursor->value.data)[0]);
    ++cbulk->entry;
    ++r->recno;

    return (0);
}

/*
 * __wt_bulk_insert_fix_bitmap --
 *     Fixed-length column-store bulk insert.
 */
int
__wt_bulk_insert_fix_bitmap(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WT_RECONCILE *r;
    uint32_t entries, offset, page_entries, page_size;
    const uint8_t *data;

    r = cbulk->reconcile;
    btree = S2BT(session);
    cursor = &cbulk->cbt.iface;

    if (((r->recno - 1) * btree->bitcnt) & 0x7)
        WT_RET_MSG(session, EINVAL, "Bulk bitmap load not aligned on a byte boundary");
    for (data = cursor->value.data, entries = (uint32_t)cursor->value.size; entries > 0;
         entries -= page_entries, data += page_size) {
        WT_RET(__rec_col_fix_bulk_insert_split_check(cbulk));

        page_entries = WT_MIN(entries, cbulk->nrecs - cbulk->entry);
        page_size = __bitstr_size(page_entries * btree->bitcnt);
        offset = __bitstr_size(cbulk->entry * btree->bitcnt);
        memcpy(r->first_free + offset, data, page_size);
        cbulk->entry += page_entries;
        r->recno += page_entries;
    }
    return (0);
}

/*
 * __wt_bulk_insert_var --
 *     Variable-length column-store bulk insert.
 */
int
__wt_bulk_insert_var(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk, bool deleted)
{
    WT_BTREE *btree;
    WT_RECONCILE *r;
    WT_REC_KV *val;
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
        WT_RET(__wt_rec_cell_build_val(
          session, r, cbulk->last->data, cbulk->last->size, &tw, cbulk->rle));

    /* Boundary: split or write the page. */
    if (WT_CROSSING_SPLIT_BND(r, val->len))
        WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len, false));

    /* Copy the value onto the page. */
    if (btree->dictionary)
        WT_RET(__wt_rec_dict_replace(session, r, &tw, cbulk->rle, val));
    __wt_rec_image_copy(session, r, val);
    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &tw);

    /* Update the starting record number in case we split. */
    r->recno += cbulk->rle;

    return (0);
}

/*
 * __rec_col_merge --
 *     Merge in a split page.
 */
static int
__rec_col_merge(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
    WT_ADDR *addr;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_REC_KV *val;
    uint32_t i;

    mod = page->modify;

    val = &r->v;

    /* For each entry in the split array... */
    for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
        /* Update the starting record number in case we split. */
        r->recno = multi->key.recno;

        /* Build the value cell. */
        addr = &multi->addr;
        __wt_rec_cell_build_addr(session, r, addr, NULL, false, r->recno);

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, val->len))
            WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len, false));

        /* Copy the value onto the page. */
        __wt_rec_image_copy(session, r, val);
        WT_TIME_AGGREGATE_MERGE(session, &r->cur_ptr->ta, &addr->ta);
    }
    return (0);
}

/*
 * __wt_rec_col_int --
 *     Reconcile a column-store internal page.
 */
int
__wt_rec_col_int(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref)
{
    WT_ADDR *addr;
    WT_BTREE *btree;
    WT_CELL_UNPACK_ADDR *vpack, _vpack;
    WT_CHILD_STATE state;
    WT_DECL_RET;
    WT_PAGE *child, *page;
    WT_REC_KV *val;
    WT_REF *ref;
    WT_TIME_AGGREGATE ta;
    bool hazard;

    btree = S2BT(session);
    page = pageref->page;
    child = NULL;
    hazard = false;
    WT_TIME_AGGREGATE_INIT(&ta);

    val = &r->v;
    vpack = &_vpack;

    WT_RET(__wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxintlpage_precomp));

    /* For each entry in the in-memory page... */
    WT_INTL_FOREACH_BEGIN (session, page, ref) {
        /* Update the starting record number in case we split. */
        r->recno = ref->ref_recno;

        /*
         * Modified child. The page may be emptied or internally created during a split.
         * Deleted/split pages are merged into the parent and discarded.
         */
        WT_ERR(__wt_rec_child_modify(session, r, ref, &hazard, &state));
        addr = NULL;
        child = ref->page;

        switch (state) {
        case WT_CHILD_IGNORE:
            /* Ignored child. */
            WT_CHILD_RELEASE_ERR(session, hazard, ref);
            continue;

        case WT_CHILD_MODIFIED:
            /*
             * Modified child. Empty pages are merged into the parent and discarded.
             */
            switch (child->modify->rec_result) {
            case WT_PM_REC_EMPTY:
                /*
                 * Column-store pages are almost never empty, as discarding a page would remove a
                 * chunk of the name space. The exceptions are pages created when the tree is
                 * created, and never filled.
                 */
                WT_CHILD_RELEASE_ERR(session, hazard, ref);
                continue;
            case WT_PM_REC_MULTIBLOCK:
                WT_ERR(__rec_col_merge(session, r, child));
                WT_CHILD_RELEASE_ERR(session, hazard, ref);
                continue;
            case WT_PM_REC_REPLACE:
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
            /*
             * Deleted child where we write a proxy cell, not yet supported for column-store.
             */
            WT_ERR(__wt_illegal_value(session, state));
        }

        /*
         * Build the value cell. The child page address is in one of 3 places: if the page was
         * replaced, the page's modify structure references it and we built the value cell just
         * above in the switch statement. Else, the WT_REF->addr reference points to an on-page cell
         * or an off-page WT_ADDR structure: if it's an on-page cell and we copy it from the page,
         * else build a new cell.
         */
        if (addr == NULL && __wt_off_page(page, ref->addr))
            addr = ref->addr;
        if (addr == NULL) {
            __wt_cell_unpack_addr(session, page->dsk, ref->addr, vpack);
            if (F_ISSET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED)) {
                /* Need to rebuild the cell with the updated time info. */
                __wt_rec_cell_build_addr(session, r, NULL, vpack, false, ref->ref_recno);
            } else {
                val->buf.data = ref->addr;
                val->buf.size = __wt_cell_total_len(vpack);
                val->cell_len = 0;
                val->len = val->buf.size;
            }
            WT_TIME_AGGREGATE_COPY(&ta, &vpack->ta);
        } else {
            __wt_rec_cell_build_addr(session, r, addr, NULL, false, ref->ref_recno);
            WT_TIME_AGGREGATE_COPY(&ta, &addr->ta);
        }
        WT_CHILD_RELEASE_ERR(session, hazard, ref);

        /* Boundary: split or write the page. */
        if (__wt_rec_need_split(r, val->len))
            WT_ERR(__wt_rec_split_crossing_bnd(session, r, val->len, false));

        /* Copy the value (which is in val, val == r->v) onto the page. */
        __wt_rec_image_copy(session, r, val);
        WT_TIME_AGGREGATE_MERGE(session, &r->cur_ptr->ta, &ta);
    }
    WT_INTL_FOREACH_END;

    /* Write the remnant page. */
    return (__wt_rec_split_finish(session, r));

err:
    WT_CHILD_RELEASE(session, hazard, ref);
    return (ret);
}

/*
 * __wt_rec_col_fix --
 *     Reconcile a fixed-width, column-store leaf page.
 */
int
__wt_rec_col_fix(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref)
{
    WT_BTREE *btree;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_UPDATE *upd;
    WT_UPDATE_SELECT upd_select;
    uint64_t recno;
    uint32_t entry, nrecs;

    btree = S2BT(session);
    page = pageref->page;
    upd = NULL;

    WT_RET(__wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxleafpage));

    /* Copy the original, disk-image bytes into place. */
    if (page->entries != 0)
        memcpy(
          r->first_free, page->pg_fix_bitf, __bitstr_size((size_t)page->entries * btree->bitcnt));

    /* Update any changes to the original on-page data items. */
    WT_SKIP_FOREACH (ins, WT_COL_UPDATE_SINGLE(page)) {
        WT_RET(__wt_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
        upd = upd_select.upd;
        if (upd != NULL)
            __bit_setv(
              r->first_free, WT_INSERT_RECNO(ins) - pageref->ref_recno, btree->bitcnt, *upd->data);
    }

    /* Calculate the number of entries per page remainder. */
    entry = page->entries;
    nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail) - page->entries;
    r->recno += entry;

    /* Walk any append list. */
    for (ins = WT_SKIP_FIRST(WT_COL_APPEND(page));; ins = WT_SKIP_NEXT(ins)) {
        if (ins == NULL) {
            /*
             * If the page split, instantiate any missing records in
             * the page's name space. (Imagine record 98 is
             * transactionally visible, 99 wasn't created or is not
             * yet visible, 100 is visible. Then the page splits and
             * record 100 moves to another page. When we reconcile
             * the original page, we write record 98, then we don't
             * see record 99 for whatever reason. If we've moved
             * record 100, we don't know to write a deleted record
             * 99 on the page.)
             *
             * The record number recorded during the split is the
             * first key on the split page, that is, one larger than
             * the last key on this page, we have to decrement it.
             */
            if ((recno = page->modify->mod_col_split_recno) == WT_RECNO_OOB)
                break;
            recno -= 1;

            /*
             * The following loop assumes records to write, and the previous key might have been
             * visible.
             */
            if (r->recno > recno)
                break;
            upd = NULL;
        } else {
            WT_RET(__wt_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
            upd = upd_select.upd;
            recno = WT_INSERT_RECNO(ins);
        }
        for (;;) {
            /*
             * The application may have inserted records which left gaps in the name space.
             */
            for (; nrecs > 0 && r->recno < recno; --nrecs, ++entry, ++r->recno)
                __bit_setv(r->first_free, entry, btree->bitcnt, 0);

            if (nrecs > 0) {
                __bit_setv(r->first_free, entry, btree->bitcnt, upd == NULL ? 0 : *upd->data);
                --nrecs;
                ++entry;
                ++r->recno;
                break;
            }

            /*
             * If everything didn't fit, update the counters and split.
             *
             * Boundary: split or write the page.
             *
             * No need to have a minimum split size boundary, all pages are filled 100% except the
             * last, allowing it to grow in the future.
             */
            __wt_rec_incr(session, r, entry, __bitstr_size((size_t)entry * btree->bitcnt));
            WT_RET(__wt_rec_split(session, r, 0, false));

            /* Calculate the number of entries per page. */
            entry = 0;
            nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);
        }

        /*
         * Execute this loop once without an insert item to catch any missing records due to a
         * split, then quit.
         */
        if (ins == NULL)
            break;
    }

    /* Update the counters. */
    __wt_rec_incr(session, r, entry, __bitstr_size((size_t)entry * btree->bitcnt));

    /* Write the remnant page. */
    WT_RET(__wt_rec_split_finish(session, r));

    return (0);
}

/*
 * __wt_rec_col_fix_slvg --
 *     Reconcile a fixed-width, column-store leaf page created during salvage.
 */
int
__wt_rec_col_fix_slvg(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    uint64_t page_start, page_take;
    uint32_t entry, nrecs;

    btree = S2BT(session);
    page = pageref->page;

    /*
     * It's vanishingly unlikely and probably impossible for fixed-length column-store files to have
     * overlapping key ranges. It's possible for an entire key range to go missing (if a page is
     * corrupted and lost), but because pages can't split, it shouldn't be possible to find pages
     * where the key ranges overlap. That said, we check for it during salvage and clean up after it
     * here because it doesn't cost much and future column-store formats or operations might allow
     * for fixed-length format ranges to overlap during salvage, and I don't want to have to
     * retrofit the code later.
     */
    WT_RET(__wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxleafpage));

    /* We may not be taking all of the entries on the original page. */
    page_take = salvage->take == 0 ? page->entries : salvage->take;
    page_start = salvage->skip == 0 ? 0 : salvage->skip;

    /* Calculate the number of entries per page. */
    entry = 0;
    nrecs = WT_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);

    for (; nrecs > 0 && salvage->missing > 0; --nrecs, --salvage->missing, ++entry)
        __bit_setv(r->first_free, entry, btree->bitcnt, 0);

    for (; nrecs > 0 && page_take > 0; --nrecs, --page_take, ++page_start, ++entry)
        __bit_setv(r->first_free, entry, btree->bitcnt,
          __bit_getv(page->pg_fix_bitf, (uint32_t)page_start, btree->bitcnt));

    r->recno += entry;
    __wt_rec_incr(session, r, entry, __bitstr_size((size_t)entry * btree->bitcnt));

    /*
     * We can't split during salvage -- if everything didn't fit, it's all gone wrong.
     */
    if (salvage->missing != 0 || page_take != 0)
        WT_RET_PANIC(session, WT_PANIC, "%s page too large, attempted split during salvage",
          __wt_page_type_string(page->type));

    /* Write the page. */
    return (__wt_rec_split_finish(session, r));
}

/*
 * __rec_col_var_helper --
 *     Create a column-store variable length record cell and write it onto a page.
 */
static int
__rec_col_var_helper(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_SALVAGE_COOKIE *salvage,
  WT_ITEM *value, WT_TIME_WINDOW *tw, uint64_t rle, bool deleted, bool overflow_type)
{
    WT_BTREE *btree;
    WT_REC_KV *val;

    btree = S2BT(session);
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
    } else if (overflow_type) {
        val->cell_len =
          __wt_cell_pack_ovfl(session, &val->cell, WT_CELL_VALUE_OVFL, tw, rle, value->size);
        val->buf.data = value->data;
        val->buf.size = value->size;
        val->len = val->cell_len + value->size;
    } else
        WT_RET(__wt_rec_cell_build_val(session, r, value->data, value->size, tw, rle));

    /* Boundary: split or write the page. */
    if (__wt_rec_need_split(r, val->len))
        WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len, false));

    /* Copy the value onto the page. */
    if (!deleted && !overflow_type && btree->dictionary)
        WT_RET(__wt_rec_dict_replace(session, r, tw, rle, val));
    __wt_rec_image_copy(session, r, val);
    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, tw);

    /* Update the starting record number in case we split. */
    r->recno += rle;

    return (0);
}

/*
 * __wt_rec_col_var --
 *     Reconcile a variable-width column-store leaf page.
 */
int
__wt_rec_col_var(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    enum { OVFL_IGNORE, OVFL_UNUSED, OVFL_USED } ovfl_state;
    struct {
        WT_ITEM *value; /* Value */
        WT_TIME_WINDOW tw;
        bool deleted; /* If deleted */
    } last;
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *vpack, _vpack;
    WT_COL *cip;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(orig);
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_TIME_WINDOW clear_tw, *twp;
    WT_UPDATE *upd;
    WT_UPDATE_SELECT upd_select;
    uint64_t n, nrepeat, repeat_count, rle, skip, src_recno;
    uint32_t i, size;
    bool deleted, orig_deleted, update_no_copy;
    const void *data;

    btree = S2BT(session);
    vpack = &_vpack;
    page = pageref->page;
    WT_TIME_WINDOW_INIT(&clear_tw);
    twp = NULL;
    upd = NULL;
    size = 0;
    data = NULL;

    cbt = &r->update_modify_cbt;
    cbt->iface.session = (WT_SESSION *)session;

    /* Set the "last" values to cause failure if they're not set. */
    last.value = r->last;
    WT_TIME_WINDOW_INIT(&last.tw);
    last.deleted = false;

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
    WT_RET(__wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxleafpage_precomp));

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

            /*
             * Correct the number of records we're going to "take", pretending the missing records
             * were on the page.
             */
            salvage->take += salvage->missing;
        } else
            WT_ERR(__rec_col_var_helper(
              session, r, NULL, NULL, &clear_tw, salvage->missing, true, false));
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

        /*
         * If the original value is "deleted", there's no value to compare, we're done.
         */
        orig_deleted = vpack->type == WT_CELL_DEL;
        if (orig_deleted)
            goto record_loop;

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

        /*
         * If data is Huffman encoded, we have to decode it in order to compare it with the last
         * item we saw, which may have been an update string. This guarantees we find every single
         * pair of objects we can RLE encode, including applications updating an existing record
         * where the new value happens (?) to match a Huffman- encoded value in a previous or next
         * record.
         */
        WT_ERR(__wt_dsk_cell_data_ref(session, WT_PAGE_COL_VAR, vpack, orig));

record_loop:
        /*
         * Generate on-page entries: loop repeat records, looking for WT_INSERT entries matching the
         * record number. The WT_INSERT lists are in sorted order, so only need check the next one.
         */
        for (n = 0; n < nrepeat; n += repeat_count, src_recno += repeat_count) {
            upd = NULL;
            if (ins != NULL && WT_INSERT_RECNO(ins) == src_recno) {
                WT_ERR(__wt_rec_upd_select(session, r, ins, NULL, vpack, &upd_select));
                upd = upd_select.upd;
                ins = WT_SKIP_NEXT(ins);
            }

            update_no_copy = true; /* No data copy */
            repeat_count = 1;      /* Single record */
            deleted = false;

            if (upd == NULL) {
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
                    goto compare;
                }
                twp = &vpack->tw;

                /* Clear the on-disk cell time window if it is obsolete. */
                __wt_rec_time_window_clear_obsolete(session, NULL, vpack, r);

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
                        WT_ERR(__rec_col_var_helper(
                          session, r, salvage, last.value, &last.tw, rle, last.deleted, false));
                        rle = 0;
                    }

                    last.value->data = vpack->data;
                    last.value->size = vpack->size;
                    WT_ERR(__rec_col_var_helper(
                      session, r, salvage, last.value, twp, repeat_count, false, true));

                    /* Track if page has overflow items. */
                    r->ovfl_items = true;

                    ovfl_state = OVFL_USED;
                    continue;
                case OVFL_USED:
                    /*
                     * Original is an overflow item; we used it for a key and now we need another
                     * copy; read it into memory.
                     */
                    WT_ERR(__wt_dsk_cell_data_ref(session, WT_PAGE_COL_VAR, vpack, orig));

                    ovfl_state = OVFL_IGNORE;
                /* FALLTHROUGH */
                case OVFL_IGNORE:
                    /*
                     * Original is an overflow item and we were forced to copy it into memory, or
                     * the original wasn't an overflow item; use the data copied into orig.
                     */
                    data = orig->data;
                    size = (uint32_t)orig->size;
                    break;
                }
            } else {
                twp = &upd_select.tw;

                switch (upd->type) {
                case WT_UPDATE_MODIFY:
                    cbt->slot = WT_COL_SLOT(page, cip);
                    WT_ERR(
                      __wt_modify_reconstruct_from_upd_list(session, cbt, upd, cbt->upd_value));
                    WT_ERR(__wt_value_return(cbt, cbt->upd_value));
                    data = cbt->iface.value.data;
                    size = (uint32_t)cbt->iface.value.size;
                    update_no_copy = false;
                    break;
                case WT_UPDATE_STANDARD:
                    data = upd->data;
                    size = upd->size;
                    /*
                     * When an out-of-order or mixed-mode tombstone is getting written to disk,
                     * remove any historical versions that are greater in the history store for this
                     * key.
                     */
                    if (upd_select.ooo_tombstone && r->hs_clear_on_tombstone)
                        WT_ERR(__wt_rec_hs_clear_on_tombstone(
                          session, r, twp->durable_stop_ts, src_recno, NULL, true));

                    break;
                case WT_UPDATE_TOMBSTONE:
                    /*
                     * When an out-of-order or mixed-mode tombstone is getting written to disk,
                     * remove any historical versions that are greater in the history store for this
                     * key.
                     */
                    if (upd_select.ooo_tombstone && r->hs_clear_on_tombstone)
                        WT_ERR(__wt_rec_hs_clear_on_tombstone(
                          session, r, twp->durable_stop_ts, src_recno, NULL, false));

                    deleted = true;
                    twp = &clear_tw;
                    break;
                default:
                    WT_ERR(__wt_illegal_value(session, upd->type));
                }
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
                WT_ERR(__rec_col_var_helper(
                  session, r, salvage, last.value, &last.tw, rle, last.deleted, false));
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
        if (ins == NULL) {
            /*
             * If the page split, instantiate any missing records in
             * the page's name space. (Imagine record 98 is
             * transactionally visible, 99 wasn't created or is not
             * yet visible, 100 is visible. Then the page splits and
             * record 100 moves to another page. When we reconcile
             * the original page, we write record 98, then we don't
             * see record 99 for whatever reason. If we've moved
             * record 100, we don't know to write a deleted record
             * 99 on the page.)
             *
             * Assert the recorded record number is past the end of
             * the page.
             *
             * The record number recorded during the split is the
             * first key on the split page, that is, one larger than
             * the last key on this page, we have to decrement it.
             */
            if ((n = page->modify->mod_col_split_recno) == WT_RECNO_OOB)
                break;
            WT_ASSERT(session, n >= src_recno);
            n -= 1;

            upd = NULL;
        } else {
            WT_ERR(__wt_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
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
                    WT_ERR(
                      __wt_modify_reconstruct_from_upd_list(session, cbt, upd, cbt->upd_value));
                    WT_ERR(__wt_value_return(cbt, cbt->upd_value));
                    data = cbt->iface.value.data;
                    size = (uint32_t)cbt->iface.value.size;
                    update_no_copy = false;
                    break;
                case WT_UPDATE_STANDARD:
                    data = upd->data;
                    size = upd->size;
                    break;
                case WT_UPDATE_TOMBSTONE:
                    twp = &clear_tw;
                    deleted = true;
                    break;
                default:
                    WT_ERR(__wt_illegal_value(session, upd->type));
                }
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
                WT_ERR(__rec_col_var_helper(
                  session, r, salvage, last.value, &last.tw, rle, last.deleted, false));
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
    if (rle != 0)
        WT_ERR(__rec_col_var_helper(
          session, r, salvage, last.value, &last.tw, rle, last.deleted, false));

    /* Write the remnant page. */
    ret = __wt_rec_split_finish(session, r);

err:
    __wt_scr_free(session, &orig);
    return (ret);
}
