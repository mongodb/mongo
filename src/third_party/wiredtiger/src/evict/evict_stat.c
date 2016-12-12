/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __evict_stat_walk --
 *	Walk all the pages in cache for a dhandle gathering stats information
 */
static void
__evict_stat_walk(WT_SESSION_IMPL *session)
{
	WT_BTREE *btree;
	WT_PAGE *page;
	WT_REF *next_walk;
	uint64_t dsk_size, gen_gap, size;
	uint64_t written_size_cnt, written_size_sum;
	uint64_t gen_gap_cnt, gen_gap_max, gen_gap_sum;
	uint64_t max_pagesize, min_written_size;
	uint64_t num_memory, num_queued, num_not_queueable, num_smaller_allocsz;
	uint64_t pages_clean, pages_dirty, pages_internal, pages_leaf;
	uint64_t seen_count, walk_count;

	btree = S2BT(session);
	next_walk = NULL;
	written_size_cnt = written_size_sum = 0;
	gen_gap_cnt = gen_gap_max = gen_gap_sum = 0;
	max_pagesize = 0;
	num_memory = num_queued = num_not_queueable = num_smaller_allocsz = 0;
	pages_clean = pages_dirty = pages_internal = pages_leaf = 0;
	seen_count = walk_count = 0;
	min_written_size = UINT64_MAX;

	while (__wt_tree_walk_count(session, &next_walk, &walk_count,
	    WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_WAIT) == 0 &&
	    next_walk != NULL) {
		++seen_count;
		page = next_walk->page;
		size = page->memory_footprint;

		if (__wt_page_is_modified(page))
			++pages_dirty;
		else
			++pages_clean;

		if (!__wt_ref_is_root(next_walk) &&
		    !__wt_page_can_evict(session, next_walk, NULL))
			++num_not_queueable;

		if (F_ISSET_ATOMIC(page, WT_PAGE_EVICT_LRU))
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

		if (WT_PAGE_IS_INTERNAL(page))
			++pages_internal;
		else
			++pages_leaf;

		/* Skip root pages since they are never considered */
		if (__wt_ref_is_root(next_walk))
			continue;

		gen_gap =
		    S2C(session)->cache->evict_pass_gen - page->evict_pass_gen;
		if (gen_gap > gen_gap_max)
			gen_gap_max = gen_gap;
		gen_gap_sum += gen_gap;
		++gen_gap_cnt;
	}

	WT_STAT_DATA_SET(session, cache_state_avg_written_size,
	    written_size_cnt == 0 ? 0 : written_size_sum / written_size_cnt);
	WT_STAT_DATA_SET(session, cache_state_gen_avg_gap,
	    gen_gap_cnt == 0 ? 0 : gen_gap_sum / gen_gap_cnt);

	WT_STAT_DATA_SET(session, cache_state_gen_max_gap, gen_gap_max);
	WT_STAT_DATA_SET(session, cache_state_max_pagesize, max_pagesize);
	WT_STAT_DATA_SET(session,
	    cache_state_min_written_size, min_written_size);
	WT_STAT_DATA_SET(session, cache_state_memory, num_memory);
	WT_STAT_DATA_SET(session, cache_state_queued, num_queued);
	WT_STAT_DATA_SET(session, cache_state_not_queueable, num_not_queueable);
	WT_STAT_DATA_SET(session,
	    cache_state_smaller_alloc_size, num_smaller_allocsz);
	WT_STAT_DATA_SET(session, cache_state_pages, walk_count);
	WT_STAT_DATA_SET(session, cache_state_pages_clean, pages_clean);
	WT_STAT_DATA_SET(session, cache_state_pages_dirty, pages_dirty);
	WT_STAT_DATA_SET(session, cache_state_pages_internal, pages_internal);
	WT_STAT_DATA_SET(session, cache_state_pages_leaf, pages_leaf);
	WT_STAT_DATA_SET(session,
	    cache_state_refs_skipped, walk_count - seen_count);
}

/*
 * __wt_curstat_cache_walk --
 *	Initialize the statistics for a cache cache_walk pass.
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
	WT_STAT_DATA_SET(session,
	    cache_state_gen_current, conn->cache->evict_pass_gen);

	/* Root page statistics */
	root_idx = WT_INTL_INDEX_GET_SAFE(btree->root.page);
	WT_STAT_DATA_SET(session,
	    cache_state_root_entries, root_idx->entries);
	WT_STAT_DATA_SET(session,
	    cache_state_root_size, btree->root.page->memory_footprint);

	WT_WITH_HANDLE_LIST_LOCK(session, __evict_stat_walk(session));
}
