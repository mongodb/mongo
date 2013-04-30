/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_stat_init --
 *	Initialize a LSM statistics structure.
 */
int
__wt_lsm_stat_init(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_CURSOR_STAT *cst, uint32_t flags)
{
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(uribuf);
	WT_DECL_RET;
	WT_DSRC_STATS *child, *stats;
	WT_LSM_CHUNK *chunk;
	u_int i;
	int locked;
	const char **p;
	const char *cfg[] = {
	    WT_CONFIG_BASE(session, session_open_cursor), NULL, NULL, NULL };
	const char *disk_cfg[] = {
	   WT_CONFIG_BASE(session, session_open_cursor),
	   "checkpoint=WiredTigerCheckpoint", NULL, NULL, NULL };

	locked = 0;

	/*
	 * If the upper-level statistics call is fast and/or clear, propagate
	 * that to the cursors we open.
	 */
	p = &cfg[1];
	if (LF_ISSET(WT_STATISTICS_CLEAR))
		*p++ = "statistics_clear";
	if (LF_ISSET(WT_STATISTICS_FAST))
		*p++ = "statistics_fast";

	p = &disk_cfg[1];
	if (LF_ISSET(WT_STATISTICS_CLEAR))
		*p++ = "statistics_clear";
	if (LF_ISSET(WT_STATISTICS_FAST))
		*p++ = "statistics_fast";

	/*
	 * Allocate an aggregated statistics structure, or clear any already
	 * allocated one.
	 */
	if ((stats = (WT_DSRC_STATS *)cst->stats) == NULL) {
		WT_ERR(__wt_calloc_def(session, 1, &stats));
		__wt_stat_init_dsrc_stats(stats);
		cst->stats_first = cst->stats = (WT_STATS *)stats;
		cst->stats_count = sizeof(*stats) / sizeof(WT_STATS);
	} else
		__wt_stat_clear_dsrc_stats(stats);

	/*
	 * Set some statistics that aren't aggregated, and clear the ones
	 * (counters) to which the clear flag applies.
	 */
	WT_STAT_SET(stats, lsm_chunk_count, lsm_tree->nchunks);
	WT_STAT_SET(stats, bloom_hit, lsm_tree->bloom_hit);
	WT_STAT_SET(stats, bloom_miss,lsm_tree->bloom_miss);
	WT_STAT_SET(
	    stats, bloom_false_positive, lsm_tree->bloom_false_positive);
	WT_STAT_SET(stats, lsm_lookup_no_bloom, lsm_tree->lookup_no_bloom);
	if (LF_ISSET(WT_STATISTICS_CLEAR)) {
		lsm_tree->bloom_hit = 0;
		lsm_tree->bloom_miss = 0;
		lsm_tree->bloom_false_positive = 0;
		lsm_tree->lookup_no_bloom = 0;
	}

	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));

	/* Hold the LSM lock so that we can safely walk through the chunks. */
	WT_ERR(__wt_readlock(session, lsm_tree->rwlock));
	locked = 1;

	/*
	 * For each chunk, get its statistics as well as any associated bloom
	 * filter statistics, then aggregate into the total statistics.
	 */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];

		/*
		 * Get the statistics for the chunk's underlying object.
		 *
		 * XXX kludge: we may have an empty chunk where no checkpoint
		 * was written.  If so, try to open the ordinary handle on that
		 * chunk instead.
		 */
		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "statistics:%s", chunk->uri));
		ret = __wt_curstat_open(session, uribuf->data,
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? disk_cfg : cfg,
		    &stat_cursor);
		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			ret = __wt_curstat_open(
			    session, uribuf->data, cfg, &stat_cursor);
		WT_ERR(ret);

		/*
		 * The underlying statistics have now been initialized; fill in
		 * values from the chunk's information, then aggregate into the
		 * top-level.
		 */
		child = (WT_DSRC_STATS *)
		    ((WT_CURSOR_STAT *)stat_cursor)->stats_first;
		WT_STAT_SET(child, lsm_generation_max, chunk->generation);

		__wt_stat_aggregate_dsrc_stats(child, stats);
		WT_ERR(stat_cursor->close(stat_cursor));

		if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			continue;

		/* Top-level statistic: maintain a count of bloom filters. */
		WT_STAT_INCR(stats, bloom_count);

		/* Get the bloom filter's underlying object. */
		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "statistics:%s", chunk->bloom_uri));
		WT_ERR(__wt_curstat_open(
		    session, uribuf->data, cfg, &stat_cursor));

		/*
		 * The underlying statistics have now been initialized; fill in
		 * values from the bloom filter's information, then aggregate
		 * into the top-level.
		 */
		child = (WT_DSRC_STATS *)
		    ((WT_CURSOR_STAT *)stat_cursor)->stats_first;
		WT_STAT_SET(child,
		    bloom_size, (chunk->count * lsm_tree->bloom_bit_count) / 8);
		WT_STAT_SET(child,
		    bloom_page_evict,
		    WT_STAT(child, cache_eviction_clean) +
		    WT_STAT(child, cache_eviction_dirty));
		WT_STAT_SET(child, bloom_page_read, WT_STAT(child, cache_read));

		__wt_stat_aggregate_dsrc_stats(child, stats);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

err:	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	__wt_scr_free(&uribuf);

	return (ret);
}
