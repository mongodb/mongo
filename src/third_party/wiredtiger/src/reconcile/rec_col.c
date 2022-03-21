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
            __bit_clear_end(
              WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem), cbulk->entry, btree->bitcnt);
            WT_RET(__wt_rec_split(session, r, 0));
        }
        cbulk->entry = 0;
        cbulk->nrecs = WT_COL_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);
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
    WT_TIME_WINDOW tw;

    r = cbulk->reconcile;
    btree = S2BT(session);
    cursor = &cbulk->cbt.iface;

    WT_RET(__rec_col_fix_bulk_insert_split_check(cbulk));
    __bit_setv(
      r->first_free, cbulk->entry, btree->bitcnt, deleted ? 0 : ((uint8_t *)cursor->value.data)[0]);
    ++cbulk->entry;
    ++r->recno;

    /*
     * Initialize the time aggregate that's going into the parent page. It's necessary to update an
     * aggregate at least once if it's been initialized for merging, or it will fail validation.
     * Also, it should reflect the fact that we've just loaded a batch of stable values.
     */
    WT_TIME_WINDOW_INIT(&tw);
    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &tw);
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
    WT_TIME_WINDOW tw;
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

    /* Initialize the time aggregate that's going into the parent page. See note above. */
    WT_TIME_WINDOW_INIT(&tw);
    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &tw);
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
        WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len));

    /* Copy the value onto the page. */
    if (btree->dictionary)
        WT_RET(__wt_rec_dict_replace(session, r, &tw, cbulk->rle, val));
    __wt_rec_image_copy(session, r, val);

    /* Initialize the time aggregate that's going into the parent page. See note above. */
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
            WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len));

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

    WT_RET(
      __wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxintlpage_precomp, 0));

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
            WT_ERR(__wt_rec_split_crossing_bnd(session, r, val->len));

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
 * __wt_col_fix_estimate_auxiliary_space --
 *     Estimate how much on-disk auxiliary space a fixed-length column store page will need.
 */
static uint32_t
__wt_col_fix_estimate_auxiliary_space(WT_PAGE *page)
{
    WT_INSERT *ins;
    uint32_t count;

    count = 0;

    /*
     * Iterate both the update and append lists to count the number of possible time windows. This
     * isn't free, but it's likely a win if it can avoid having to reallocate the write buffer in
     * the middle of reconciliation.
     *
     */
    WT_SKIP_FOREACH (ins, WT_COL_UPDATE_SINGLE(page))
        count++;
    WT_SKIP_FOREACH (ins, WT_COL_APPEND(page))
        count++;

    /* Add in the existing time windows. */
    if (WT_COL_FIX_TWS_SET(page))
        count += page->pg_fix_numtws;

    /*
     * Each time window record is two cells and might take up as much as 63 bytes:
     *     - 1: key cell descriptor byte
     *     - 5: key (32-bit recno offset)
     *     - 1: value cell descriptor byte
     *     - 1: value cell time window descriptor byte
     *     - 36: up to 4 64-bit timestamps
     *     - 18: up to 2 64-bit transaction ids
     *     - 1: zero byte for value length
     *     - 0: value
     *
     * For now, allocate enough space to hold a maximal cell pair for each possible time window.
     * This is perhaps too pessimistic. Also include the reservation for header space, since the
     * downstream code counts that in the auxiliary space.
     */
    return (count * 63 + WT_COL_FIX_AUXHEADER_RESERVATION);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __rec_col_fix_get_bitmap_size --
 *     Figure the bitmap size of a new page from the reconciliation info.
 */
static uint32_t
__rec_col_fix_get_bitmap_size(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    uint32_t primary_size;

    /* Figure the size of the primary part of the page by subtracting off the header. */
    primary_size = r->aux_start_offset - WT_COL_FIX_AUXHEADER_RESERVATION;

    /* Subtract off the main page header. */
    return (primary_size - WT_PAGE_HEADER_BYTE_SIZE(S2BT(session)));
}
#endif

/*
 * __wt_rec_col_fix_addtw --
 *     Create a fixed-length column store time window cell and add it to the new page image.
 */
static int
__wt_rec_col_fix_addtw(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, uint32_t recno_offset, WT_TIME_WINDOW *tw)
{
    WT_REC_KV *key, *val;
    size_t add_len, len;
    uint8_t keyspace[WT_INTPACK64_MAXSIZE], *p;

    WT_ASSERT(session,
      recno_offset <= ((__rec_col_fix_get_bitmap_size(session, r)) * 8) / S2BT(session)->bitcnt);

    key = &r->k;
    val = &r->v;

    /* Pack the key. */
    p = keyspace;
    WT_RET(__wt_vpack_uint(&p, sizeof(keyspace), recno_offset));
    key->buf.data = keyspace;
    key->buf.size = WT_PTRDIFF(p, keyspace);
    key->cell_len = __wt_cell_pack_leaf_key(&key->cell, 0, key->buf.size);
    key->len = key->cell_len + key->buf.size;

    /* Pack the value, which is empty, but with a time window. */
    WT_RET(__wt_rec_cell_build_val(session, r, NULL, 0, tw, 0));

    /* Figure how much space we need, and reallocate the page if about to run out. */
    len = key->len + val->len;
    if (len > r->aux_space_avail) {
        /*
         * Reallocate the page. Increase the size by 1/3 of the auxiliary space. This is arbitrary,
         * but chosen on purpose (instead of just doubling the size of the page image, which is the
         * usual thing to do) because we already made a generous estimate of the required auxiliary
         * space, and if we don't fit it's probably because a few extra updates happened, not
         * because a huge amount more time window data suddenly appeared. Use a fraction of the
         * current space to avoid adverse asymptotic behavior if a lot of stuff _did_ appear, but
         * not a huge one to avoid wasting memory.
         */
        add_len = (r->page_size - r->aux_start_offset) / 3;
        /* Just in case. */
        if (add_len < len)
            add_len = len * 2;
        WT_RET(__wt_rec_split_grow(session, r, add_len));
    }

    /* Copy both cells onto the page. This counts as one entry. */
    __wt_rec_auximage_copy(session, r, 0, key);
    __wt_rec_auximage_copy(session, r, 1, val);

    WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, tw);

    /* If we're on key 3 we should have just written at most the 4th time window. */
    WT_ASSERT(session, r->aux_entries <= recno_offset + 1);

    return (0);
}

/*
 * __wt_rec_col_fix --
 *     Reconcile a fixed-width, column-store leaf page.
 */
int
__wt_rec_col_fix(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *pageref, WT_SALVAGE_COOKIE *salvage)
{
    WT_BTREE *btree;
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_DECL_RET;
    WT_INSERT *ins;
    WT_PAGE *page;
    WT_UPDATE *upd;
    WT_UPDATE_SELECT upd_select;
    uint64_t curstartrecno, i, rawbitmapsize, origstartrecno, recno;
    uint32_t auxspace, bitmapsize, entry, maxrecs, nrecs, numtws, tw;
    uint8_t val;

    btree = S2BT(session);
    /*
     * Blank the unpack record in case we need to use it before unpacking anything into it. The
     * visibility code currently only uses the value and the time window, and asserts about the
     * type, but that could change so be careful.
     */
    memset(&unpack, 0, sizeof(unpack));
    page = pageref->page;
    upd = NULL;
    /* Track the start of the current page we're working on. Changes when we split. */
    curstartrecno = pageref->ref_recno;
    /* Also check where the disk image starts, which might be different in salvage. */
    origstartrecno = page->dsk == NULL ? WT_RECNO_OOB : page->dsk->recno;

    /*
     * The configured max leaf page size is the size of the bitmap data on the page, not including
     * the time window data. The actual page size is (often) larger. Estimate how much more space we
     * need. This isn't a guarantee (more inserts can happen while we're working) but it should
     * avoid needing to reallocate the page buffer in the common case.
     *
     * Do this before fiddling around with the salvage logic so the latter can make sure the page
     * size doesn't try to grow past 2^32.
     */
    auxspace = __wt_col_fix_estimate_auxiliary_space(page);

    /*
     * The salvage code may have found overlapping ranges in the key namespace, in which case we're
     * given (a) either a count of missing entries to write at the beginning of the page or a count
     * of existing entries to skip over at the start of the page, and (b) a count of the number of
     * entries to take from the page.
     *
     * In theory we shouldn't ever get pages with overlapping key ranges, even during salvage.
     * Because all the pages are the same size, they should always begin at the same recnos,
     * regardless of what might have happened at runtime.
     *
     * In practice this is not so clear; there are at least three ways that odd-sized pages can
     * appear (and it's possible that more might be added in the future) and once that happens, it
     * can happen differently on different runs and lead to overlapping key ranges detected during
     * salvage. (Because pages are never merged once written, in order to get overlapping ranges of
     * keys in VLCS one must also be seeing the results of different splits on different runs, so
     * such scenarios are within the scope of what salvage needs to handle.)
     *
     * First, odd-sized pages can be generated by in-memory (append) splits. These do not honor the
     * configured page size and are based on in-memory size estimates, which in FLCS are quite
     * different from on-disk sizes. The resulting sizes can be completely arbitrary. Note that even
     * if things are changed to keep this from happening in the future, it has been this way for a
     * long time so it's reasonable to assume that in general any deployed database with an FLCS
     * column can already have odd-sized pages in it.
     *
     * Second, it isn't clear that we prevent the user from changing the configured leaf_page_max
     * after there are already pages in the database, nor is it clear that we should; if this were
     * to happen we'll then have pages of multiple sizes. This is less likely to generate
     * overlapping ranges, but it isn't impossible, especially in conjunction with the next case.
     *
     * Third, because at salvage time we account for missing key ranges by writing larger pages and
     * splitting them again later, as described below, if there are odd-sized pages before salvage,
     * running salvage can shift around where the page boundaries are. Thus on a subsequent salvage
     * run, overlaps that wouldn't otherwise be possible can manifest.
     *
     * For these reasons, and because we don't want to have to refit the code later if more reasons
     * appear, and because it doesn't cost much, we do check for overlapping ranges during salvage
     * (this doesn't even require additional code because the column-store internal pages are the
     * same for VLCS and FLCS) and handle it here.
     */
    if (salvage != NULL) {
        /* We should not already be done. */
        WT_ASSERT(session, salvage->done == false);

        /* We shouldn't both have missing records to insert and records to skip. */
        WT_ASSERT(session, salvage->missing == 0 || salvage->skip == 0);

        /* If there's a page, we shouldn't have been asked for more than was already on the page. */
        WT_ASSERT(
          session, page->dsk == NULL || salvage->skip + salvage->take <= page->dsk->u.entries);
        /* Allow us to be called without a disk page, to generate a fresh page of missing items. */
        WT_ASSERT(session,
          page->dsk != NULL || (salvage->missing > 0 && salvage->skip + salvage->take == 0));

        /*
         * The upstream code changed the page start "for" us; assert things are as expected. That
         * is: it should have been adjusted either down by the missing count or up by the skip
         * count. Skip if there's no disk image since in that case there's no original start. Under
         * normal circumstances salvage will always have a disk image, since that's the point, but
         * this code is deliberately written so salvage can ask it to generate fresh pages of zeros
         * to help populate missing ranges of the key space, and if code for that ever appears it
         * won't have a disk image to pass.
         */
        WT_ASSERT(session,
          page->dsk == NULL ||
            (curstartrecno + salvage->missing == origstartrecno + salvage->skip));

        /*
         * Compute how much space we need for the resulting bitmap data.
         *
         * This may be vastly greater than the intended maximum page size. If a page gets corrupted
         * and is thus lost, its entire key range will be missing, and on the next page we'll be
         * asked to fill in those keys. In fact, if a series of pages goes missing, all the dropped
         * keys will appear in salvage->missing on the next keys. So salvage->missing may be not
         * only greater than the maximum page size but a multiple of it. Since we cannot split
         * during salvage, and unlike VLCS we have no compact representation for a large range of
         * deleted keys, if this happens the only possible approach is to create a monster page,
         * write it out, and live with it, since it also currently isn't possible to re-split it
         * later once it's been created.
         *
         * FUTURE: I've intentionally written the code here to allow the upstream code to
         * manufacture empty new pages and reconcile each of them with salvage->missing equal to the
         * intended items per page, instead of asking us to produce monster pages, since doing so
         * was cheap. Whether doing this in the upstream code is feasible or not I dunno, but it's
         * perhaps worth looking into.
         *
         * FUTURE: Alternatively, when we get fast-delete support for column store it is reasonable
         * to teach the upstream code to produce fast-delete entries for whole missing pages rather
         * than have us materialize all the zeros.
         *
         * In principle if we have a small number of entries to take, we could generate a small page
         * rather than allocating the full size. At least for the moment this won't work because we
         * assume elsewhere that any small page might be appended to.
         */
        rawbitmapsize = WT_ALIGN(
          WT_COL_FIX_ENTRIES_TO_BYTES(btree, salvage->take + salvage->missing), btree->allocsize);

        /* Salvage is the backup plan: don't let this fail. */
        auxspace *= 2;

        if (rawbitmapsize + auxspace > UINT32_MAX || salvage->take + salvage->missing > UINT32_MAX)
            WT_RET_PANIC(session, WT_PANIC,
              "%s page too large (%" PRIu64 "); cannot split it during salvage",
              __wt_page_type_string(page->type), rawbitmapsize + auxspace);

        bitmapsize = (uint32_t)rawbitmapsize;
        if (bitmapsize < btree->maxleafpage)
            bitmapsize = btree->maxleafpage;
    } else {
        /* Under ordinary circumstances the bitmap size is the configured maximum page size. */
        bitmapsize = btree->maxleafpage;

        /* If not in salvage, there should be no shenanigans with the page start. */
        WT_ASSERT(session, page->dsk == NULL || curstartrecno == origstartrecno);

        /*
         * In theory the page could have been generated by a prior salvage run and be oversized. If
         * so, preserve the size. In principle such pages should be split, but the logic below does
         * not support that and I don't want to complicate it just to support this (very marginal)
         * case.
         */
        if (bitmapsize < __bitstr_size((size_t)page->entries * btree->bitcnt))
            bitmapsize = (uint32_t)__bitstr_size((size_t)page->entries * btree->bitcnt);
    }

    WT_RET(__wt_rec_split_init(session, r, page, curstartrecno, bitmapsize, auxspace));

    /* Remember where we are. */
    entry = 0;

    if (salvage != NULL) {
        /* If salvage wants us to insert entries, do that. */
        if (salvage->missing > 0) {
            memset(r->first_free, 0, __bitstr_size((size_t)salvage->missing * btree->bitcnt));
            entry += (uint32_t)salvage->missing;
            salvage->missing = 0;
        }

        /*
         * Now copy the entries from the page data. We could proceed one at a time until we reach
         * byte-alignment and then memcpy, but don't do that, on the grounds that it would be easy
         * to get the code wrong and hard to test it.
         */
        for (i = salvage->skip; i < salvage->skip + salvage->take; i++, entry++)
            __bit_setv(
              r->first_free, entry, btree->bitcnt, __bit_getv(page->pg_fix_bitf, i, btree->bitcnt));
        salvage->skip = 0;
        salvage->take = 0;
    } else if (page->entries != 0) {
        /* Copy the original, disk-image bytes into place. */
        memcpy(
          r->first_free, page->pg_fix_bitf, __bitstr_size((size_t)page->entries * btree->bitcnt));
        entry += page->entries;
    }

    /* Remember how far we can go before the end of page. */
    maxrecs = WT_COL_FIX_BYTES_TO_ENTRIES(btree, r->space_avail);

    /*
     * Iterate over the data items on the page. We need to go through both the insert list and the
     * timestamp index together, to make sure that if we have an update for an item that also has a
     * time window in the existing on-disk page we write out at most one time window and it's the
     * one from the update. (Also, we want the keys to come out in order.)
     *
     * Note that if we're in salvage, we might be changing the page's start recno. This makes the
     * offset computations complicated: offsets from the old page are relative to the old page start
     * (origstartrecno, which came from the disk image) and offsets from the new page are relative
     * to the new page start, which is curstartrecno. (And also ref->recno, but we don't use the
     * latter in case anyone wants to rewrite this code to split in the middle of the existing
     * bitmap.)
     *
     * So for time windows, when reading compute the absolute recno by adding the old page start,
     * and recompute it against the new page start when writing. (Note that at this point we can't
     * have split yet, so these are the same if we aren't in salvage, but if we changed things so
     * that we could, this would still be the correct computation.)
     *
     * Apply the bitmap data changes from the update too, of course.
     *
     * Note: origstartrecno is not valid if there is no prior disk image, but in that case there
     * will also be no time windows, and also nothing in the update (rather than insert) list.
     */

    tw = 0;
    numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;

    if (salvage != NULL && salvage->skip > 0) {
        /* Salvage wanted us to skip some records. Skip their time windows too. */
        WT_ASSERT(session, curstartrecno > origstartrecno);
        while (tw < numtws && origstartrecno + page->pg_fix_tws[tw].recno_offset < curstartrecno)
            tw++;
    }

    WT_SKIP_FOREACH (ins, WT_COL_UPDATE_SINGLE(page)) {
        recno = WT_INSERT_RECNO(ins);

        if (salvage != NULL && (recno < curstartrecno || recno >= curstartrecno + entry))
            /* Update for skipped item. Shouldn't happen, but just in case it does, skip it. */
            continue;

        /* Copy in all the preexisting time windows for keys before this one. */
        while (tw < numtws && origstartrecno + page->pg_fix_tws[tw].recno_offset < recno) {
            /* Get the previous time window so as to copy it. */
            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

            /* Clear the on-disk cell time window if it is obsolete. */
            __wt_rec_time_window_clear_obsolete(session, NULL, &unpack, r);

            /* If it's from a previous run, it might become empty; if so, skip it. */
            if (!WT_TIME_WINDOW_IS_EMPTY(&unpack.tw))
                WT_ERR(__wt_rec_col_fix_addtw(session, r,
                  (uint32_t)(origstartrecno + page->pg_fix_tws[tw].recno_offset - curstartrecno),
                  &unpack.tw));
            tw++;
        }

        /*
         * Fake up an unpack record to pass to update selection; it needs to have the current
         * on-disk value and its timestamp, if any, and it also needs to be tagged as a value cell.
         * This is how that value gets into the history store if that's needed.
         */
        if (tw < numtws && origstartrecno + page->pg_fix_tws[tw].recno_offset == recno) {
            /* Get the on-disk time window by unpacking the value cell. */
            cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
            __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);
        } else {
            /* Fake up a value cell with a default time window. */
            unpack.type = WT_CELL_VALUE;
            WT_TIME_WINDOW_INIT(&unpack.tw);
        }

        /*
         * Stick in the current on-disk value. We can't use __bit_getv_recno here because it
         * implicitly uses pageref->ref_recno to figure the offset; that's wrong if salvage has
         * changed the page origin.
         */
        WT_ASSERT(session, page->dsk != NULL && origstartrecno != WT_RECNO_OOB);
        val = __bit_getv(page->pg_fix_bitf, recno - origstartrecno, btree->bitcnt);
        unpack.data = &val;
        unpack.size = 1;

        WT_ERR(__wt_rec_upd_select(session, r, ins, NULL, &unpack, &upd_select));
        upd = upd_select.upd;
        if (upd == NULL) {
            /*
             * It apparently used to be possible to get back no update but a nonempty time window to
             * apply to the current on-disk value. As of Oct. 2021 this is no longer the case;
             * instead we get back an update with a copy of the current on-disk value. In case of
             * future changes, assert that there's nothing to do.
             */
            WT_ASSERT(session, WT_TIME_WINDOW_IS_EMPTY(&upd_select.tw));
            continue;
        }

        /* If there's an update to apply, apply the value. */

        if (upd->type == WT_UPDATE_TOMBSTONE) {
            /*
             * When an out-of-order or mixed-mode tombstone is getting written to disk, remove any
             * historical versions that are greater in the history store for this key.
             */
            if (upd_select.ooo_tombstone && r->hs_clear_on_tombstone)
                WT_ERR(__wt_rec_hs_clear_on_tombstone(
                  session, r, upd_select.tw.durable_stop_ts, recno, NULL, false));

            val = 0;
        } else {
            /* MODIFY is not allowed in FLCS. */
            WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
            val = *upd->data;
        }

        /* Write the data. */
        __bit_setv(r->first_free, recno - curstartrecno, btree->bitcnt, val);

        /* Write the time window. */
        if (!WT_TIME_WINDOW_IS_EMPTY(&upd_select.tw)) {
            /*
             * When an out-of-order or mixed-mode tombstone is getting written to disk, remove any
             * historical versions that are greater in the history store for this key.
             */
            if (upd_select.ooo_tombstone && r->hs_clear_on_tombstone)
                WT_ERR(__wt_rec_hs_clear_on_tombstone(
                  session, r, upd_select.tw.durable_stop_ts, recno, NULL, true));

            WT_ERR(__wt_rec_col_fix_addtw(
              session, r, (uint32_t)(recno - curstartrecno), &upd_select.tw));
        }

        /* If there was an entry in the time windows index for this key, skip over it. */
        if (tw < numtws && origstartrecno + page->pg_fix_tws[tw].recno_offset == recno)
            tw++;

        /* We should never see an update off the end of the tree. Those should be inserts. */
        WT_ASSERT(session, recno - curstartrecno < entry);
    }

    /* Copy all the remaining time windows, if any. */
    while (tw < numtws) {
        /* Get the old page's time window so as to copy it. */
        cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
        __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

        recno = origstartrecno + page->pg_fix_tws[tw].recno_offset;
        if (salvage != NULL && (recno < curstartrecno || recno >= curstartrecno + entry))
            /* This time window is for an item salvage wants us to skip. */
            continue;

        /* Clear the on-disk cell time window if it is obsolete. */
        __wt_rec_time_window_clear_obsolete(session, NULL, &unpack, r);

        /* If it's from a previous run, it might become empty; if so, skip it. */
        if (!WT_TIME_WINDOW_IS_EMPTY(&unpack.tw))
            WT_ERR(
              __wt_rec_col_fix_addtw(session, r, (uint32_t)(recno - curstartrecno), &unpack.tw));
        tw++;
    }

    /*
     * Figure out how much more space is left. This is how many more entries will fit in in the
     * bitmap data. We have to accommodate the auxiliary data for those entries, even if it becomes
     * large. We can't split based on the auxiliary image size, at least not without a major
     * rewrite.
     */
    nrecs = maxrecs - entry;
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
             *
             * Assert that we haven't already overrun the split; that is,
             * r->recno (the next key to write) should not be greater.
             */
            if ((recno = page->modify->mod_col_split_recno) == WT_RECNO_OOB)
                break;

            WT_ASSERT(session, r->recno <= recno);
            recno -= 1;

            /*
             * The following loop assumes records to write, and the previous key might have been
             * visible. If so, we had r->recno == recno before the decrement.
             */
            if (r->recno > recno)
                break;
            upd = NULL;
            /* Make sure not to apply an uninitialized time window, or one from another key. */
            WT_TIME_WINDOW_INIT(&unpack.tw);
        } else {
            /* We shouldn't ever get appends during salvage. */
            WT_ASSERT(session, salvage == NULL);

            WT_ERR(__wt_rec_upd_select(session, r, ins, NULL, NULL, &upd_select));
            upd = upd_select.upd;
            recno = WT_INSERT_RECNO(ins);
            /*
             * Currently __wt_col_modify assumes that all restored updates are updates rather than
             * appends. Therefore, if we see an invisible update, we need to write a value under it
             * (instead of just skipping by) -- otherwise, when it's restored after reconciliation
             * is done, __wt_col_modify mishandles it. Fixing __wt_col_modify to handle restored
             * appends appears to be straightforward (and would reduce the tendency of the end of
             * the tree to move around nontransactionally) but is not on the critical path, so I'm
             * not going to do it for now. But in principle we can check here for a null update and
             * continue to the next insert entry.
             */
        }
        for (;;) {
            /*
             * The application may have inserted records which left gaps in the name space. Note:
             * nrecs is the number of bitmap entries left on the page.
             */
            for (; nrecs > 0 && r->recno < recno; --nrecs, ++entry, ++r->recno)
                __bit_setv(r->first_free, entry, btree->bitcnt, 0);

            if (nrecs > 0) {
                /* There's still space; write the inserted value. */
                WT_ASSERT(session, curstartrecno + entry == recno);
                if (upd == NULL || upd->type == WT_UPDATE_TOMBSTONE)
                    val = 0;
                else {
                    /* MODIFY is not allowed in FLCS. */
                    WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
                    val = *upd->data;
                }
                __bit_setv(r->first_free, entry, btree->bitcnt, val);
                if (upd != NULL && !WT_TIME_WINDOW_IS_EMPTY(&upd_select.tw))
                    WT_ERR(__wt_rec_col_fix_addtw(session, r, entry, &upd_select.tw));
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

            /* If there are entries we didn't write timestamps for, aggregate a stable timestamp. */
            if (r->aux_entries < r->entries) {
                WT_TIME_WINDOW_INIT(&unpack.tw);
                WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &unpack.tw);
            }

            /* Make sure the trailing bits in the bitmap get cleared. */
            __bit_clear_end(
              WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem), r->entries, btree->bitcnt);

            /* Now split. */
            WT_ERR(__wt_rec_split(session, r, 0));

            /* (Re)calculate the number of entries per page. */
            entry = 0;
            nrecs = maxrecs;
            curstartrecno = r->recno;
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

    /*
     * If there are entries we didn't write timestamps for, aggregate in a stable timestamp. Do this
     * when there are no entries too, just in case that happens. Otherwise the aggregate, which was
     * initialized for merging, will fail validation if nothing's been merged into it.
     */
    if (r->aux_entries < r->entries || r->entries == 0) {
        WT_TIME_WINDOW_INIT(&unpack.tw);
        WT_TIME_AGGREGATE_UPDATE(session, &r->cur_ptr->ta, &unpack.tw);
    }

    /* Make sure the trailing bits in the bitmap get cleared. */
    __bit_clear_end(WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem), r->entries, btree->bitcnt);

    /* Write the remnant page. */
    WT_ERR(__wt_rec_split_finish(session, r));

err:
    return (ret);
}

/*
 * __wt_rec_col_fix_write_auxheader --
 *     Write the auxiliary header into the page image.
 */
void
__wt_rec_col_fix_write_auxheader(WT_SESSION_IMPL *session, uint32_t entries,
  uint32_t aux_start_offset, uint32_t auxentries, uint8_t *image, size_t size)
{
    WT_BTREE *btree;
    uint32_t auxheaderoffset, bitmapsize, offset, space;
    uint8_t *endp, *p;

    btree = S2BT(session);
    WT_UNUSED(size); /* only used in DIAGNOSTIC */

    WT_ASSERT(session, size <= UINT32_MAX);

    /*
     * Compute some positions.
     *
     * If the page is full, or oversized, the page contents are as follows:
     *    - the page header
     *    - the bitmap data
     *    - the auxiliary header
     *    - the auxiliary data
     *
     * If the page is not full, the page contents are as follows:
     *    - the page header
     *    - the bitmap data
     *    - the auxiliary header
     *    - some waste space
     *    - the auxiliary data
     *
     * During normal operation we don't know if the page will be full or not; if it isn't already
     * full this depends on the append list, but we can only iterate the append list once (for
     * atomicity) and we need to start writing auxiliary data before we get to that point.
     *
     * Therefore, we always begin the page assuming the primary data will be a full page, and write
     * the auxiliary data in the proper position for that. If the page ends up not full, there is a
     * gap. We always write the auxiliary header immediately after the bitmap data, so we can find
     * it easily when we read the page back in; the gap thus appears between the auxiliary header
     * and the auxiliary data.
     *
     * (FUTURE: if the auxiliary data is small we could memmove it; this isn't free, but might be
     * cheaper than writing out the waste space and then reading it back in. Note that in an ideal
     * world only the last page in the tree is short, so the waste is limited, but currently there
     * are also other ways for odd-sized pages to appear.)
     *
     * Salvage needs to be able to write out oversized pages, and then once that happens currently
     * they can't be split again later. For these pages we know what the bitmap size will be
     * (because there are no appends during salvage, and if we see appends to an oversize page at
     * some later point we aren't going to grow the page and they'll go on the next one) so we can
     * always put the auxiliary data in the right place up front.
     *
     * However, this means that we should not assume the bitmap size is given by the btree maximum
     * leaf page size but get it from the reconciliation info.
     *
     * Note: it is important to use *this* chunk's auxiliary start offset (passed in) and not read
     * the auxiliary start offset from the WT_RECONCILE, as we may be writing the previous chunk and
     * the latter describes the current chunk.
     */

    /* Figure how much primary data we have. */
    bitmapsize = __bitstr_size(entries * btree->bitcnt);

    /* The auxiliary header goes after the bitmap, which goes after the page header. */
    auxheaderoffset = WT_PAGE_HEADER_BYTE_SIZE(btree) + bitmapsize;

    /* This should also have left sufficient room for the header. */
    WT_ASSERT(session, aux_start_offset >= auxheaderoffset + WT_COL_FIX_AUXHEADER_RESERVATION);

    /*
     * If there is no auxiliary data, we will have already shortened the image size to discard the
     * auxiliary section and the auxiliary section should be past the end. In this case, skip the
     * header. This writes a page compatible with earlier versions. On odd-sized pages, e.g. the
     * last page in the tree, this also avoids the space wastage described above.
     */
    if (auxentries == 0) {
        WT_ASSERT(session, aux_start_offset >= size);
        return;
    }

    /* The offset we're going to write is the distance from the header start to the data. */
    offset = aux_start_offset - auxheaderoffset;

    /*
     * Encoding the offset should fit -- either it is less than what encodes to 1 byte or greater
     * than or equal to the maximum header size. This works out to asserting that the latter is less
     * than the maximum 1-byte-encoded integer. That in turn is a static condition.
     *
     * This in turn guarantees that the pack calls cannot fail.
     */
    WT_STATIC_ASSERT(WT_COL_FIX_AUXHEADER_SIZE_MAX < POS_1BYTE_MAX);

    p = image + auxheaderoffset;
    endp = image + aux_start_offset;

    *(p++) = WT_COL_FIX_VERSION_TS;
    WT_IGNORE_RET(__wt_vpack_uint(&p, WT_PTRDIFF32(endp, p), auxentries));
    WT_IGNORE_RET(__wt_vpack_uint(&p, WT_PTRDIFF32(endp, p), offset));
    WT_ASSERT(session, p <= endp);

    /* Zero the empty space, if any. */
    space = WT_PTRDIFF32(endp, p);
    if (space > 0)
        memset(p, 0, space);
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
        WT_RET(__wt_rec_split_crossing_bnd(session, r, val->len));

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

    WT_RET(
      __wt_rec_split_init(session, r, page, pageref->ref_recno, btree->maxleafpage_precomp, 0));

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
                    __wt_value_return(cbt, cbt->upd_value);
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
                    __wt_value_return(cbt, cbt->upd_value);
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
