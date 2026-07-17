/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __stat_tree_walk(WT_SESSION_IMPL *);
static int __stat_page(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_col_var(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_row_int(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __size_stat_hist_incr(WT_SESSION_IMPL *, size_t);
static int __size_stat_overflow(WT_SESSION_IMPL *, const uint8_t *, size_t, int);

/*
 * Leaf page-size histogram bucketing for the size summary. All but the last bucket are equal-width
 * slices of [0, leaf page max); the final bucket holds pages at or above the leaf page max. The
 * bucket count must match the number of btree_size_leaf_hist_N statistics.
 */
#define WT_SIZE_STAT_HIST_BUCKETS 9

/*
 * Overflow item kinds for the size summary, identifying what an overflow page's payload represents.
 * Only leaf key/value payloads are user data; internal-page overflow keys are tree overhead.
 */
#define WT_SIZE_STAT_OVFL_OVERHEAD 0
#define WT_SIZE_STAT_OVFL_KEY 1
#define WT_SIZE_STAT_OVFL_VALUE 2

/*
 * __wt_btree_stat_init --
 *     Initialize the Btree statistics.
 */
int
__wt_btree_stat_init(WT_SESSION_IMPL *session, WT_CURSOR_STAT *cst)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DSRC_STATS **stats;
    uint64_t avg_internal_chain, avg_leaf_chain;

    btree = S2BT(session);
    bm = btree->bm;
    stats = btree->dhandle->stats;

    WT_RET(bm->stat(bm, session, stats[0]));

    WT_STATP_DSRC_SET(session, stats, btree_fixed_len, btree->bitcnt);
    WT_STATP_DSRC_SET(session, stats, btree_maximum_depth, btree->maximum_depth);
    WT_STATP_DSRC_SET(session, stats, btree_maxintlpage, btree->maxintlpage);
    WT_STATP_DSRC_SET(session, stats, btree_maxleafkey, btree->maxleafkey);
    WT_STATP_DSRC_SET(session, stats, btree_maxleafpage, btree->maxleafpage);
    WT_STATP_DSRC_SET(session, stats, btree_maxleafvalue, btree->maxleafvalue);
    WT_STATP_DSRC_SET(session, stats, rec_multiblock_max, btree->rec_multiblock_max);

    WT_STATP_DSRC_SET(session, stats, cache_bytes_dirty, __wt_btree_dirty_inuse(session));
    WT_STATP_DSRC_SET(session, stats, cache_bytes_dirty_leaf, __wt_btree_dirty_leaf_inuse(session));
    WT_STATP_DSRC_SET(
      session, stats, cache_bytes_dirty_internal, __wt_btree_dirty_intl_inuse(session));
    WT_STATP_DSRC_SET(session, stats, cache_bytes_dirty_total,
      __wt_cache_bytes_plus_overhead(
        S2C(session)->cache, __wt_atomic_load_uint64_relaxed(&btree->bytes_dirty_total)));
    WT_STATP_DSRC_SET(session, stats, cache_bytes_inuse, __wt_btree_bytes_inuse(session));

    WT_STATP_DSRC_SET(
      session, stats, compress_precomp_leaf_max_page_size, btree->maxleafpage_precomp);
    WT_STATP_DSRC_SET(
      session, stats, compress_precomp_intl_max_page_size, btree->maxintlpage_precomp);

    avg_internal_chain = (uint64_t)WT_STAT_DSRC_READ(stats, rec_pages_with_internal_deltas) == 0 ?
      0 :
      (uint64_t)WT_STAT_DSRC_READ(stats, rec_page_delta_internal) /
        (uint64_t)WT_STAT_DSRC_READ(stats, rec_pages_with_internal_deltas);
    avg_leaf_chain = (uint64_t)WT_STAT_DSRC_READ(stats, rec_pages_with_leaf_deltas) == 0 ?
      0 :
      (uint64_t)WT_STAT_DSRC_READ(stats, rec_page_delta_leaf) /
        (uint64_t)WT_STAT_DSRC_READ(stats, rec_pages_with_leaf_deltas);
    WT_STATP_DSRC_SET(
      session, stats, rec_average_internal_page_delta_chain_length, avg_internal_chain);
    WT_STATP_DSRC_SET(session, stats, rec_average_leaf_page_delta_chain_length, avg_leaf_chain);

    /*
     * These read as WT_LEAF_STATS_UNKNOWN for a table whose checkpoint metadata predates this
     * tracking and hasn't yet had a corrective WT_STAT_TYPE_TREE_WALK; callers treat that reserved
     * marker as "unknown, fall back to another estimation technique or trigger a walk."
     */
    WT_STATP_DSRC_SET(session, stats, btree_row_leaf_avg_entries,
      __wt_atomic_load_uint64_relaxed(&btree->leaf_entry_ewma));
    WT_STATP_DSRC_SET(session, stats, btree_row_leaf_pages,
      __wt_atomic_load_uint64_relaxed(&btree->approx_leaf_pages));

    if (F_ISSET(cst, WT_STAT_TYPE_CACHE_WALK))
        __wt_evict_cache_stat_walk(session);

    if (F_ISSET(cst, WT_STAT_TYPE_TREE_WALK))
        WT_RET(__stat_tree_walk(session));

    return (0);
}

/*
 * __stat_tree_walk --
 *     Gather btree statistics that require traversing the tree.
 */
static int
__stat_tree_walk(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_DSRC_STATS **stats;
    WT_REF *next_walk;
    uint32_t walk_flags;

    btree = S2BT(session);
    stats = btree->dhandle->stats;

    /*
     * Clear the statistics we're about to count.
     */
    WT_STATP_DSRC_SET(session, stats, btree_column_deleted, 0);
    WT_STATP_DSRC_SET(session, stats, btree_column_internal, 0);
    WT_STATP_DSRC_SET(session, stats, btree_column_rle, 0);
    WT_STATP_DSRC_SET(session, stats, btree_column_variable, 0);
    WT_STATP_DSRC_SET(session, stats, btree_entries, 0);
    WT_STATP_DSRC_SET(session, stats, btree_overflow, 0);
    WT_STATP_DSRC_SET(session, stats, btree_row_internal, 0);
    WT_STATP_DSRC_SET(session, stats, btree_row_leaf, 0);

    next_walk = NULL;
    walk_flags = WT_READ_INTERNAL_OP | WT_READ_VISIBLE_ALL | WT_READ_WONT_NEED;
    if (F_ISSET(session, WT_SESSION_READ_SKIP_CORRUPT))
        FLD_SET(walk_flags, WT_READ_SKIP_CORRUPT);

    /*
     * Pages read for statistics aren't "useful"; don't update the read generation of pages already
     * in memory, and if a page is read, set its generation to a low value so it is evicted quickly.
     * Same as with compact.
     */
    while ((ret = __wt_tree_walk(session, &next_walk, walk_flags)) == 0 && next_walk != NULL) {
        WT_WITH_PAGE_INDEX(session, ret = __stat_page(session, next_walk->page, stats));
        WT_ERR(ret);
    }

err:
    WT_IGNORE_RET(__wt_page_release(session, next_walk, 0));
    /*
     * Correct approx_leaf_pages and leaf_entry_ewma to exact values from the walk. Skip on error: a
     * partial walk produces unreliable counts.
     */
    if ((ret == 0 || ret == WT_NOTFOUND) && btree->type == BTREE_ROW) {
        uint64_t exact = (uint64_t)WT_STAT_DSRC_READ(stats, btree_row_leaf);
        uint64_t exact_avg =
          exact > 0 ? (uint64_t)WT_STAT_DSRC_READ(stats, btree_entries) / exact : 0;

        __wt_atomic_store_uint64_relaxed(&btree->approx_leaf_pages, exact);
        WT_STATP_DSRC_SET(session, stats, btree_row_leaf_pages, exact);
        __wt_atomic_store_uint64_relaxed(&btree->leaf_entry_ewma, exact_avg);
        WT_STATP_DSRC_SET(session, stats, btree_row_leaf_avg_entries, exact_avg);
    }
    return (ret == WT_NOTFOUND ? 0 : ret);
}

/*
 * __stat_page --
 *     Stat any Btree page.
 */
static int
__stat_page(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
    /*
     * All internal pages and overflow pages are trivial, all we track is a count of the page type.
     */
    switch (page->type) {
    case WT_PAGE_COL_INT:
        WT_STATP_DSRC_INCR(session, stats, btree_column_internal);
        break;
    case WT_PAGE_COL_VAR:
        __stat_page_col_var(session, page, stats);
        break;
    case WT_PAGE_ROW_INT:
        __stat_page_row_int(session, page, stats);
        break;
    case WT_PAGE_ROW_LEAF:
        __stat_page_row_leaf(session, page, stats);
        break;
    default:
        return (__wt_illegal_value(session, page->type));
    }
    return (0);
}

/*
 * __stat_page_col_var --
 *     Stat a WT_PAGE_COL_VAR page.
 */
static void
__stat_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV *unpack, _unpack;
    WT_COL *cip;
    WT_INSERT *ins;
    uint64_t deleted_cnt, entry_cnt, ovfl_cnt, rle_cnt;
    uint32_t i;
    bool orig_deleted;

    unpack = &_unpack;
    deleted_cnt = entry_cnt = ovfl_cnt = rle_cnt = 0;

    WT_STATP_DSRC_INCR(session, stats, btree_column_variable);

    /*
     * Walk the page counting regular items, adjusting if the item has been subsequently deleted or
     * not. This is a mess because 10-item RLE might have 3 of the items subsequently deleted.
     * Overflow items are harder, we can't know if an updated item will be an overflow item or not;
     * do our best, and simply count every overflow item (or RLE set of items) we see.
     */
    WT_COL_FOREACH (page, cip, i) {
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(session, page->dsk, cell, unpack);
        /* A stop time point is a delete. */
        if (unpack->type == WT_CELL_DEL || WT_TIME_WINDOW_HAS_STOP(&unpack->tw)) {
            orig_deleted = true;
            deleted_cnt += __wt_cell_rle(unpack);
        } else {
            orig_deleted = false;
            entry_cnt += __wt_cell_rle(unpack);
        }
        rle_cnt += __wt_cell_rle(unpack) - 1;
        if (F_ISSET(unpack, WT_CELL_UNPACK_OVERFLOW))
            ++ovfl_cnt;

        /*
         * Walk the insert list, checking for changes. For each insert we find, correct the original
         * count based on its state.
         */
        WT_SKIP_FOREACH (ins, WT_COL_UPDATE(page, cip)) {
            switch (ins->upd->type) {
            case WT_UPDATE_MODIFY:
            case WT_UPDATE_STANDARD:
                if (orig_deleted) {
                    --deleted_cnt;
                    ++entry_cnt;
                }
                break;
            case WT_UPDATE_RESERVE:
                break;
            case WT_UPDATE_TOMBSTONE:
                if (!orig_deleted) {
                    ++deleted_cnt;
                    --entry_cnt;
                }
                break;
            }
        }
    }

    /* Walk any append list. */
    WT_SKIP_FOREACH (ins, WT_COL_APPEND(page))
        switch (ins->upd->type) {
        case WT_UPDATE_MODIFY:
        case WT_UPDATE_STANDARD:
            ++entry_cnt;
            break;
        case WT_UPDATE_RESERVE:
            break;
        case WT_UPDATE_TOMBSTONE:
            break;
        }

    WT_STATP_DSRC_INCRV(session, stats, btree_column_deleted, deleted_cnt);
    WT_STATP_DSRC_INCRV(session, stats, btree_column_rle, rle_cnt);
    WT_STATP_DSRC_INCRV(session, stats, btree_entries, entry_cnt);
    WT_STATP_DSRC_INCRV(session, stats, btree_overflow, ovfl_cnt);
}

/*
 * __stat_page_row_int --
 *     Stat a WT_PAGE_ROW_INT page.
 */
static void
__stat_page_row_int(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
    WT_CELL_UNPACK_ADDR unpack;
    uint32_t ovfl_cnt;

    ovfl_cnt = 0;

    WT_STATP_DSRC_INCR(session, stats, btree_row_internal);

    /*
     * Overflow keys are hard: we have to walk the disk image to count them, the in-memory
     * representation of the page doesn't necessarily contain a reference to the original cell.
     */
    if (page->dsk != NULL) {
        WT_CELL_FOREACH_ADDR (session, page->dsk, unpack) {
            if (__wt_cell_type(unpack.cell) == WT_CELL_KEY_OVFL)
                ++ovfl_cnt;
        }
        WT_CELL_FOREACH_END;
    }

    WT_STATP_DSRC_INCRV(session, stats, btree_overflow, ovfl_cnt);
}

/*
 * __stat_page_row_leaf --
 *     Stat a WT_PAGE_ROW_LEAF page.
 */
static void
__stat_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
    WT_CELL_UNPACK_KV unpack;
    WT_INSERT *ins;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint32_t empty_values, entry_cnt, i, ovfl_cnt;
    bool key;

    empty_values = entry_cnt = ovfl_cnt = 0;

    WT_STATP_DSRC_INCR(session, stats, btree_row_leaf);

    /*
     * Walk any K/V pairs inserted into the page before the first from-disk key on the page.
     */
    WT_SKIP_FOREACH (ins, WT_ROW_INSERT_SMALLEST(page))
        if (ins->upd->type != WT_UPDATE_RESERVE && ins->upd->type != WT_UPDATE_TOMBSTONE)
            ++entry_cnt;

    /*
     * Walk the page's K/V pairs. Count overflow values, where an overflow item is any on-disk
     * overflow item that hasn't been updated.
     */
    WT_ROW_FOREACH (page, rip, i) {
        upd = WT_ROW_UPDATE(page, rip);
        if (upd == NULL || (upd->type != WT_UPDATE_RESERVE && upd->type != WT_UPDATE_TOMBSTONE))
            ++entry_cnt;
        if (upd == NULL) {
            __wt_row_leaf_value_cell(session, page, rip, &unpack);
            if (unpack.type == WT_CELL_VALUE_OVFL)
                ++ovfl_cnt;
            /* A stop time point is a delete. */
            if (WT_TIME_WINDOW_HAS_STOP(&unpack.tw))
                --entry_cnt;
        }

        /* Walk K/V pairs inserted after the on-page K/V pair. */
        WT_SKIP_FOREACH (ins, WT_ROW_INSERT(page, rip))
            if (ins->upd->type != WT_UPDATE_RESERVE && ins->upd->type != WT_UPDATE_TOMBSTONE)
                ++entry_cnt;
    }

    /*
     * Overflow keys are hard: we have to walk the disk image to count them, the in-memory
     * representation of the page doesn't necessarily contain a reference to the original cell.
     *
     * Zero-length values are the same, we have to look at the disk image to know. They aren't
     * stored but we know they exist if there are two keys in a row, or a key as the last item.
     */
    if (page->dsk != NULL) {
        key = false;
        WT_CELL_FOREACH_KV (session, page->dsk, unpack) {
            switch (__wt_cell_type(unpack.cell)) {
            case WT_CELL_KEY_OVFL:
                ++ovfl_cnt;
            /* FALLTHROUGH */
            case WT_CELL_KEY:
                if (key)
                    ++empty_values;
                key = true;
                break;
            default:
                key = false;
                break;
            }
        }
        WT_CELL_FOREACH_END;
        if (key)
            ++empty_values;
    }

    WT_STATP_DSRC_INCRV(session, stats, btree_row_empty_values, empty_values);
    WT_STATP_DSRC_INCRV(session, stats, btree_entries, entry_cnt);
    WT_STATP_DSRC_INCRV(session, stats, btree_overflow, ovfl_cnt);
}

/*
 * __size_stat_hist_incr --
 *     Increment the leaf page-size histogram bucket for the size summary.
 */
static void
__size_stat_hist_incr(WT_SESSION_IMPL *session, size_t bucket)
{
    switch (bucket) {
    case 0:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_0);
        break;
    case 1:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_1);
        break;
    case 2:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_2);
        break;
    case 3:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_3);
        break;
    case 4:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_4);
        break;
    case 5:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_5);
        break;
    case 6:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_6);
        break;
    case 7:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_7);
        break;
    default:
        WT_STAT_DSRC_INCR(session, btree_size_leaf_hist_8);
        break;
    }
}

/*
 * __size_stat_overflow --
 *     Read an overflow page and account it against the size summary. The payload (datalen) counts
 *     as key or value data; the overflow page image counts as overhead.
 */
static int
__size_stat_overflow(WT_SESSION_IMPL *session, const uint8_t *addr, size_t addr_size, int kind)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    const WT_PAGE_HEADER *dsk;

    WT_RET(__wt_scr_alloc(session, 0, &tmp));
    WT_ERR(__wt_blkcache_read(session, tmp, NULL, addr, addr_size));

    dsk = tmp->data;
    if (dsk->type != WT_PAGE_OVFL)
        WT_ERR_MSG(session, WT_ERROR, "size summary: referenced page at %s is not an overflow page",
          __wt_addr_string(session, addr, addr_size, tmp));

    WT_STAT_DSRC_INCR(session, btree_size_overflow_pages);
    WT_STAT_DSRC_INCRV(session, btree_size_overflow_bytes, dsk->mem_size);
    if (kind == WT_SIZE_STAT_OVFL_KEY) {
        WT_STAT_DSRC_INCRV(session, btree_size_key_bytes, dsk->u.datalen);
        WT_STAT_DSRC_INCR(session, btree_size_key_count);
    } else if (kind == WT_SIZE_STAT_OVFL_VALUE) {
        WT_STAT_DSRC_INCRV(session, btree_size_value_bytes, dsk->u.datalen);
        WT_STAT_DSRC_INCR(session, btree_size_value_count);
    }

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}

/*
 * __wt_size_stat_reset --
 *     Clear the size-summary statistics ahead of a fresh accounting traversal. The counters live in
 *     the shared dhandle stats, so callers must not reset while another size_stats walk on this
 *     btree is still accumulating.
 */
void
__wt_size_stat_reset(WT_SESSION_IMPL *session)
{
    WT_DSRC_STATS **stats;

    if (session->dhandle == NULL || session->dhandle->stat_array == NULL)
        return;
    stats = session->dhandle->stats;

    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_pages, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_internal_pages, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_overflow_pages, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_bytes, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_internal_bytes, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_overflow_bytes, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_key_bytes, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_value_bytes, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_key_count, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_value_count, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_0, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_1, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_2, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_3, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_4, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_5, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_6, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_7, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_leaf_hist_8, 0);
    WT_STATP_DSRC_SET(session, stats, btree_size_no_image_pages, 0);
}

/*
 * __wti_size_stat_page --
 *     Accumulate one page into the size-summary statistics. Measured from the on-disk page image,
 *     so pages without one (dirty, in-memory only) are skipped. Key and value byte totals are
 *     row-store only.
 */
int
__wti_size_stat_page(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /* The size summary is a row-store-only feature; callers must not arm it on column store. */
    WT_ASSERT(session, S2BT(session)->type == BTREE_ROW);

    /*
     * Only account pages that have an on-disk image. A page built in memory and not yet read back
     * from disk has no image; count these so callers can quantify how much of the tree the summary
     * did not measure.
     */
    const WT_PAGE_HEADER *dsk = page->dsk;
    if (dsk == NULL) {
        WT_STAT_DSRC_INCR(session, btree_size_no_image_pages);
        return (0);
    }
    uint64_t page_mem = dsk->mem_size;

    if (WT_PAGE_IS_INTERNAL(page)) {
        WT_STAT_DSRC_INCR(session, btree_size_internal_pages);
        WT_STAT_DSRC_INCRV(session, btree_size_internal_bytes, page_mem);

        /*
         * Overflow keys referenced from an internal page are tree overhead. Match the raw type: a
         * removed overflow cell (WT_CELL_KEY_OVFL_RM) normalizes to WT_CELL_KEY_OVFL but its
         * backing blocks are already freed, so reading it would fault or corrupt the accounting.
         */
        WT_CELL_UNPACK_ADDR unpack_addr;
        WT_CELL_FOREACH_ADDR (session, dsk, unpack_addr) {
            if (unpack_addr.raw == WT_CELL_KEY_OVFL)
                WT_RET(__size_stat_overflow(
                  session, unpack_addr.data, unpack_addr.size, WT_SIZE_STAT_OVFL_OVERHEAD));
        }
        WT_CELL_FOREACH_END;
        return (0);
    }

    WT_STAT_DSRC_INCR(session, btree_size_leaf_pages);
    WT_STAT_DSRC_INCRV(session, btree_size_leaf_bytes, page_mem);

    /*
     * Bucket the leaf by uncompressed size: equal-width slices of [0, leaf page max), with the
     * final bucket for pages at or above the configured maximum.
     */
    uint32_t bucket_width = S2BT(session)->maxleafpage / (WT_SIZE_STAT_HIST_BUCKETS - 1);
    size_t hist_bucket;
    if (bucket_width == 0)
        hist_bucket = 0;
    else
        hist_bucket = (size_t)WT_MIN(page_mem / bucket_width, WT_SIZE_STAT_HIST_BUCKETS - 1);
    __size_stat_hist_incr(session, hist_bucket);

    /*
     * Key and value bytes are row-store only. Keys are counted at their physical (on-page) length;
     * prefix compression is deliberately not resolved, matching what occupies the page image.
     * Overflow key/value payloads are pulled in from their referenced pages.
     *
     * A removed overflow cell (WT_CELL_{KEY,VALUE}_OVFL_RM) normalizes to WT_CELL_{KEY,VALUE}_OVFL
     * but its backing blocks are already freed; skip it via the raw type so we never read them. The
     * removed payload no longer occupies the tree, so there is nothing to attribute.
     */
    if (page->type == WT_PAGE_ROW_LEAF) {
        WT_CELL_UNPACK_KV unpack_kv;
        WT_CELL_FOREACH_KV (session, dsk, unpack_kv) {
            switch (unpack_kv.type) {
            case WT_CELL_KEY:
                WT_STAT_DSRC_INCRV(session, btree_size_key_bytes, unpack_kv.size);
                WT_STAT_DSRC_INCR(session, btree_size_key_count);
                break;
            case WT_CELL_VALUE:
                WT_STAT_DSRC_INCRV(session, btree_size_value_bytes, unpack_kv.size);
                WT_STAT_DSRC_INCR(session, btree_size_value_count);
                break;
            case WT_CELL_KEY_OVFL:
                if (unpack_kv.raw == WT_CELL_KEY_OVFL)
                    WT_RET(__size_stat_overflow(
                      session, unpack_kv.data, unpack_kv.size, WT_SIZE_STAT_OVFL_KEY));
                break;
            case WT_CELL_VALUE_OVFL:
                if (unpack_kv.raw == WT_CELL_VALUE_OVFL)
                    WT_RET(__size_stat_overflow(
                      session, unpack_kv.data, unpack_kv.size, WT_SIZE_STAT_OVFL_VALUE));
                break;
            }
        }
        WT_CELL_FOREACH_END;
    }

    return (0);
}
