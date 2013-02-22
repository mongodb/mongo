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
	WT_DSRC_STATS *stats;
	WT_LSM_CHUNK *chunk;
	const char *cfg[] = API_CONF_DEFAULTS(
	    session, open_cursor, "statistics_fast=on");
	const char *disk_cfg[] = API_CONF_DEFAULTS(session,
	    open_cursor, "checkpoint=WiredTigerCheckpoint,statistics_fast=on");
	const char *desc, *pvalue;
	uint64_t value;
	u_int i;
	int locked, stat_key;

	WT_UNUSED(flags);
	locked = 0;

	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));

	/* Clear the statistics we are about to recalculate. */
	if (cst->stats != NULL)
		stats = (WT_DSRC_STATS *)cst->stats;
	else {
		WT_ERR(__wt_calloc_def(session, 1, &stats));
		__wt_stat_init_dsrc_stats(stats);
		cst->stats_first = cst->stats = (WT_STATS *)stats;
		cst->stats_count = sizeof(*stats) / sizeof(WT_STATS);
	}
	*stats = lsm_tree->stats;

	if (LF_ISSET(WT_STATISTICS_CLEAR))
		__wt_stat_clear_dsrc_stats(&lsm_tree->stats);

	/* Hold the LSM lock so that we can safely walk through the chunks. */
	WT_ERR(__wt_readlock(session, lsm_tree->rwlock));
	locked = 1;

	/* Set the stats for this run. */
	WT_STAT_SET(stats, lsm_chunk_count, lsm_tree->nchunks);
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		if (chunk->generation >
		    (uint32_t)WT_STAT(stats, lsm_generation_max))
			WT_STAT_SET(stats,
			    lsm_generation_max, chunk->generation);

		/*
		 * LSM chunk reads happen from a checkpoint, so get the
		 * statistics for a checkpoint if one exists.
		 */
		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "statistics:%s", chunk->uri));
		ret = __wt_curstat_open(session, uribuf->data,
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? disk_cfg : cfg,
		    &stat_cursor);
		/*
		 * XXX kludge: we may have an empty chunk where no checkpoint
		 * was written.  If so, try to open the ordinary handle on that
		 * chunk instead.
		 */
		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			ret = __wt_curstat_open(
			    session, uribuf->data, cfg, &stat_cursor);
		WT_ERR(ret);

		while ((ret = stat_cursor->next(stat_cursor)) == 0) {
			WT_ERR(stat_cursor->get_key(stat_cursor, &stat_key));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRKV(stats, stat_key, value);
		}
		WT_ERR_NOTFOUND_OK(ret);
		WT_ERR(stat_cursor->close(stat_cursor));

		if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			continue;

		WT_STAT_INCR(stats, bloom_count);
		WT_STAT_INCRV(stats, bloom_size,
		    (chunk->count * lsm_tree->bloom_bit_count) / 8);

		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "statistics:%s", chunk->bloom_uri));
		WT_ERR(__wt_curstat_open(session, uribuf->data,
		    cfg, &stat_cursor));

		stat_cursor->set_key(
		    stat_cursor, WT_STAT_DSRC_CACHE_EVICTION_CLEAN);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(stats, cache_eviction_clean, value);
		WT_STAT_INCRV(stats, bloom_page_evict, value);

		stat_cursor->set_key(
		    stat_cursor, WT_STAT_DSRC_CACHE_EVICTION_DIRTY);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(stats, cache_eviction_dirty, value);
		WT_STAT_INCRV(stats, bloom_page_evict, value);

		stat_cursor->set_key(
		    stat_cursor, WT_STAT_DSRC_CACHE_EVICTION_FAIL);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(stats, cache_eviction_fail, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_DSRC_CACHE_READ);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(stats, cache_read, value);
		WT_STAT_INCRV(stats, bloom_page_read, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_DSRC_CACHE_WRITE);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(stats, cache_write, value);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

err:	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	__wt_scr_free(&uribuf);

	return (ret);
}
