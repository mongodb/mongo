/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * __rec_cell_addr_stats --
 *     Track statistics for time values associated with an address.
 */
static WT_INLINE void
__rec_cell_addr_stats(WTI_RECONCILE *r, WT_TIME_AGGREGATE *ta)
{
    if (ta->newest_start_durable_ts != WT_TS_NONE)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_START_DURABLE_TS);
    if (ta->newest_stop_durable_ts != WT_TS_NONE)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_DURABLE_TS);
    if (ta->oldest_start_ts != WT_TS_NONE)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_OLDEST_START_TS);
    if (ta->newest_txn != WT_TXN_NONE)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_TXN);
    if (ta->newest_stop_ts != WT_TS_MAX)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_TS);
    if (ta->newest_stop_txn != WT_TXN_MAX)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_TXN);
    if (ta->prepare != 0)
        FLD_SET(r->ts_usage_flags, WTI_REC_TIME_PREPARE);
}

/*
 * __rec_cell_tw_stats --
 *     Gather statistics about this cell.
 */
static WT_INLINE void
__rec_cell_tw_stats(WTI_RECONCILE *r, WT_TIME_WINDOW *tw)
{
    if (tw->durable_start_ts != WT_TS_NONE)
        ++r->count_durable_start_ts;
    if (tw->start_ts != WT_TS_NONE)
        ++r->count_start_ts;
    if (tw->start_txn != WT_TXN_NONE)
        ++r->count_start_txn;
    if (tw->durable_stop_ts != WT_TS_NONE)
        ++r->count_durable_stop_ts;
    if (tw->stop_ts != WT_TS_MAX)
        ++r->count_stop_ts;
    if (tw->stop_txn != WT_TXN_MAX)
        ++r->count_stop_txn;
    if (WT_TIME_WINDOW_HAS_PREPARE(tw))
        ++r->count_prepare;
}

/*
 * __rec_page_time_stats_clear --
 *     Clear page statistics.
 */
static WT_INLINE void
__rec_page_time_stats_clear(WTI_RECONCILE *r)
{
    r->count_durable_start_ts = 0;
    r->count_start_ts = 0;
    r->count_start_txn = 0;
    r->count_durable_stop_ts = 0;
    r->count_stop_ts = 0;
    r->count_stop_txn = 0;
    r->count_prepare = 0;

    r->ts_usage_flags = 0;
}

/*
 * __rec_page_delta_stats_clear --
 *     Clear page delta statistics.
 */
static WT_INLINE void
__rec_page_delta_stats_clear(WTI_RECONCILE *r)
{
    r->count_internal_page_delta_key_deleted = 0;
    r->count_internal_page_delta_key_updated = 0;
}

/*
 * __rec_page_time_stats --
 *     Update statistics about this page.
 */
static WT_INLINE void
__rec_page_time_stats(WT_SESSION_IMPL *session, WTI_RECONCILE *r)
{
    /* Time window statistics */
    if (r->count_durable_start_ts != 0) {
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_durable_start_ts);
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_ts, r->count_durable_start_ts * sizeof(wt_timestamp_t));
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_durable_start_ts, r->count_durable_start_ts);
        r->rec_page_cell_with_ts = true;
    }
    if (r->count_start_ts != 0) {
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_ts, r->count_start_ts * sizeof(wt_timestamp_t));
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_start_ts, r->count_start_ts);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_start_ts);
        r->rec_page_cell_with_ts = true;
    }
    if (r->count_start_txn != 0) {
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_txn, r->count_start_txn * sizeof(uint64_t));
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_start_txn, r->count_start_txn);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_start_txn);
        r->rec_page_cell_with_txn_id = true;
    }
    if (r->count_durable_stop_ts != 0) {
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_ts, r->count_durable_stop_ts * sizeof(wt_timestamp_t));
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_durable_stop_ts, r->count_durable_stop_ts);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_durable_stop_ts);
        r->rec_page_cell_with_ts = true;
    }
    if (r->count_stop_ts != 0) {
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_ts, r->count_stop_ts * sizeof(wt_timestamp_t));
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_stop_ts, r->count_stop_ts);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_stop_ts);
        r->rec_page_cell_with_ts = true;
    }
    if (r->count_stop_txn != 0) {
        WT_STAT_CONN_DSRC_INCRV(
          session, rec_time_window_bytes_txn, r->count_stop_txn * sizeof(uint64_t));
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_stop_txn, r->count_stop_txn);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_stop_txn);
        r->rec_page_cell_with_txn_id = true;
    }

    if (r->count_prepare != 0) {
        WT_STAT_CONN_DSRC_INCRV(session, rec_time_window_prepared, r->count_prepare);
        WT_STAT_CONN_DSRC_INCR(session, rec_time_window_pages_prepared);
        r->rec_page_cell_with_prepared_txn = true;
    }

    /* Time aggregate statistics */
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_START_DURABLE_TS))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_newest_start_durable_ts);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_DURABLE_TS))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_newest_stop_durable_ts);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_OLDEST_START_TS))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_oldest_start_ts);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_TXN))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_newest_txn);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_TS))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_newest_stop_ts);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_NEWEST_STOP_TXN))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_newest_stop_txn);
    if (FLD_ISSET(r->ts_usage_flags, WTI_REC_TIME_PREPARE))
        WT_STAT_CONN_DSRC_INCR(session, rec_time_aggr_prepared);
}

/*
 * __wti_rec_need_split --
 *     Check whether adding some bytes to the page requires a split.
 */
static WT_INLINE bool
__wti_rec_need_split(WTI_RECONCILE *r, size_t len)
{
    uint32_t page_items;

    /* We cannot split a page that is restored from deltas. */
    if (F_ISSET(r, WT_REC_REWRITE_DELTA))
        return (false);

    page_items = r->entries + r->supd_onpage_or_restore;

    /*
     * In the case of a row-store leaf page, we want to encourage a split if we see lots of
     * in-memory content. This allows pages to be split for update/restore and history store
     * eviction even when the disk image itself isn't growing.
     *
     * Make sure that there are a reasonable number of items (entries on the disk image or saved
     * updates) associated with the page. If there are barely any items on the page, then it's not
     * worth splitting. We also want to temper this effect to avoid in-memory updates from
     * dominating the calculation and causing excessive splitting. Therefore, we'll limit the impact
     * to a tenth of the cache usage occupied by those updates. For small updates the overhead of
     * the update structure could skew the calculation, so we subtract the overhead before
     * considering the cache usage by the updates.
     */
    if (r->page->type == WT_PAGE_ROW_LEAF && page_items > WTI_REC_SPLIT_MIN_ITEMS_USE_MEM)
        len += (r->supd_memsize - ((size_t)r->supd_onpage_or_restore * WT_UPDATE_SIZE)) / 10;

    /* Check for the disk image crossing a boundary. */
    return (WTI_CHECK_CROSSING_BND(r, len));
}

/*
 * __wti_rec_incr --
 *     Update the memory tracking structure for a set of new entries.
 */
static WT_INLINE void
__wti_rec_incr(WT_SESSION_IMPL *session, WTI_RECONCILE *r, uint32_t v, size_t size)
{
    /*
     * The buffer code is fragile and prone to off-by-one errors -- check for overflow in diagnostic
     * mode.
     */
    WT_ASSERT(session, r->space_avail >= size);
    WT_ASSERT(session,
      WT_BLOCK_FITS(r->first_free, size, r->cur_ptr->image.mem, r->cur_ptr->image.memsize));

    r->entries += v;
    r->space_avail -= size;
    r->first_free += size;

    /*
     * If offset for the minimum split size boundary is not set, we have not yet reached the minimum
     * boundary, reduce the space available for it.
     */
    if (r->cur_ptr->min_offset == 0) {
        if (r->min_space_avail >= size)
            r->min_space_avail -= size;
        else
            r->min_space_avail = 0;
    }
}

/*
 * __wti_rec_kv_copy --
 *     Copy a key/value cell and buffer pair. FIXME-WT-14887: ensure memory safety on the pointer.
 */
static WT_INLINE void
__wti_rec_kv_copy(WT_SESSION_IMPL *session, uint8_t *p, WTI_REC_KV *kv)
{
    size_t len;
    uint8_t *t;

    /*
     * If there's only one chunk of data to copy (because the cell and data are being copied from
     * the original disk page), the cell length won't be set, the WT_ITEM data/length will reference
     * the data to be copied.
     *
     * WT_CELLs are typically small, 1 or 2 bytes -- don't call memcpy, do the copy in-line.
     */
    for (t = (uint8_t *)&kv->cell, len = kv->cell_len; len > 0; --len)
        *p++ = *t++;

    /* The data can be quite large -- call memcpy. */
    if (kv->buf.size != 0)
        memcpy(p, kv->buf.data, kv->buf.size);

    WT_ASSERT(session, kv->len == kv->cell_len + kv->buf.size);
}

/*
 * __wti_rec_image_copy --
 *     Copy a key/value cell and buffer pair into the new image.
 */
static WT_INLINE void
__wti_rec_image_copy(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WTI_REC_KV *kv)
{
    __wti_rec_kv_copy(session, r->first_free, kv);
    __wti_rec_incr(session, r, 1, kv->len);
}

/*
 * __rec_auxincr --
 *     Update the memory tracking structure for a set of new entries in the auxiliary image.
 */
static WT_INLINE void
__rec_auxincr(WT_SESSION_IMPL *session, WTI_RECONCILE *r, uint32_t v, size_t size)
{
    /*
     * The buffer code is fragile and prone to off-by-one errors -- check for overflow in diagnostic
     * mode.
     */
    WT_ASSERT(session, r->aux_space_avail >= size);
    WT_ASSERT(session,
      WT_BLOCK_FITS(r->aux_first_free, size, r->cur_ptr->image.mem, r->cur_ptr->image.memsize));

    r->aux_entries += v;
    r->aux_space_avail -= size;
    r->aux_first_free += size;
}

/*
 * __wti_rec_auximage_copy --
 *     Copy a key/value cell and buffer pair into the new auxiliary image.
 */
static WT_INLINE void
__wti_rec_auximage_copy(WT_SESSION_IMPL *session, WTI_RECONCILE *r, uint32_t count, WTI_REC_KV *kv)
{
    size_t len;
    uint8_t *p;
    const uint8_t *t;

    /* Make sure we didn't run out of space. */
    WT_ASSERT(session, kv->len <= r->aux_space_avail);

    /*
     * If there's only one chunk of data to copy (because the cell and data are being copied from
     * the original disk page), the cell length won't be set, the WT_ITEM data/length will reference
     * the data to be copied.
     *
     * WT_CELLs are typically small, 1 or 2 bytes -- don't call memcpy, do the copy in-line.
     */
    for (p = r->aux_first_free, t = (const uint8_t *)&kv->cell, len = kv->cell_len; len > 0; --len)
        *p++ = *t++;

    /* Here the data is also small, when not entirely empty. */
    if (kv->buf.size != 0)
        for (t = (const uint8_t *)kv->buf.data, len = kv->buf.size; len > 0; --len)
            *p++ = *t++;

    WT_ASSERT(session, kv->len == kv->cell_len + kv->buf.size);
    __rec_auxincr(session, r, count, kv->len);
}

/*
 * __wti_rec_cell_build_addr --
 *     Process an address or unpack reference and return a cell structure to be stored on the page.
 */
static WT_INLINE void
__wti_rec_cell_build_addr(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_ADDR *addr,
  WT_CELL_UNPACK_ADDR *vpack, uint64_t recno, WT_PAGE_DELETED *page_del)
{
    WTI_REC_KV *val;
    WT_TIME_AGGREGATE *ta;
    u_int cell_type;

    val = &r->v;

    /*
     * Caller includes fast-delete information in the case of fast-delete proxy cells, which both
     * flags the fast-delete case and provides the additional information written in the parent's
     * address cell.
     */
    if (vpack == NULL) {
        switch (addr->type) {
        case WT_ADDR_INT:
            cell_type = WT_CELL_ADDR_INT;
            break;
        case WT_ADDR_LEAF:
            cell_type = WT_CELL_ADDR_LEAF;
            break;
        case WT_ADDR_LEAF_NO:
        default:
            cell_type = WT_CELL_ADDR_LEAF_NO;
            break;
        }
        WT_ASSERT(session, addr->block_cookie_size != 0);
        ta = &addr->ta;
    } else {
        cell_type = vpack->type;
        ta = &vpack->ta;
    }
    __rec_cell_addr_stats(r, ta);

    /*
     * If passed fast-delete information, override the cell type. We should never see fast-truncate
     * cell types without fast-truncate information.
     */
    WT_ASSERT(session, page_del != NULL || cell_type != WT_CELL_ADDR_DEL);
    if (page_del != NULL) {
        /*
         * We only fast-truncate leaf pages without overflow items, however, we can write a proxy
         * cell for a page, evict and then read the internal page, and then checkpoint is writing it
         * again.
         */
        WT_ASSERT(session, cell_type == WT_CELL_ADDR_DEL || cell_type == WT_CELL_ADDR_LEAF_NO);
        cell_type = WT_CELL_ADDR_DEL;

        /* We should never be in an in-progress prepared state. */
        WT_ASSERT(session,
          page_del->prepare_state == WT_PREPARE_INIT ||
            page_del->prepare_state == WT_PREPARE_RESOLVED);
    }

    /*
     * We don't copy the data into the buffer, it's not necessary; just re-point the buffer's
     * data/length fields.
     */
    if (vpack == NULL) {
        WT_ASSERT(session, addr != NULL);
        val->buf.data = addr->block_cookie;
        val->buf.size = addr->block_cookie_size;
    } else {
        WT_ASSERT(session, addr == NULL);
        val->buf.data = vpack->data;
        val->buf.size = vpack->size;
    }

    val->cell_len =
      __wt_cell_pack_addr(session, &val->cell, cell_type, recno, page_del, ta, val->buf.size);
    val->len = val->cell_len + val->buf.size;
}

/*
 * __wti_rec_cell_build_val --
 *     Process a data item and return a WT_CELL structure and byte string to be stored on the page.
 */
static WT_INLINE int
__wti_rec_cell_build_val(WT_SESSION_IMPL *session, WTI_RECONCILE *r, const void *data, size_t size,
  WT_TIME_WINDOW *tw, uint64_t rle)
{
    WT_BTREE *btree;
    WTI_REC_KV *val;

    btree = S2BT(session);
    val = &r->v;

    /*
     * Unless necessary we don't copy the data into the buffer; start by just re-pointing the
     * buffer's data/length fields.
     */
    val->buf.data = data;
    val->buf.size = size;

    /* Create an overflow object if the data won't fit. */
    WT_ASSERT(session, btree->maxleafvalue > 0);
    if (val->buf.size > btree->maxleafvalue) {
        WT_STAT_CONN_DSRC_INCR(session, rec_overflow_value);

        return (__wti_rec_cell_build_ovfl(session, r, val, WT_CELL_VALUE_OVFL, tw, rle));
    }
    __rec_cell_tw_stats(r, tw);

    val->cell_len = __wt_cell_pack_value(session, &val->cell, tw, rle, val->buf.size);
    val->len = val->cell_len + val->buf.size;

    return (0);
}

/*
 * __wti_rec_dict_replace --
 *     Check for a dictionary match.
 */
static WT_INLINE int
__wti_rec_dict_replace(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_TIME_WINDOW *tw, uint64_t rle, WTI_REC_KV *val)
{
    WTI_REC_DICTIONARY *dp;
    uint64_t offset;

    /*
     * We optionally create a dictionary of values and only write a unique value once per page,
     * using a special "copy" cell for all subsequent copies of the value. We have to do the cell
     * build and resolution at this low level because we need physical cell offsets for the page.
     *
     * Sanity check: short-data cells can be smaller than dictionary-copy cells. If the data is
     * already small, don't bother doing the work. This isn't just work avoidance: on-page cells
     * can't grow as a result of writing a dictionary-copy cell, the reconciliation functions do a
     * split-boundary test based on the size required by the value's cell; if we grow the cell after
     * that test we'll potentially write off the end of the buffer's memory.
     */
    if (val->buf.size <= WT_INTPACK32_MAXSIZE)
        return (0);
    WT_RET(__wti_rec_dictionary_lookup(session, r, val, &dp));
    if (dp == NULL)
        return (0);

    /*
     * If the dictionary offset isn't set, we're creating a new entry in the dictionary, set its
     * location.
     *
     * If the dictionary offset is set, we have a matching value. Create a copy cell instead.
     */
    if (dp->offset == 0)
        dp->offset = WT_PTRDIFF32(r->first_free, r->cur_ptr->image.mem);
    else {
        /*
         * The offset is the byte offset from this cell to the previous, matching cell, NOT the byte
         * offset from the beginning of the page.
         */
        offset = (uint64_t)WT_PTRDIFF(r->first_free, (uint8_t *)r->cur_ptr->image.mem + dp->offset);
        val->len = val->cell_len = __wt_cell_pack_copy(session, &val->cell, tw, rle, offset);
        val->buf.data = NULL;
        val->buf.size = 0;
    }
    return (0);
}

/*
 * __wti_rec_time_window_clear_obsolete --
 *     Where possible modify time window values to avoid writing obsolete values to the cell later.
 */
static WT_INLINE void
__wti_rec_time_window_clear_obsolete(WT_SESSION_IMPL *session, WTI_UPDATE_SELECT *upd_select,
  WT_CELL_UNPACK_KV *vpack, WTI_RECONCILE *r)
{
    WT_TIME_WINDOW *tw;

    WT_ASSERT(
      session, (upd_select != NULL && vpack == NULL) || (upd_select == NULL && vpack != NULL));
    tw = upd_select != NULL ? &upd_select->tw : &vpack->tw;

    /* Return if the start time window is empty. */
    if (!WT_TIME_WINDOW_HAS_START(tw))
        return;

    /*
     * In memory database don't need to avoid writing values to the cell. If we remove this check we
     * create an extra update on the end of the chain later in reconciliation as we'll re-append the
     * disk image value to the update chain.
     */
    if (!WT_TIME_WINDOW_HAS_PREPARE(tw) && !F_ISSET(S2C(session), WT_CONN_IN_MEMORY) &&
      !F_ISSET(S2BT(session), WT_BTREE_IN_MEMORY)) {
        /*
         * Check if the start of the time window is globally visible, and if so remove unnecessary
         * values.
         */
        if (WTI_REC_TW_START_VISIBLE_ALL(r, tw)) {
            /* The durable timestamp should never be less than the start timestamp. */
            WT_ASSERT(session, tw->start_ts <= tw->durable_start_ts);

            tw->start_ts = tw->durable_start_ts = WT_TS_NONE;
            tw->start_txn = WT_TXN_NONE;

            /* Mark the cell with time window cleared flag to let the cell to be rebuild again. */
            if (vpack)
                F_SET(vpack, WT_CELL_UNPACK_TIME_WINDOW_CLEARED);
        }
    }
}
