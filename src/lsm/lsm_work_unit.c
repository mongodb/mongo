/*-
 * Copyright (c) 2014-2015 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_bloom_create(
    WT_SESSION_IMPL *, WT_LSM_TREE *, WT_LSM_CHUNK *, u_int);
static int __lsm_discard_handle(WT_SESSION_IMPL *, const char *, const char *);

/*
 * __lsm_copy_chunks --
 *	 Take a copy of part of the LSM tree chunk array so that we can work on
 *	 the contents without holding the LSM tree handle lock long term.
 */
static int
__lsm_copy_chunks(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_WORKER_COOKIE *cookie, int old_chunks)
{
	WT_DECL_RET;
	u_int i, nchunks;
	size_t alloc;

	/* Always return zero chunks on error. */
	cookie->nchunks = 0;

	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE))
		return (__wt_lsm_tree_readunlock(session, lsm_tree));

	/* Take a copy of the current state of the LSM tree. */
	nchunks = old_chunks ? lsm_tree->nold_chunks : lsm_tree->nchunks;
	alloc = old_chunks ? lsm_tree->old_alloc : lsm_tree->chunk_alloc;

	/*
	 * If the tree array of active chunks is larger than our current buffer,
	 * increase the size of our current buffer to match.
	 */
	if (cookie->chunk_alloc < alloc)
		WT_ERR(__wt_realloc(session,
		    &cookie->chunk_alloc, alloc, &cookie->chunk_array));
	if (nchunks > 0)
		memcpy(cookie->chunk_array,
		    old_chunks ? lsm_tree->old_chunks : lsm_tree->chunk,
		    nchunks * sizeof(*cookie->chunk_array));

	/*
	 * Mark each chunk as active, so we don't drop it until after we know
	 * it's safe.
	 */
	for (i = 0; i < nchunks; i++)
		(void)WT_ATOMIC_ADD4(cookie->chunk_array[i]->refcnt, 1);

err:	WT_TRET(__wt_lsm_tree_readunlock(session, lsm_tree));

	if (ret == 0)
		cookie->nchunks = nchunks;
	return (ret);
}

/*
 * __wt_lsm_get_chunk_to_flush --
 *	Find and pin a chunk in the LSM tree that is likely to need flushing.
 */
int
__wt_lsm_get_chunk_to_flush(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, int force, WT_LSM_CHUNK **chunkp)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, *evict_chunk, *flush_chunk;
	u_int i;

	*chunkp = NULL;
	chunk = evict_chunk = flush_chunk = NULL;

	WT_ASSERT(session, lsm_tree->queue_ref > 0);
	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_ACTIVE) || lsm_tree->nchunks == 0)
		return (__wt_lsm_tree_readunlock(session, lsm_tree));

	/* Search for a chunk to evict and/or a chunk to flush. */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
			/*
			 * Normally we don't want to force out the last chunk.
			 * But if we're doing a forced flush on behalf of a
			 * compact, then we want to include the final chunk.
			 */
			if (evict_chunk == NULL &&
			    !chunk->evicted &&
			    !F_ISSET(chunk, WT_LSM_CHUNK_STABLE))
				evict_chunk = chunk;
		} else if (flush_chunk == NULL &&
		    chunk->switch_txn != 0 &&
		    (force || i < lsm_tree->nchunks - 1))
			flush_chunk = chunk;
	}

	/*
	 * Don't be overly zealous about pushing old chunks from cache.
	 * Attempting too many drops can interfere with checkpoints.
	 *
	 * If retrying a discard push an additional work unit so there are
	 * enough to trigger checkpoints.
	 */
	if (evict_chunk != NULL && flush_chunk != NULL) {
		chunk = (__wt_random(session->rnd) & 1) ?
		    evict_chunk : flush_chunk;
		WT_ERR(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
	} else
		chunk = (evict_chunk != NULL) ? evict_chunk : flush_chunk;

	if (chunk != NULL) {
		WT_ERR(__wt_verbose(session, WT_VERB_LSM,
		    "Flush%s: return chunk %u of %u: %s",
		    force ? " w/ force" : "",
		    i, lsm_tree->nchunks, chunk->uri));

		(void)WT_ATOMIC_ADD4(chunk->refcnt, 1);
	}

err:	WT_RET(__wt_lsm_tree_readunlock(session, lsm_tree));

	*chunkp = chunk;
	return (ret);
}

/*
 * __lsm_unpin_chunks --
 *	Decrement the reference count for a set of chunks. Allowing those
 *	chunks to be considered for deletion.
 */
static void
__lsm_unpin_chunks(WT_SESSION_IMPL *session, WT_LSM_WORKER_COOKIE *cookie)
{
	u_int i;

	for (i = 0; i < cookie->nchunks; i++) {
		if (cookie->chunk_array[i] == NULL)
			continue;
		WT_ASSERT(session, cookie->chunk_array[i]->refcnt > 0);
		(void)WT_ATOMIC_SUB4(cookie->chunk_array[i]->refcnt, 1);
	}
	/* Ensure subsequent calls don't double decrement. */
	cookie->nchunks = 0;
}

/*
 * __wt_lsm_work_switch --
 *	Do a switch if the LSM tree needs one.
 */
int
__wt_lsm_work_switch(
    WT_SESSION_IMPL *session, WT_LSM_WORK_UNIT **entryp, int *ran)
{
	WT_DECL_RET;
	WT_LSM_WORK_UNIT *entry;

	/* We've become responsible for freeing the work unit. */
	entry = *entryp;
	*ran = 0;
	*entryp = NULL;

	if (F_ISSET(entry->lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
		WT_WITH_SCHEMA_LOCK(session,
		    ret = __wt_lsm_tree_switch(session, entry->lsm_tree));
		/* Failing to complete the switch is fine */
		if (ret == EBUSY) {
			if (F_ISSET(entry->lsm_tree, WT_LSM_TREE_NEED_SWITCH))
				WT_ERR(__wt_lsm_manager_push_entry(session,
				    WT_LSM_WORK_SWITCH, 0, entry->lsm_tree));
			ret = 0;
		} else
			*ran = 1;
	}
err:	__wt_lsm_manager_free_work_unit(session, entry);
	return (ret);
}

/*
 * __wt_lsm_work_bloom --
 *	Try to create a Bloom filter for the newest on-disk chunk that doesn't
 *	have one.
 */
int
__wt_lsm_work_bloom(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	u_int i, merge;

	WT_CLEAR(cookie);

	WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, 0));

	/* Create bloom filters in all checkpointed chunks. */
	merge = 0;
	for (i = 0; i < cookie.nchunks; i++) {
		chunk = cookie.chunk_array[i];

		/*
		 * Skip if a thread is still active in the chunk or it
		 * isn't suitable.
		 */
		if (!F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ||
		    F_ISSET(chunk, WT_LSM_CHUNK_BLOOM | WT_LSM_CHUNK_MERGING) ||
		    chunk->generation > 0 ||
		    chunk->count == 0)
			continue;

		/*
		 * See if we win the race to switch on the "busy" flag and
		 * recheck that the chunk still needs a Bloom filter.
		 */
		if (WT_ATOMIC_CAS4(chunk->bloom_busy, 0, 1)) {
			if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
				ret = __lsm_bloom_create(
				    session, lsm_tree, chunk, (u_int)i);
				/*
				 * Record if we were successful so that we can
				 * later push a merge work unit.
				 */
				if (ret == 0)
					merge = 1;
			}
			chunk->bloom_busy = 0;
			break;
		}
	}
	/*
	 * If we created any bloom filters, we push a merge work unit now.
	 */
	if (merge)
		WT_ERR(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_MERGE, 0, lsm_tree));

err:
	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);
	return (ret);
}

/*
 * __wt_lsm_checkpoint_chunk --
 *	Flush a single LSM chunk to disk.
 */
int
__wt_lsm_checkpoint_chunk(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_DECL_RET;
	WT_TXN_ISOLATION saved_isolation;

	/*
	 * If the chunk is already checkpointed, make sure it is also evicted.
	 * Either way, there is no point trying to checkpoint it again.
	 */
	if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) &&
	    !F_ISSET(chunk, WT_LSM_CHUNK_STABLE) &&
	    !chunk->evicted) {
		if ((ret = __lsm_discard_handle(
		    session, chunk->uri, NULL)) == 0)
			chunk->evicted = 1;
		else if (ret == EBUSY)
			ret = 0;
		else
			WT_RET_MSG(session, ret, "discard handle");
	}
	if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
		    "LSM worker %s already on disk",
		    chunk->uri));
		return (0);
	}

	/* Stop if a running transaction needs the chunk. */
	__wt_txn_update_oldest(session);
	if (chunk->switch_txn == WT_TXN_NONE ||
	    !__wt_txn_visible_all(session, chunk->switch_txn)) {
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
		    "LSM worker %s: running transaction, return",
		    chunk->uri));
		return (0);
	}

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker flushing %s",
	    chunk->uri));

	/*
	 * Flush the file before checkpointing: this is the expensive part in
	 * terms of I/O.
	 *
	 * Use the special eviction isolation level to avoid interfering with
	 * an application checkpoint: we have already checked that all of the
	 * updates in this chunk are globally visible.
	 *
	 * !!! We can wait here for checkpoints and fsyncs to complete, which
	 * can be a long time.
	 */
	if ((ret = __wt_session_get_btree(
	    session, chunk->uri, NULL, NULL, 0)) == 0) {
		saved_isolation = session->txn.isolation;
		session->txn.isolation = TXN_ISO_EVICTION;
		ret = __wt_cache_op(session, NULL, WT_SYNC_WRITE_LEAVES);
		session->txn.isolation = saved_isolation;
		WT_TRET(__wt_session_release_btree(session));
	}
	WT_RET(ret);

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker checkpointing %s",
	    chunk->uri));

	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_worker(session, chunk->uri,
	    __wt_checkpoint, NULL, NULL, 0));

	if (ret != 0)
		WT_RET_MSG(session, ret, "LSM checkpoint");

	/* Now the file is written, get the chunk size. */
	WT_RET(__wt_lsm_tree_set_chunk_size(session, chunk));

	/* Update the flush timestamp to help track ongoing progress. */
	WT_RET(__wt_epoch(session, &lsm_tree->last_flush_ts));

	/* Lock the tree, mark the chunk as on disk and update the metadata. */
	WT_RET(__wt_lsm_tree_writelock(session, lsm_tree));
	F_SET(chunk, WT_LSM_CHUNK_ONDISK);
	ret = __wt_lsm_meta_write(session, lsm_tree);
	++lsm_tree->dsk_gen;

	/* Update the throttle time. */
	__wt_lsm_tree_throttle(session, lsm_tree, 1);
	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (ret != 0)
		WT_RET_MSG(session, ret, "LSM metadata write");

	/*
	 * Clear the no-eviction flag so the primary can be evicted and
	 * eventually closed.  Only do this once the checkpoint has succeeded:
	 * otherwise, accessing the leaf page during the checkpoint can trigger
	 * forced eviction.
	 */
	WT_RET(__wt_session_get_btree(session, chunk->uri, NULL, NULL, 0));
	__wt_btree_evictable(session, 1);
	WT_RET(__wt_session_release_btree(session));

	/* Make sure we aren't pinning a transaction ID. */
	__wt_txn_release_snapshot(session);

	WT_RET(__wt_verbose(session, WT_VERB_LSM, "LSM worker checkpointed %s",
	    chunk->uri));

	/* Schedule a bloom filter create for our newly flushed chunk. */
	if (!FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF))
		WT_RET(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_BLOOM, 0, lsm_tree));
	else
		WT_RET(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_MERGE, 0, lsm_tree));
	return (0);
}

/*
 * __lsm_bloom_create --
 *	Create a bloom filter for a chunk of the LSM tree that has been
 *	checkpointed but not yet been merged.
 */
static int
__lsm_bloom_create(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk, u_int chunk_off)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src;
	WT_DECL_RET;
	WT_ITEM key;
	uint64_t insert_count;

	WT_RET(__wt_lsm_tree_setup_bloom(session, lsm_tree, chunk));

	bloom = NULL;
	/*
	 * This is merge-like activity, and we don't want compacts to give up
	 * because we are creating a bunch of bloom filters before merging.
	 */
	++lsm_tree->merge_progressing;
	WT_RET(__wt_bloom_create(session, chunk->bloom_uri,
	    lsm_tree->bloom_config, chunk->count,
	    lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));

	/* Open a special merge cursor just on this chunk. */
	WT_ERR(__wt_open_cursor(session, lsm_tree->name, NULL, NULL, &src));
	F_SET(src, WT_CURSTD_RAW);
	WT_ERR(__wt_clsm_init_merge(src, chunk_off, chunk->id, 1));

	/*
	 * Setup so that we don't hold pages we read into cache, and so
	 * that we don't get stuck if the cache is full. If we allow
	 * ourselves to get stuck creating bloom filters, the entire tree
	 * can stall since there may be no worker threads available to flush.
	 */
	F_SET(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_CACHE_CHECK);
	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		WT_ERR(src->get_key(src, &key));
		WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_ERR_NOTFOUND_OK(ret);
	WT_TRET(src->close(src));

	WT_TRET(__wt_bloom_finalize(bloom));
	WT_ERR(ret);

	F_CLR(session, WT_SESSION_NO_CACHE);

	/* Load the new Bloom filter into cache. */
	WT_CLEAR(key);
	WT_ERR_NOTFOUND_OK(__wt_bloom_get(bloom, &key));

	WT_ERR(__wt_verbose(session, WT_VERB_LSM,
	    "LSM worker created bloom filter %s. "
	    "Expected %" PRIu64 " items, got %" PRIu64,
	    chunk->bloom_uri, chunk->count, insert_count));

	/* Ensure the bloom filter is in the metadata. */
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	ret = __wt_lsm_meta_write(session, lsm_tree);
	++lsm_tree->dsk_gen;
	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (ret != 0)
		WT_ERR_MSG(session, ret, "LSM bloom worker metadata write");

err:	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	F_CLR(session, WT_SESSION_NO_CACHE | WT_SESSION_NO_CACHE_CHECK);
	return (ret);
}

/*
 * __lsm_discard_handle --
 *	Try to discard a handle from cache.
 */
static int
__lsm_discard_handle(
    WT_SESSION_IMPL *session, const char *uri, const char *checkpoint)
{
	/* This will fail with EBUSY if the file is still in use. */
	WT_RET(__wt_session_get_btree(session, uri, checkpoint, NULL,
	    WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

	F_SET(session->dhandle, WT_DHANDLE_DISCARD_FORCE);
	return (__wt_session_release_btree(session));
}

/*
 * __lsm_drop_file --
 *	Helper function to drop part of an LSM tree.
 */
static int
__lsm_drop_file(WT_SESSION_IMPL *session, const char *uri)
{
	WT_DECL_RET;
	const char *drop_cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_drop), "remove_files=false", NULL };

	/*
	 * We need to grab the schema lock to drop the file, so first try to
	 * make sure there is minimal work to freeing space in the cache.  Only
	 * bother trying to discard the checkpoint handle: the in-memory handle
	 * should have been closed already.
	 *
	 * This will fail with EBUSY if the file is still in use.
	 */
	WT_RET(__lsm_discard_handle(session, uri, WT_CHECKPOINT));

	/*
	 * Take the schema lock for the drop operation.  Since __wt_schema_drop
	 * results in the hot backup lock being taken when it updates the
	 * metadata (which would be too late to prevent our drop).
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_drop(session, uri, drop_cfg));

	if (ret == 0)
		ret = __wt_remove(session, uri + strlen("file:"));
	WT_RET(__wt_verbose(session, WT_VERB_LSM, "Dropped %s", uri));

	if (ret == EBUSY || ret == ENOENT)
		WT_RET(__wt_verbose(session, WT_VERB_LSM,
		    "LSM worker drop of %s failed with %d", uri, ret));

	return (ret);
}

/*
 * __wt_lsm_free_chunks --
 *	Try to drop chunks from the tree that are no longer required.
 */
int
__wt_lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	u_int i, skipped;
	int flush_metadata, drop_ret;

	flush_metadata = 0;

	if (lsm_tree->nold_chunks == 0)
		return (0);

	/*
	 * Make sure only a single thread is freeing the old chunk array
	 * at any time.
	 */
	if (!WT_ATOMIC_CAS4(lsm_tree->freeing_old_chunks, 0, 1))
		return (0);
	/*
	 * Take a copy of the current state of the LSM tree and look for chunks
	 * to drop.  We do it this way to avoid holding the LSM tree lock while
	 * doing I/O or waiting on the schema lock.
	 *
	 * This is safe because only one thread will be in this function at a
	 * time.  Merges may complete concurrently, and the old_chunks array
	 * may be extended, but we shuffle down the pointers each time we free
	 * one to keep the non-NULL slots at the beginning of the array.
	 */
	WT_CLEAR(cookie);
	WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, 1));
	for (i = skipped = 0; i < cookie.nchunks; i++) {
		chunk = cookie.chunk_array[i];
		WT_ASSERT(session, chunk != NULL);
		/* Skip the chunk if another worker is using it. */
		if (chunk->refcnt > 1) {
			++skipped;
			continue;
		}

		/*
		 * Don't remove files if a hot backup is in progress.
		 *
		 * The schema lock protects the set of live files, this check
		 * prevents us from removing a file that hot backup already
		 * knows about.
		 */
		if (S2C(session)->hot_backup != 0)
			break;

		/*
		 * Drop any bloom filters and chunks we can. Don't try to drop
		 * a chunk if the bloom filter drop fails.
		 *  An EBUSY return indicates that a cursor is still open in
		 *       the tree - move to the next chunk in that case.
		 * An ENOENT return indicates that the LSM tree metadata was
		 *       out of sync with the on disk state. Update the
		 *       metadata to match in that case.
		 */
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			drop_ret = __lsm_drop_file(session, chunk->bloom_uri);
			if (drop_ret == EBUSY) {
				++skipped;
				continue;
			} else if (drop_ret != ENOENT)
				WT_ERR(drop_ret);

			flush_metadata = 1;
			F_CLR(chunk, WT_LSM_CHUNK_BLOOM);
		}
		if (chunk->uri != NULL) {
			drop_ret = __lsm_drop_file(session, chunk->uri);
			if (drop_ret == EBUSY) {
				++skipped;
				continue;
			} else if (drop_ret != ENOENT)
				WT_ERR(drop_ret);
			flush_metadata = 1;
		}

		/* Lock the tree to clear out the old chunk information. */
		WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));

		/*
		 * The chunk we are looking at should be the first one in the
		 * tree that we haven't already skipped over.
		 */
		WT_ASSERT(session, lsm_tree->old_chunks[skipped] == chunk);
		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, lsm_tree->old_chunks[skipped]);

		/* Shuffle down to keep all occupied slots at the beginning. */
		if (--lsm_tree->nold_chunks > skipped) {
			memmove(lsm_tree->old_chunks + skipped,
			    lsm_tree->old_chunks + skipped + 1,
			    (lsm_tree->nold_chunks - skipped) *
			    sizeof(WT_LSM_CHUNK *));
			lsm_tree->old_chunks[lsm_tree->nold_chunks] = NULL;
		}

		WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));

		/*
		 * Clear the chunk in the cookie so we don't attempt to
		 * decrement the reference count.
		 */
		cookie.chunk_array[i] = NULL;
	}

err:	/* Flush the metadata unless the system is in panic */
	if (flush_metadata && ret != WT_PANIC) {
		WT_TRET(__wt_lsm_tree_writelock(session, lsm_tree));
		WT_TRET(__wt_lsm_meta_write(session, lsm_tree));
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	}
	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);
	lsm_tree->freeing_old_chunks = 0;

	/* Returning non-zero means there is no work to do. */
	if (!flush_metadata)
		WT_TRET(WT_NOTFOUND);

	return (ret);
}
