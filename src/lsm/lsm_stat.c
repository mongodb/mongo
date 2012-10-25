/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
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
__wt_lsm_stat_init(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t flags)
{
	WT_CURSOR *stat_cursor;
	WT_DECL_ITEM(uribuf);
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	const char *cfg[] = API_CONF_DEFAULTS(
	    session, open_cursor, "statistics_fast=on");
	const char *disk_cfg[] = API_CONF_DEFAULTS(session,
	    open_cursor, "checkpoint=WiredTigerCheckpoint,statistics_fast=on");
	const char *desc, *pvalue;
	int i;
	uint64_t value;

	WT_UNUSED(flags);
	WT_ERR(__wt_scr_alloc(session, 0, &uribuf));

	/*
	 * TODO: Make a copy of the stat fields so the stat cursor gets a
	 * consistent view? If so should the copy belong to the stat cursor?
	 */
	/* Clear the statistics we are about to recalculate. */
	WT_STAT_SET(lsm_tree->stats, bloom_cache_read, 0);
	WT_STAT_SET(lsm_tree->stats, bloom_cache_evict, 0);
	WT_STAT_SET(lsm_tree->stats, bloom_count, 0);
	WT_STAT_SET(lsm_tree->stats, bloom_space, 0);
	WT_STAT_SET(lsm_tree->stats, cache_evict, 0);
	WT_STAT_SET(lsm_tree->stats, cache_evict_fail, 0);
	WT_STAT_SET(lsm_tree->stats, cache_read, 0);
	WT_STAT_SET(lsm_tree->stats, cache_write, 0);
	WT_STAT_SET(lsm_tree->stats, chunk_cache_evict, 0);
	WT_STAT_SET(lsm_tree->stats, chunk_cache_read, 0);
	WT_STAT_SET(lsm_tree->stats, generation_max, 0);

	/* Hold the LSM lock so that we can safely walk through the chunks. */
	__wt_spin_lock(session, &lsm_tree->lock);

	/* Set the stats for this run. */
	WT_STAT_SET(lsm_tree->stats, chunk_count, lsm_tree->nchunks);
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		if (chunk->generation >
		    (uint32_t)WT_STAT(lsm_tree->stats, generation_max))
			WT_STAT_SET(lsm_tree->stats,
			    generation_max, chunk->generation);

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

		stat_cursor->set_key(stat_cursor, WT_STAT_page_evict_fail);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_evict_fail, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_evict);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_evict, value);
		WT_STAT_INCRV(lsm_tree->stats, chunk_cache_evict, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_read);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_read, value);
		WT_STAT_INCRV(lsm_tree->stats, chunk_cache_read, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_write);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_write, value);
		WT_ERR(stat_cursor->close(stat_cursor));

		if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			continue;

		WT_STAT_INCR(lsm_tree->stats, bloom_count);
		WT_STAT_INCRV(lsm_tree->stats, bloom_space,
		    (chunk->count * lsm_tree->bloom_bit_count) / 8);

		WT_ERR(__wt_buf_fmt(
		    session, uribuf, "statistics:%s", chunk->bloom_uri));
		WT_ERR(__wt_curstat_open(session, uribuf->data,
		    cfg, &stat_cursor));

		stat_cursor->set_key(stat_cursor, WT_STAT_page_evict);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_evict, value);
		WT_STAT_INCRV(lsm_tree->stats, bloom_cache_evict, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_evict_fail);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_evict_fail, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_read);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_read, value);
		WT_STAT_INCRV(lsm_tree->stats, bloom_cache_read, value);

		stat_cursor->set_key(stat_cursor, WT_STAT_page_write);
		WT_ERR(stat_cursor->search(stat_cursor));
		WT_ERR(stat_cursor->get_value(
		    stat_cursor, &desc, &pvalue, &value));
		WT_STAT_INCRV(lsm_tree->stats, cache_write, value);
		WT_ERR(stat_cursor->close(stat_cursor));
	}

err:	__wt_spin_unlock(session, &lsm_tree->lock);
	__wt_scr_free(&uribuf);

	return (ret);
}
