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
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	const char *cfg[] = API_CONF_DEFAULTS(
	    session, open_cursor, "statistics_fast=on");
	const char *disk_cfg[] = API_CONF_DEFAULTS(session,
	    open_cursor, "checkpoint=WiredTigerCheckpoint,statistics_fast=on");
	const char *desc, *pvalue;
	char *uri;
	int i;
	size_t alloc_len, uri_len;
	uint64_t value;

	WT_UNUSED(flags);
	alloc_len = 0;
	uri = NULL;
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

	/* Grab a snapshot of the LSM tree chunks. */
	WT_CLEAR(cookie);
	WT_ERR(__wt_lsm_copy_chunks(session, lsm_tree, &cookie));

	/* Set the stats for this run. */
	WT_STAT_SET(lsm_tree->stats, chunk_count, cookie.nchunks);
	for (i = 0; i < cookie.nchunks; i++) {
		chunk = cookie.chunk_array[i];
		if (chunk->generation >
		    (uint32_t)WT_STAT(lsm_tree->stats, generation_max))
			WT_STAT_SET(lsm_tree->stats,
			    generation_max, chunk->generation);
		uri_len = strlen("statistics:") + strlen(chunk->uri) + 1;
		if (uri_len > alloc_len)
			WT_ERR(__wt_realloc(
			    session, &alloc_len, uri_len, &uri));
		snprintf(uri, alloc_len, "statistics:%s", chunk->uri);
		/*
		 * LSM chunk reads happen from a checkpoint, so get the
		 * statistics for a checkpoint if one exists.
		 */
		WT_ERR(__wt_curstat_open(session, uri,
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ? disk_cfg : cfg,
		    &stat_cursor));
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

		if (chunk->bloom_uri != NULL) {
			WT_STAT_INCR(lsm_tree->stats, bloom_count);
			WT_STAT_INCRV(lsm_tree->stats, bloom_space,
			    (chunk->count * lsm_tree->bloom_bit_count) / 8);
			uri_len = strlen("statistics:") +
			    strlen(chunk->bloom_uri) + 1;
			if (uri_len > alloc_len)
				WT_ERR(__wt_realloc(
				    session, &alloc_len, uri_len, &uri));
			snprintf(uri, alloc_len, "statistics:%s",
			    chunk->bloom_uri);
			WT_ERR(__wt_curstat_open(session, uri,
			    cfg, &stat_cursor));
			stat_cursor->set_key(stat_cursor, WT_STAT_page_evict);
			WT_ERR(stat_cursor->search(stat_cursor));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRV(lsm_tree->stats, cache_evict, value);
			WT_STAT_INCRV(lsm_tree->stats,
			    bloom_cache_evict, value);
			stat_cursor->set_key(
			    stat_cursor, WT_STAT_page_evict_fail);
			WT_ERR(stat_cursor->search(stat_cursor));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRV(lsm_tree->stats, cache_evict_fail, value);
			stat_cursor->set_key(stat_cursor, WT_STAT_page_read);
			WT_ERR(stat_cursor->search(stat_cursor));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRV(lsm_tree->stats, cache_read, value);
			WT_STAT_INCRV(lsm_tree->stats,
			    bloom_cache_read, value);
			stat_cursor->set_key(stat_cursor, WT_STAT_page_write);
			WT_ERR(stat_cursor->search(stat_cursor));
			WT_ERR(stat_cursor->get_value(
			    stat_cursor, &desc, &pvalue, &value));
			WT_STAT_INCRV(lsm_tree->stats, cache_write, value);
			WT_ERR(stat_cursor->close(stat_cursor));
		}
	}
err:	__wt_free(session, cookie.chunk_array);
	__wt_free(session, uri);

	return (ret);
}
