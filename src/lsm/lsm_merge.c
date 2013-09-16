/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_merge_update_tree --
 *	Merge a set of chunks and populate a new one.
 *	Must be called with the LSM lock held.
 */
int
__wt_lsm_merge_update_tree(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, u_int start_chunk, u_int nchunks,
    WT_LSM_CHUNK *chunk)
{
	size_t chunk_sz, chunks_after_merge;
	u_int i, j;

	WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);

	/* Setup the array of obsolete chunks. */
	if (nchunks > lsm_tree->old_avail) {
		chunk_sz = sizeof(*lsm_tree->old_chunks);
		WT_RET(__wt_realloc_def(session, &lsm_tree->old_alloc,
		    (lsm_tree->nold_chunks - lsm_tree->old_avail) + nchunks,
		    &lsm_tree->old_chunks));
		lsm_tree->old_avail += (u_int)(lsm_tree->old_alloc / chunk_sz) -
		    lsm_tree->nold_chunks;
		lsm_tree->nold_chunks = (u_int)(lsm_tree->old_alloc / chunk_sz);
	}
	/* Copy entries one at a time, so we can reuse gaps in the list. */
	for (i = j = 0; j < nchunks && i < lsm_tree->nold_chunks; i++) {
		if (lsm_tree->old_chunks[i] == NULL) {
			lsm_tree->old_chunks[i] =
			    lsm_tree->chunk[start_chunk + j];
			++j;
			--lsm_tree->old_avail;
		}
	}

	WT_ASSERT(session, j == nchunks);

	/* Update the current chunk list. */
	chunks_after_merge = lsm_tree->nchunks - (nchunks + start_chunk);
	memmove(lsm_tree->chunk + start_chunk + 1,
	    lsm_tree->chunk + start_chunk + nchunks,
	    chunks_after_merge * sizeof(*lsm_tree->chunk));
	lsm_tree->nchunks -= nchunks - 1;
	memset(lsm_tree->chunk + lsm_tree->nchunks, 0,
	    (nchunks - 1) * sizeof(*lsm_tree->chunk));
	lsm_tree->chunk[start_chunk] = chunk;

	return (0);
}

/*
 * __wt_lsm_merge --
 *	Merge a set of chunks of an LSM tree.
 */
int
__wt_lsm_merge(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int id, int aggressive)
{
	WT_BLOOM *bloom;
	WT_CURSOR *dest, *src;
	WT_DECL_ITEM(bbuf);
	WT_DECL_RET;
	WT_ITEM buf, key, value;
	WT_LSM_CHUNK *chunk, *youngest;
	uint32_t generation, start_id;
	uint64_t insert_count, record_count;
	u_int dest_id, end_chunk, i, nchunks, start_chunk;
	u_int max_chunks, min_chunks;
	int create_bloom;
	const char *cfg[3];

	bloom = NULL;
	create_bloom = 0;
	dest = src = NULL;
	max_chunks = lsm_tree->merge_max;
	min_chunks = (id == 0) ? lsm_tree->merge_min : 2;
	start_id = 0;

	/*
	 * If there aren't any chunks to merge, or some of the chunks aren't
	 * yet written, we're done.  A non-zero error indicates that the worker
	 * should assume there is no work to do: if there are unwritten chunks,
	 * the worker should write them immediately.
	 */
	if (lsm_tree->nchunks <= 1)
		return (WT_NOTFOUND);

	/*
	 * Use the lsm_tree lock to read the chunks (so no switches occur), but
	 * avoid holding it while the merge is in progress: that may take a
	 * long time.
	 */
	WT_RET(__wt_writelock(session, lsm_tree->rwlock));

	/*
	 * Only include chunks that are stable on disk and not involved in a
	 * merge.
	 */
	end_chunk = lsm_tree->nchunks - 1;
	while (end_chunk > 0 &&
	    (!F_ISSET(lsm_tree->chunk[end_chunk], WT_LSM_CHUNK_ONDISK) ||
	    F_ISSET(lsm_tree->chunk[end_chunk], WT_LSM_CHUNK_MERGING)))
		--end_chunk;

	/*
	 * Look for the most efficient merge we can do.  We define efficiency
	 * as collapsing as many levels as possible while processing the
	 * smallest number of rows.
	 *
	 * We make a distinction between "major" and "minor" merges.  The
	 * difference is whether the oldest chunk is involved: if it is, we can
	 * discard tombstones, because there can be no older record to marked
	 * deleted.
	 *
	 * Respect the configured limit on the number of chunks to merge: start
	 * with the most recent set of chunks and work backwards until going
	 * further becomes significantly less efficient.
	 */
	for (start_chunk = end_chunk + 1, record_count = 0;
	    start_chunk > 0; ) {
		chunk = lsm_tree->chunk[start_chunk - 1];
		youngest = lsm_tree->chunk[end_chunk];
		nchunks = (end_chunk + 1) - start_chunk;

		/* If the chunk is already involved in a merge, stop. */
		if (F_ISSET(chunk, WT_LSM_CHUNK_MERGING))
			break;

		/*
		 * Stay in the youngest generation in the first thread if there
		 * are multiple threads.  If there is a single thread, wait
		 * looking for small merges before trying a big one.
		 */
		if (id == 0 && chunk->generation > 0 &&
		    (!aggressive || lsm_tree->merge_threads > 1))
			break;

		/*
		 * If we have enough chunks for a merge and the next chunk is
		 * in a different generation, stop.
		 */
		if (nchunks >= min_chunks &&
		    chunk->generation > youngest->generation)
			break;

		F_SET(chunk, WT_LSM_CHUNK_MERGING);
		record_count += chunk->count;
		--start_chunk;

		if (nchunks == max_chunks) {
			F_CLR(youngest, WT_LSM_CHUNK_MERGING);
			record_count -= youngest->count;
			--end_chunk;
		}
	}

	nchunks = (end_chunk + 1) - start_chunk;
	WT_ASSERT(session, nchunks <= max_chunks);

	if (nchunks > 0) {
		chunk = lsm_tree->chunk[start_chunk];
		start_id = chunk->id;
		youngest = lsm_tree->chunk[end_chunk];

		/*
		 * Don't do small merges or merge across more than 2
		 * generations.
		 */
		if (nchunks < min_chunks ||
		    chunk->generation > youngest->generation + 1) {
			for (i = 0; i < nchunks; i++)
				F_CLR(lsm_tree->chunk[start_chunk + i],
				    WT_LSM_CHUNK_MERGING);
			nchunks = 0;
		}
	}

	/* Find the merge generation. */
	for (generation = 0, i = 0; i < nchunks; i++)
		generation = WT_MAX(generation,
		    lsm_tree->chunk[start_chunk + i]->generation + 1);

	WT_RET(__wt_rwunlock(session, lsm_tree->rwlock));

	if (nchunks == 0)
		return (WT_NOTFOUND);

	/* Allocate an ID for the merge. */
	dest_id = WT_ATOMIC_ADD(lsm_tree->last, 1);

	WT_VERBOSE_RET(session, lsm,
	    "Merging chunks %d-%d into %d (%" PRIu64 " records)"
	    ", generation %d\n",
	    start_chunk, end_chunk, dest_id, record_count, generation);

	WT_RET(__wt_calloc_def(session, 1, &chunk));
	chunk->id = dest_id;

	if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_MERGED) &&
	    (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST) ||
	    start_chunk > 0) && record_count > 0)
		create_bloom = 1;

	/*
	 * Special setup for the merge cursor:
	 * first, reset to open the dependent cursors;
	 * then restrict the cursor to a specific number of chunks;
	 * then set MERGE so the cursor doesn't track updates to the tree.
	 */
	WT_ERR(__wt_open_cursor(session, lsm_tree->name, NULL, NULL, &src));
	F_SET(src, WT_CURSTD_RAW);
	WT_ERR(__wt_clsm_init_merge(src, start_chunk, start_id, nchunks));

	WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_setup_chunk(
	    session, lsm_tree, chunk));
	WT_ERR(ret);
	if (create_bloom) {
		WT_CLEAR(buf);
		WT_ERR(__wt_lsm_tree_bloom_name(
		    session, lsm_tree, chunk->id, &buf));
		chunk->bloom_uri = __wt_buf_steal(session, &buf, NULL);

		WT_ERR(__wt_bloom_create(session, chunk->bloom_uri,
		    lsm_tree->bloom_config,
		    record_count, lsm_tree->bloom_bit_count,
		    lsm_tree->bloom_hash_count, &bloom));
	}

	/* Discard pages we read as soon as we're done with them. */
	F_SET(session, WT_SESSION_NO_CACHE);

	cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	cfg[1] = "bulk,raw";
	cfg[2] = NULL;
	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));

	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		if (insert_count % 1000 &&
		    !F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
			ret = EINTR;
			goto err;
		}
		WT_ERR(src->get_key(src, &key));
		dest->set_key(dest, &key);
		WT_ERR(src->get_value(src, &value));
		dest->set_value(dest, &value);
		WT_ERR(dest->insert(dest));
		if (create_bloom)
			WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_ERR_NOTFOUND_OK(ret);

	WT_CSTAT_INCRV(session, lsm_rows_merged, insert_count);
	WT_VERBOSE_ERR(session, lsm,
	    "Bloom size for %" PRIu64 " has %" PRIu64 " items inserted.",
	    record_count, insert_count);

	/*
	 * We've successfully created the new chunk.  Now install it. We need
	 * to ensure that the NO_CACHE flag is cleared and the bloom filter
	 * is closed (even if a step fails), so track errors but don't return
	 * until we've cleaned up.
	 */
	WT_TRET(src->close(src));
	WT_TRET(dest->close(dest));
	src = dest = NULL;

	if (create_bloom) {
		if (ret == 0)
			WT_TRET(__wt_bloom_finalize(bloom));

		/*
		 * Read in a key to make sure the Bloom filters btree handle is
		 * open before it becomes visible to application threads.
		 * Otherwise application threads will stall while it is opened
		 * and internal pages are read into cache.
		 */
		if (ret == 0) {
			WT_CLEAR(key);
			WT_TRET_NOTFOUND_OK(__wt_bloom_get(bloom, &key));
		}

		WT_TRET(__wt_bloom_close(bloom));
		bloom = NULL;
	}
	F_CLR(session, WT_SESSION_NO_CACHE);
	WT_ERR(ret);

	/*
	 * Open a handle on the new chunk before application threads attempt
	 * to access it. Opening the pre-loads internal pages into the file
	 * system cache.
	 */
	cfg[1] = "checkpoint=WiredTigerCheckpoint";
	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));
	WT_TRET(dest->close(dest));
	dest = NULL;
	WT_ERR_NOTFOUND_OK(ret);

	WT_ERR(__wt_writelock(session, lsm_tree->rwlock));

	/*
	 * Check whether we raced with another merge, and adjust the chunk
	 * array offset as necessary.
	 */
	if (start_chunk >= lsm_tree->nchunks ||
	    lsm_tree->chunk[start_chunk]->id != start_id)
		for (start_chunk = 0;
		    start_chunk < lsm_tree->nchunks;
		    start_chunk++)
			if (lsm_tree->chunk[start_chunk]->id == start_id)
				break;

	ret = __wt_lsm_merge_update_tree(
	    session, lsm_tree, start_chunk, nchunks, chunk);

	if (create_bloom)
		F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	chunk->count = insert_count;
	chunk->generation = generation;
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);

	ret = __wt_lsm_meta_write(session, lsm_tree);
	lsm_tree->dsk_gen++;
	WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

err:	if (src != NULL)
		WT_TRET(src->close(src));
	if (dest != NULL)
		WT_TRET(dest->close(dest));
	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	__wt_scr_free(&bbuf);
	if (ret != 0) {
		/*
		 * Ideally we would drop the new chunk on error, but that
		 * introduces potential deadlock problems. It is relatively
		 * harmless to leave the file - it does not interfere
		 * with later re-use.
		WT_WITH_SCHEMA_LOCK(session,
		    (void)__wt_schema_drop(session, chunk->uri, NULL));
		 */
		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);

		if (ret == EINTR)
			WT_VERBOSE_TRET(session, lsm,
			    "Merge aborted due to close");
		else
			WT_VERBOSE_TRET(session, lsm,
			    "Merge failed with %s", wiredtiger_strerror(ret));
		F_CLR(session, WT_SESSION_NO_CACHE);
	}
	return (ret);
}
