/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_lsm_major_merge --
 *	Merge a set of chunks of an LSM tree including the oldest.
 */
int
__wt_lsm_major_merge(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src, *dest;
	WT_DECL_ITEM(bbuf);
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_LSM_CHUNK *chunk;
	WT_SESSION *wt_session;
	const char *dest_uri;
	uint64_t record_count;
	int dest_id, nchunks;

	src = dest = NULL;

	/*
	 * TODO: describe the dace with lsm_tree->lock here to avoid holding
	 * the tree locked while a merge is in progress.
	 */

	/* Figure out how many chunks to merge, allocate an ID for the merge. */
	__wt_spin_lock(session, &lsm_tree->lock);
	nchunks = lsm_tree->nchunks - 1;
	if (nchunks > 1)
		dest_id = lsm_tree->last++;
	__wt_spin_unlock(session, &lsm_tree->lock);

	/*
	 * If there aren't any chunks to merge, or some of the chunks aren't
	 * yet written, we're done.  A non-zero error indicates that the worker
	 * should assume there is no work to do: if there are unwritten chunks,
	 * the worker should write them immediately.
	 */
	if (nchunks <= 1)
		return (WT_NOTFOUND);

	/*
	 * We have a limited number of hazard references, and we want to bound
	 * the amount of work in the merge.
	 */
	nchunks = WT_MIN((int)S2C(session)->hazard_size / 2, nchunks);
	if (!F_ISSET(&lsm_tree->chunk[nchunks - 1], WT_LSM_CHUNK_ONDISK))
		return (0);

	printf("Merging first %d chunks into %d\n", nchunks, dest_id);

	WT_RET(__wt_scr_alloc(session, 0, &bbuf));
	WT_RET(__wt_lsm_tree_bloom_name(session, lsm_tree, dest_id, bbuf));
	/* TODO sum the record count in the chunks */
	record_count = 100000;
	WT_RET(__wt_bloom_create(session, bbuf->data, NULL, record_count,
	    lsm_tree->bloom_factor, lsm_tree->bloom_k, &bloom));

	/*
	 * Special setup for the merge cursor:
	 * first, reset to open the dependent cursors;
	 * then restrict the cursor to a specific number of chunks;
	 * then set MERGE so the cursor doesn't track updates to the tree.
	 */
	wt_session = &session->iface;
	WT_RET(wt_session->open_cursor(
	    wt_session, lsm_tree->name, NULL, NULL, &src));
	F_SET(src, WT_CURSTD_RAW);
	WT_ERR(src->reset(src));
	((WT_CURSOR_LSM *)src)->nchunks = nchunks;
	F_SET((WT_CURSOR_LSM *)src, WT_CLSM_MERGE);

	WT_WITH_SCHEMA_LOCK(session, ret = __wt_lsm_tree_create_chunk(
	    session, lsm_tree, dest_id, &dest_uri));
	WT_ERR(ret);
	WT_ERR(wt_session->open_cursor(
	    wt_session, dest_uri, NULL, "raw,bulk", &dest));

	for (record_count = 0; (ret = src->next(src)) == 0; record_count++) {
		WT_ERR(src->get_key(src, &key));
		dest->set_key(dest, &key);
		WT_ERR(src->get_value(src, &value));
		dest->set_value(dest, &value);
		WT_ERR(dest->insert(dest));
		WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_ERR_NOTFOUND_OK(ret);

	/* We've successfully created the new chunk.  Now install it. */
	WT_TRET(src->close(src));
	WT_TRET(dest->close(dest));
	WT_TRET(__wt_bloom_finalize(bloom));
	WT_TRET(__wt_bloom_close(bloom));
	src = dest = NULL;
	bloom = NULL;
	WT_ERR(ret);

	__wt_spin_lock(session, &lsm_tree->lock);
	/*
	 * TODO: save the old chunk names so they can be removed later, when
	 * we can be sure no cursors are looking at them.
	 * TODO: free the old chunk names before overwriting them.
	 */
	memmove(lsm_tree->chunk + 1, lsm_tree->chunk + nchunks,
	    (lsm_tree->nchunks - nchunks) * sizeof(*lsm_tree->chunk));
	lsm_tree->nchunks -= nchunks - 1;
	chunk = &lsm_tree->chunk[0];
	chunk->uri = dest_uri;
	chunk->bloom_uri = __wt_buf_steal(session, bbuf, NULL);
	chunk->count = record_count;
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);
	dest_uri = NULL;
	WT_ERR(__wt_lsm_tree_bump_gen(session, lsm_tree));
	ret = __wt_lsm_meta_write(session, lsm_tree);
	__wt_spin_unlock(session, &lsm_tree->lock);

	printf("Merge done\n");

err:	if (src != NULL)
		WT_TRET(src->close(src));
	if (dest != NULL)
		WT_TRET(dest->close(dest));
	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	__wt_scr_free(&bbuf);
	__wt_free(session, dest_uri);
	if (ret != 0)
		printf("Merge failed with %s\n", wiredtiger_strerror(ret));
	return (ret);
}
