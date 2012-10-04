/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_merge_update_tree --
 *	Merge a set of chunks and create a new one.
 *	Must be called with the LSM lock held.
 */
int
__wt_lsm_merge_update_tree(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, int start_chunk, int nchunks, WT_LSM_CHUNK **chunkp)
{
	WT_LSM_CHUNK *chunk;
	size_t chunk_sz, chunks_after_merge;
	int i, j;

	/* Setup the array of obsolete chunks. */
	if (nchunks > lsm_tree->old_avail) {
		chunk_sz = sizeof(*lsm_tree->old_chunks);
		WT_RET(__wt_realloc(session,
		    &lsm_tree->old_alloc,
		    chunk_sz * WT_MAX(10, lsm_tree->nold_chunks + 2 * nchunks),
		    &lsm_tree->old_chunks));
		lsm_tree->old_avail += (int)(lsm_tree->old_alloc / chunk_sz) -
		    lsm_tree->nold_chunks;
		lsm_tree->nold_chunks = (int)(lsm_tree->old_alloc / chunk_sz);
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
	WT_RET(__wt_calloc_def(session, 1, &chunk));
	lsm_tree->chunk[start_chunk] = chunk;
	lsm_tree->dsk_gen++;

	*chunkp = chunk;
	return (0);
}

/*
 * __wt_lsm_merge --
 *	Merge a set of chunks of an LSM tree.
 */
int
__wt_lsm_merge(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src, *dest;
	WT_DECL_ITEM(bbuf);
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_LSM_CHUNK *chunk;
	WT_SESSION *wt_session;
	const char *dest_uri;
	uint64_t insert_count, record_count;
	int dest_id, end_chunk, max_chunks, nchunks, start_chunk;

	src = dest = NULL;
	dest_uri = NULL;
	bloom = NULL;
	max_chunks = (int)lsm_tree->merge_max;

	/*
	 * Take a copy of the latest chunk id. This value needs to be atomically
	 * read. We need a copy, since other threads may alter the chunk count
	 * while we are doing a merge.
	 */
	nchunks = lsm_tree->nchunks - 1;

	/*
	 * If there aren't any chunks to merge, or some of the chunks aren't
	 * yet written, we're done.  A non-zero error indicates that the worker
	 * should assume there is no work to do: if there are unwritten chunks,
	 * the worker should write them immediately.
	 */
	if (nchunks <= 1)
		return (WT_NOTFOUND);

	/*
	 * Use the lsm_tree lock to read the chunks (so no switches occur), but
	 * avoid holding it while the merge is in progress: that may take a
	 * long time.
	 */
	__wt_spin_lock(session, &lsm_tree->lock);

	/* Only include chunks that are stable on disk. */
	end_chunk = nchunks - 1;
	while (end_chunk > 0 &&
	    (!F_ISSET(lsm_tree->chunk[end_chunk], WT_LSM_CHUNK_ONDISK) ||
	    lsm_tree->chunk[end_chunk]->ncursor > 0))
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
		nchunks = end_chunk - start_chunk + 1;

		/*
		 * If the next chunk isn't within 20% of the efficiency of the
		 * set we already have, stop.
		 */
		if (nchunks > 1 &&
		    10 * chunk->count > 12 * record_count / (uint64_t)nchunks)
			break;

		record_count += chunk->count;
		--start_chunk;

		if (nchunks == max_chunks)
			record_count -= lsm_tree->chunk[end_chunk--]->count;
	}
	__wt_spin_unlock(session, &lsm_tree->lock);

	WT_ASSERT(session, nchunks <= max_chunks);

	if (nchunks <= 1)
		return (0);

	/* Allocate an ID for the merge. */
	dest_id = WT_ATOMIC_ADD(lsm_tree->last, 1);

	WT_VERBOSE_RET(session, lsm,
	    "Merging chunks %d-%d into %d (%" PRIu64 " records)\n",
	    start_chunk, end_chunk, dest_id, record_count);

	if (record_count != 0) {
		WT_RET(__wt_scr_alloc(session, 0, &bbuf));
		WT_ERR(__wt_lsm_tree_bloom_name(
		    session, lsm_tree, dest_id, bbuf));

		WT_ERR(__wt_bloom_create(session, bbuf->data,
		    NULL, record_count, lsm_tree->bloom_bit_count,
		    lsm_tree->bloom_hash_count, &bloom));
	}

	/*
	 * Special setup for the merge cursor:
	 * first, reset to open the dependent cursors;
	 * then restrict the cursor to a specific number of chunks;
	 * then set MERGE so the cursor doesn't track updates to the tree.
	 */
	wt_session = &session->iface;
	WT_ERR(wt_session->open_cursor(
	    wt_session, lsm_tree->name, NULL, NULL, &src));
	F_SET(src, WT_CURSTD_RAW);
	WT_ERR(__wt_clsm_init_merge(src, start_chunk, nchunks));

	WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_create_chunk(
	    session, lsm_tree, dest_id, &dest_uri));
	WT_ERR(ret);
	WT_ERR(wt_session->open_cursor(
	    wt_session, dest_uri, NULL, "raw,bulk", &dest));

	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		WT_ERR(src->get_key(src, &key));
		dest->set_key(dest, &key);
		WT_ERR(src->get_value(src, &value));
		dest->set_value(dest, &value);
		WT_ERR(dest->insert(dest));
		if (bloom != NULL)
			WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_VERBOSE_ERR(session, lsm,
	    "Bloom size for %" PRIu64 " has %" PRIu64 " items inserted.",
	    record_count, insert_count);
	WT_ERR_NOTFOUND_OK(ret);

	/* We've successfully created the new chunk.  Now install it. */
	WT_TRET(src->close(src));
	WT_TRET(dest->close(dest));
	src = dest = NULL;
	if (bloom != NULL) {
		WT_TRET(__wt_bloom_finalize(bloom));
		WT_TRET(__wt_bloom_close(bloom));
		bloom = NULL;
	}
	WT_ERR(ret);

	__wt_spin_lock(session, &lsm_tree->lock);
	ret = __wt_lsm_merge_update_tree(
	    session, lsm_tree, start_chunk, nchunks, &chunk);

	chunk->uri = dest_uri;
	dest_uri = NULL;
	if (bbuf != NULL)
		chunk->bloom_uri = __wt_buf_steal(session, bbuf, 0);
	chunk->count = insert_count;
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);

	ret = __wt_lsm_meta_write(session, lsm_tree);
	__wt_spin_unlock(session, &lsm_tree->lock);

err:	if (src != NULL)
		WT_TRET(src->close(src));
	if (dest != NULL)
		WT_TRET(dest->close(dest));
	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	__wt_scr_free(&bbuf);
	__wt_free(session, dest_uri);
	if (ret != 0)
		WT_VERBOSE_VOID(session, lsm,
		    "Merge failed with %s\n", wiredtiger_strerror(ret));
	return (ret);
}
