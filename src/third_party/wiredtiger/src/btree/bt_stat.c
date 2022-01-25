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
static void __stat_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_col_var(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_row_int(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);
static void __stat_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *, WT_DSRC_STATS **);

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

    btree = S2BT(session);
    bm = btree->bm;
    stats = btree->dhandle->stats;

    WT_RET(bm->stat(bm, session, stats[0]));

    WT_STAT_SET(session, stats, btree_fixed_len, btree->bitcnt);
    WT_STAT_SET(session, stats, btree_maximum_depth, btree->maximum_depth);
    WT_STAT_SET(session, stats, btree_maxintlpage, btree->maxintlpage);
    WT_STAT_SET(session, stats, btree_maxleafkey, btree->maxleafkey);
    WT_STAT_SET(session, stats, btree_maxleafpage, btree->maxleafpage);
    WT_STAT_SET(session, stats, btree_maxleafvalue, btree->maxleafvalue);
    WT_STAT_SET(session, stats, rec_multiblock_max, btree->rec_multiblock_max);

    WT_STAT_SET(session, stats, cache_bytes_dirty, __wt_btree_dirty_inuse(session));
    WT_STAT_SET(session, stats, cache_bytes_dirty_total,
      __wt_cache_bytes_plus_overhead(S2C(session)->cache, btree->bytes_dirty_total));
    WT_STAT_SET(session, stats, cache_bytes_inuse, __wt_btree_bytes_inuse(session));

    WT_STAT_SET(session, stats, compress_precomp_leaf_max_page_size, btree->maxleafpage_precomp);
    WT_STAT_SET(session, stats, compress_precomp_intl_max_page_size, btree->maxintlpage_precomp);

    if (F_ISSET(cst, WT_STAT_TYPE_CACHE_WALK))
        __wt_curstat_cache_walk(session);

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

    btree = S2BT(session);
    stats = btree->dhandle->stats;

    /*
     * Clear the statistics we're about to count.
     */
    WT_STAT_SET(session, stats, btree_column_deleted, 0);
    WT_STAT_SET(session, stats, btree_column_fix, 0);
    WT_STAT_SET(session, stats, btree_column_internal, 0);
    WT_STAT_SET(session, stats, btree_column_rle, 0);
    WT_STAT_SET(session, stats, btree_column_variable, 0);
    WT_STAT_SET(session, stats, btree_entries, 0);
    WT_STAT_SET(session, stats, btree_overflow, 0);
    WT_STAT_SET(session, stats, btree_row_internal, 0);
    WT_STAT_SET(session, stats, btree_row_leaf, 0);

    next_walk = NULL;
    while (
      (ret = __wt_tree_walk(session, &next_walk, WT_READ_VISIBLE_ALL)) == 0 && next_walk != NULL) {
        WT_WITH_PAGE_INDEX(session, ret = __stat_page(session, next_walk->page, stats));
        WT_RET(ret);
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
    case WT_PAGE_COL_FIX:
        __stat_page_col_fix(session, page, stats);
        break;
    case WT_PAGE_COL_INT:
        WT_STAT_INCR(session, stats, btree_column_internal);
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
 * __stat_page_col_fix --
 *     Stat a WT_PAGE_COL_FIX page.
 */
static void
__stat_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page, WT_DSRC_STATS **stats)
{
    WT_CELL *cell;
    WT_CELL_UNPACK_KV unpack;
    WT_INSERT *ins;
    uint32_t numtws, stat_entries, stat_tws, tw;

    WT_STAT_INCR(session, stats, btree_column_fix);

    /*
     * Iterate the page to count time windows. For now at least, don't try to reason about whether
     * any particular update chain will result in an on-page timestamp after the next
     * reconciliation; this is complicated at best and also subject to change as the system runs.
     * There's accordingly no need to look at the update list.
     */
    stat_tws = 0;
    numtws = WT_COL_FIX_TWS_SET(page) ? page->pg_fix_numtws : 0;
    for (tw = 0; tw < numtws; tw++) {
        /* Unpack in case the time window becomes empty. */
        cell = WT_COL_FIX_TW_CELL(page, &page->pg_fix_tws[tw]);
        __wt_cell_unpack_kv(session, page->dsk, cell, &unpack);

        if (!WT_TIME_WINDOW_IS_EMPTY(&unpack.tw))
            stat_tws++;
    }

    /* Visit the append list to count the full number of entries on the page. */
    stat_entries = page->entries;
    WT_SKIP_FOREACH (ins, WT_COL_APPEND(page))
        stat_entries++;

    WT_STAT_INCRV(session, stats, btree_column_tws, stat_tws);
    WT_STAT_INCRV(session, stats, btree_entries, stat_entries);
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

    WT_STAT_INCR(session, stats, btree_column_variable);

    /*
     * Walk the page counting regular items, adjusting if the item has been subsequently deleted or
     * not. This is a mess because 10-item RLE might have 3 of the items subsequently deleted.
     * Overflow items are harder, we can't know if an updated item will be an overflow item or not;
     * do our best, and simply count every overflow item (or RLE set of items) we see.
     */
    WT_COL_FOREACH (page, cip, i) {
        cell = WT_COL_PTR(page, cip);
        __wt_cell_unpack_kv(session, page->dsk, cell, unpack);
        if (unpack->type == WT_CELL_DEL) {
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
            ++deleted_cnt;
            break;
        }

    WT_STAT_INCRV(session, stats, btree_column_deleted, deleted_cnt);
    WT_STAT_INCRV(session, stats, btree_column_rle, rle_cnt);
    WT_STAT_INCRV(session, stats, btree_entries, entry_cnt);
    WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
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

    WT_STAT_INCR(session, stats, btree_row_internal);

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

    WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
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

    WT_STAT_INCR(session, stats, btree_row_leaf);

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

    WT_STAT_INCRV(session, stats, btree_row_empty_values, empty_values);
    WT_STAT_INCRV(session, stats, btree_entries, entry_cnt);
    WT_STAT_INCRV(session, stats, btree_overflow, ovfl_cnt);
}
