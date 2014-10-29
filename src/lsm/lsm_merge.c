/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
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
	size_t chunks_after_merge;
	u_int i;

	WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);

	/* Setup the array of obsolete chunks. */
	WT_RET(__wt_realloc_def(session, &lsm_tree->old_alloc,
	    lsm_tree->nold_chunks + nchunks, &lsm_tree->old_chunks));

	/* Copy entries one at a time, so we can reuse gaps in the list. */
	for (i = 0; i < nchunks; i++)
		lsm_tree->old_chunks[lsm_tree->nold_chunks++] =
		    lsm_tree->chunk[start_chunk + i];

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
__wt_lsm_merge(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, u_int id)
{
	WT_BLOOM *bloom;
	WT_CURSOR *dest, *src;
	WT_DECL_ITEM(bbuf);
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_LSM_CHUNK *chunk, *previous, *youngest;
	uint32_t aggressive, generation, max_gap, max_gen, max_level, start_id;
	uint64_t insert_count, record_count, chunk_size;
	u_int dest_id, end_chunk, i, merge_max, merge_min, nchunks, start_chunk;
	u_int verb;
	int create_bloom, locked, in_sync, tret;
	const char *cfg[3];
	const char *drop_cfg[] =
	    { WT_CONFIG_BASE(session, session_drop), "force", NULL };

	bloom = NULL;
	chunk_size = 0;
	create_bloom = 0;
	dest = src = NULL;
	locked = 0;
	start_id = 0;
	in_sync = 0;

	/*
	 * If the tree is open read-only or we are compacting, be very
	 * aggressive. Otherwise, we can spend a long time waiting for merges
	 * to start in read-only applications.
	 */
	if (!lsm_tree->modified ||
	    F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING))
		lsm_tree->merge_aggressiveness = 10;

	aggressive = lsm_tree->merge_aggressiveness;
	merge_max = (aggressive > 5) ? 100 : lsm_tree->merge_min;
	merge_min = (aggressive > 5) ? 2 : lsm_tree->merge_min;
	max_gap = (aggressive + 4) / 5;
	max_level = (lsm_tree->merge_throttle > 0) ? 0 : id + aggressive;

	/*
	 * If there aren't any chunks to merge, or some of the chunks aren't
	 * yet written, we're done.  A non-zero error indicates that the worker
	 * should assume there is no work to do: if there are unwritten chunks,
	 * the worker should write them immediately.
	 */
	if (lsm_tree->nchunks < merge_min)
		return (WT_NOTFOUND);

	/*
	 * Use the lsm_tree lock to read the chunks (so no switches occur), but
	 * avoid holding it while the merge is in progress: that may take a
	 * long time.
	 */
	WT_RET(__wt_lsm_tree_writelock(session, lsm_tree));

	/*
	 * Only include chunks that already have a Bloom filter or are the
	 * result of a merge and not involved in a merge.
	 */
	for (end_chunk = lsm_tree->nchunks - 1; end_chunk > 0; --end_chunk) {
		chunk = lsm_tree->chunk[end_chunk];
		WT_ASSERT(session, chunk != NULL);
		if (F_ISSET(chunk, WT_LSM_CHUNK_MERGING))
			continue;
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) || chunk->generation > 0)
			break;
		else if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF) &&
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			break;
	}

	/*
	 * Give up immediately if there aren't enough on disk chunks in the
	 * tree for a merge.
	 */
	if (end_chunk < merge_min - 1) {
		WT_RET(__wt_lsm_tree_writeunlock(session, lsm_tree));
		return (WT_NOTFOUND);
	}

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

		/*
		 * If the chunk is already involved in a merge or a Bloom
		 * filter is being built for it, stop.
		 */
		if (F_ISSET(chunk, WT_LSM_CHUNK_MERGING) || chunk->bloom_busy)
			break;

		/*
		 * Look for small merges before trying a big one: some threads
		 * should stay in low levels until we get more aggressive.
		 */
		if (chunk->generation > max_level)
			break;

		/*
		 * If the size of the chunks selected so far exceeds the
		 * configured maximum chunk size, stop.  Keep going if we can
		 * slide the window further into the tree: we don't want to
		 * leave small chunks in the middle.
		 */
		if ((chunk_size += chunk->size) > lsm_tree->chunk_max)
			if (nchunks < merge_min ||
			    (chunk->generation > youngest->generation &&
			    chunk_size - youngest->size > lsm_tree->chunk_max))
				break;

		/*
		 * If we have enough chunks for a merge and the next chunk is
		 * in too high a generation, stop.
		 */
		if (nchunks >= merge_min) {
			previous = lsm_tree->chunk[start_chunk];
			max_gen = youngest->generation + max_gap;
			if (previous->generation <= max_gen &&
			    chunk->generation > max_gen)
				break;
		}

		F_SET(chunk, WT_LSM_CHUNK_MERGING);
		record_count += chunk->count;
		--start_chunk;

		/*
		 * If we have a full window, or the merge would be too big,
		 * remove the youngest chunk.
		 */
		if (nchunks == merge_max ||
		    chunk_size > lsm_tree->chunk_max) {
			WT_ASSERT(session,
			    F_ISSET(youngest, WT_LSM_CHUNK_MERGING));
			F_CLR(youngest, WT_LSM_CHUNK_MERGING);
			record_count -= youngest->count;
			chunk_size -= youngest->size;
			--end_chunk;
		}
	}

	nchunks = (end_chunk + 1) - start_chunk;
	WT_ASSERT(session, nchunks <= merge_max);

	if (nchunks > 0) {
		WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);
		for (i = 0; i < nchunks; i++) {
			chunk = lsm_tree->chunk[start_chunk + i];
			WT_ASSERT(session,
			    F_ISSET(chunk, WT_LSM_CHUNK_MERGING));
		}

		chunk = lsm_tree->chunk[start_chunk];
		youngest = lsm_tree->chunk[end_chunk];
		start_id = chunk->id;

		/*
		 * Don't do merges that are too small or across too many
		 * generations.
		 */
		if (nchunks < merge_min ||
		    chunk->generation > youngest->generation + max_gap) {
			for (i = 0; i < nchunks; i++) {
				chunk = lsm_tree->chunk[start_chunk + i];
				WT_ASSERT(session,
				    F_ISSET(chunk, WT_LSM_CHUNK_MERGING));
				F_CLR(chunk, WT_LSM_CHUNK_MERGING);
			}
			nchunks = 0;
		}
	}

	/* Find the merge generation. */
	for (generation = 0, i = 0; i < nchunks; i++)
		generation = WT_MAX(generation,
		    lsm_tree->chunk[start_chunk + i]->generation + 1);

	WT_RET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (nchunks == 0)
		return (WT_NOTFOUND);

	/* Allocate an ID for the merge. */
	dest_id = WT_ATOMIC_ADD4(lsm_tree->last, 1);

	/*
	 * We only want to do the chunk loop if we're running with verbose,
	 * so we wrap these statements in the conditional.  Avoid the loop
	 * in the normal path.
	 */
	if (WT_VERBOSE_ISSET(session, WT_VERB_LSM)) {
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
		    "Merging %s chunks %u-%u into %u (%" PRIu64 " records)"
		    ", generation %" PRIu32,
		    lsm_tree->name,
		    start_chunk, end_chunk, dest_id, record_count, generation));
		for (verb = start_chunk; verb <= end_chunk; verb++)
			WT_RET(__wt_verbose(session, WT_VERB_LSM,
			    "%s: Chunk[%u] id %u",
			    lsm_tree->name, verb, lsm_tree->chunk[verb]->id));
	}

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

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));
	WT_ERR(ret);
	if (create_bloom) {
		WT_ERR(__wt_lsm_tree_bloom_name(
		    session, lsm_tree, chunk->id, &chunk->bloom_uri));

		WT_ERR(__wt_bloom_create(session, chunk->bloom_uri,
		    lsm_tree->bloom_config,
		    record_count, lsm_tree->bloom_bit_count,
		    lsm_tree->bloom_hash_count, &bloom));
	}

	/* Discard pages we read as soon as we're done with them. */
	F_SET(session, WT_SESSION_NO_CACHE);

	cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	cfg[1] = "bulk,raw,skip_sort_check";
	cfg[2] = NULL;
	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));

#define	LSM_MERGE_CHECK_INTERVAL	1000
	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		if (insert_count % LSM_MERGE_CHECK_INTERVAL == 0) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
				WT_ERR(EINTR);
			/*
			 * Help out with switching chunks in case the
			 * checkpoint worker is busy.
			 */
			if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
				WT_WITH_SCHEMA_LOCK(session, ret =
				    __wt_lsm_tree_switch(session, lsm_tree));
				WT_ERR(ret);
			}
			WT_STAT_FAST_CONN_INCRV(session,
			    lsm_rows_merged, LSM_MERGE_CHECK_INTERVAL);
			++lsm_tree->merge_progressing;
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

	WT_STAT_FAST_CONN_INCRV(session,
	    lsm_rows_merged, insert_count % LSM_MERGE_CHECK_INTERVAL);
	++lsm_tree->merge_progressing;
	WT_ERR(__wt_verbose(session, WT_VERB_LSM,
	    "Bloom size for %" PRIu64 " has %" PRIu64 " items inserted.",
	    record_count, insert_count));

	/*
	 * Closing and syncing the files can take a while.  Set the
	 * merge_syncing field so that compact knows it is still in
	 * progress.
	 */
	(void)WT_ATOMIC_ADD4(lsm_tree->merge_syncing, 1);
	in_sync = 1;
	/*
	 * We've successfully created the new chunk.  Now install it.  We need
	 * to ensure that the NO_CACHE flag is cleared and the bloom filter
	 * is closed (even if a step fails), so track errors but don't return
	 * until we've cleaned up.
	 */
	WT_TRET(src->close(src));
	WT_TRET(dest->close(dest));
	src = dest = NULL;

	F_CLR(session, WT_SESSION_NO_CACHE);

	/*
	 * We're doing advisory reads to fault the new trees into cache.
	 * Don't block if the cache is full: our next unit of work may be to
	 * discard some trees to free space.
	 */
	F_SET(session, WT_SESSION_NO_CACHE_CHECK);

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
	WT_ERR(ret);

	/*
	 * Open a handle on the new chunk before application threads attempt
	 * to access it, opening it pre-loads internal pages into the file
	 * system cache.
	 */
	cfg[1] = "checkpoint=" WT_CHECKPOINT;
	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cfg, &dest));
	WT_TRET(dest->close(dest));
	dest = NULL;
	++lsm_tree->merge_progressing;
	(void)WT_ATOMIC_SUB4(lsm_tree->merge_syncing, 1);
	in_sync = 0;
	WT_ERR_NOTFOUND_OK(ret);

	WT_ERR(__wt_lsm_tree_set_chunk_size(session, chunk));
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = 1;

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

	/*
	 * It is safe to error out here - since the update can only fail
	 * prior to making updates to the tree.
	 */
	WT_ERR(__wt_lsm_merge_update_tree(
	    session, lsm_tree, start_chunk, nchunks, chunk));

	if (create_bloom)
		F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	chunk->count = insert_count;
	chunk->generation = generation;
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);

	/*
	 * We have no current way of continuing if the metadata update fails,
	 * so we will panic in that case.  Put some effort into cleaning up
	 * after ourselves here - so things have a chance of shutting down.
	 *
	 * Any errors that happened after the tree was locked are
	 * fatal - we can't guarantee the state of the tree.
	 */
	if ((ret = __wt_lsm_meta_write(session, lsm_tree)) != 0)
		WT_PANIC_ERR(session, ret, "Failed finalizing LSM merge");

	lsm_tree->dsk_gen++;

	/* Update the throttling while holding the tree lock. */
	__wt_lsm_tree_throttle(session, lsm_tree, 1);

	/* Schedule a pass to discard old chunks */
	WT_ERR(__wt_lsm_manager_push_entry(
	    session, WT_LSM_WORK_DROP, 0, lsm_tree));

err:	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	if (in_sync)
		(void)WT_ATOMIC_SUB4(lsm_tree->merge_syncing, 1);
	if (src != NULL)
		WT_TRET(src->close(src));
	if (dest != NULL)
		WT_TRET(dest->close(dest));
	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	__wt_scr_free(&bbuf);
	if (ret != 0) {
		/* Drop the newly-created files on error. */
		WT_WITH_SCHEMA_LOCK(session,
		    tret = __wt_schema_drop(session, chunk->uri, drop_cfg));
		WT_TRET(tret);
		if (create_bloom) {
			WT_WITH_SCHEMA_LOCK(session, tret = __wt_schema_drop(
			    session, chunk->bloom_uri, drop_cfg));
			WT_TRET(tret);
		}
		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);

		if (ret == EINTR)
			WT_TRET(__wt_verbose(session, WT_VERB_LSM,
			    "Merge aborted due to close"));
		else
			WT_TRET(__wt_verbose(session, WT_VERB_LSM,
			    "Merge failed with %s", wiredtiger_strerror(ret)));
	}
	F_CLR(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_CACHE_CHECK);
	return (ret);
}
