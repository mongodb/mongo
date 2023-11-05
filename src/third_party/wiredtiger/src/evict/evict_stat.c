/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __evict_stat_walk --
 *     Walk all the pages in cache for a dhandle gathering stats information
 */
static void
__evict_stat_walk(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_PAGE *page;
    WT_REF *next_walk;
    uint64_t dsk_size, gen_gap, gen_gap_max, gen_gap_sum, max_pagesize;
    uint64_t min_written_size, num_memory, num_not_queueable, num_queued;
    uint64_t num_smaller_allocsz, pages_clean, pages_dirty, pages_internal;
    uint64_t pages_leaf, seen_count, size, visited_count;
    uint64_t visited_age_gap_sum, unvisited_count, unvisited_age_gap_sum;
    uint64_t walk_count, written_size_cnt, written_size_sum;

    btree = S2BT(session);
    cache = S2C(session)->cache;
    gen_gap_max = gen_gap_sum = max_pagesize = 0;
    num_memory = num_not_queueable = num_queued = 0;
    num_smaller_allocsz = pages_clean = pages_dirty = pages_internal = 0;
    pages_leaf = seen_count = size = visited_count = 0;
    visited_age_gap_sum = unvisited_count = unvisited_age_gap_sum = 0;
    walk_count = written_size_cnt = written_size_sum = 0;
    min_written_size = UINT64_MAX;

    next_walk = NULL;
    while (__wt_tree_walk_count(session, &next_walk, &walk_count,
             WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT) == 0 &&
      next_walk != NULL) {
        ++seen_count;
        page = next_walk->page;
        size = page->memory_footprint;

        if (__wt_page_is_modified(page))
            ++pages_dirty;
        else
            ++pages_clean;

        if (!__wt_ref_is_root(next_walk) && !__wt_page_can_evict(session, next_walk, NULL))
            ++num_not_queueable;

        if (F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU))
            ++num_queued;

        if (size > max_pagesize)
            max_pagesize = size;

        dsk_size = page->dsk != NULL ? page->dsk->mem_size : 0;
        if (dsk_size != 0) {
            if (dsk_size < btree->allocsize)
                ++num_smaller_allocsz;
            if (dsk_size < min_written_size)
                min_written_size = dsk_size;
            ++written_size_cnt;
            written_size_sum += dsk_size;
        } else
            ++num_memory;

        if (F_ISSET(next_walk, WT_REF_FLAG_INTERNAL))
            ++pages_internal;
        else
            ++pages_leaf;

        /* Skip root pages since they are never considered */
        if (__wt_ref_is_root(next_walk))
            continue;

        if (page->evict_pass_gen == 0) {
            unvisited_age_gap_sum += (cache->evict_pass_gen - page->cache_create_gen);
            ++unvisited_count;
        } else {
            visited_age_gap_sum += (cache->evict_pass_gen - page->cache_create_gen);
            gen_gap = cache->evict_pass_gen - page->evict_pass_gen;
            if (gen_gap > gen_gap_max)
                gen_gap_max = gen_gap;
            gen_gap_sum += gen_gap;
            ++visited_count;
        }
    }

    WT_STAT_DATA_SET(
      session, cache_state_gen_avg_gap, visited_count == 0 ? 0 : gen_gap_sum / visited_count);
    WT_STAT_DATA_SET(session, cache_state_avg_unvisited_age,
      unvisited_count == 0 ? 0 : unvisited_age_gap_sum / unvisited_count);
    WT_STAT_DATA_SET(session, cache_state_avg_visited_age,
      visited_count == 0 ? 0 : visited_age_gap_sum / visited_count);
    WT_STAT_DATA_SET(session, cache_state_avg_written_size,
      written_size_cnt == 0 ? 0 : written_size_sum / written_size_cnt);
    WT_STAT_DATA_SET(session, cache_state_gen_max_gap, gen_gap_max);
    WT_STAT_DATA_SET(session, cache_state_max_pagesize, max_pagesize);
    WT_STAT_DATA_SET(session, cache_state_min_written_size, min_written_size);
    WT_STAT_DATA_SET(session, cache_state_memory, num_memory);
    WT_STAT_DATA_SET(session, cache_state_queued, num_queued);
    WT_STAT_DATA_SET(session, cache_state_not_queueable, num_not_queueable);
    WT_STAT_DATA_SET(session, cache_state_pages, walk_count);
    WT_STAT_DATA_SET(session, cache_state_pages_clean, pages_clean);
    WT_STAT_DATA_SET(session, cache_state_pages_dirty, pages_dirty);
    WT_STAT_DATA_SET(session, cache_state_pages_internal, pages_internal);
    WT_STAT_DATA_SET(session, cache_state_pages_leaf, pages_leaf);
    WT_STAT_DATA_SET(session, cache_state_refs_skipped, walk_count - seen_count);
    WT_STAT_DATA_SET(session, cache_state_smaller_alloc_size, num_smaller_allocsz);
    WT_STAT_DATA_SET(session, cache_state_unvisited_count, unvisited_count);
}

/*
 * __wt_curstat_cache_walk --
 *     Initialize the statistics for a cache cache_walk pass.
 */
void
__wt_curstat_cache_walk(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_PAGE_INDEX *root_idx;

    btree = S2BT(session);
    conn = S2C(session);

    /* Set statistics that don't require walking the cache. */
    WT_STAT_DATA_SET(session, cache_state_gen_current, conn->cache->evict_pass_gen);

    /* Root page statistics */
    root_idx = WT_INTL_INDEX_GET_SAFE(btree->root.page);
    WT_STAT_DATA_SET(session, cache_state_root_entries, root_idx->entries);
    WT_STAT_DATA_SET(session, cache_state_root_size, btree->root.page->memory_footprint);

    __evict_stat_walk(session);
}
