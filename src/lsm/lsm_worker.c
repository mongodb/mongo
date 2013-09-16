/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_bloom_create(WT_SESSION_IMPL *, WT_LSM_TREE *, WT_LSM_CHUNK *);
static int __lsm_discard_handle(WT_SESSION_IMPL *, const char *, const char *);
static int __lsm_free_chunks(WT_SESSION_IMPL *, WT_LSM_TREE *);

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
	u_int nchunks;
	size_t alloc;

	/* Always return zero chunks on error. */
	cookie->nchunks = 0;

	WT_RET(__wt_readlock(session, lsm_tree->rwlock));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
		return (__wt_rwunlock(session, lsm_tree->rwlock));

	/* Take a copy of the current state of the LSM tree. */
	nchunks = old_chunks ? lsm_tree->nold_chunks : lsm_tree->nchunks;
	alloc = old_chunks ? lsm_tree->old_alloc : lsm_tree->chunk_alloc;

	/*
	 * If the tree array of active chunks is larger than our current buffer,
	 * increase the size of our current buffer to match.
	 */
	if (cookie->chunk_alloc < alloc)
		WT_ERR(__wt_realloc(
		    session, &cookie->chunk_alloc, alloc,
		    &cookie->chunk_array));
	if (nchunks > 0)
		memcpy(cookie->chunk_array,
		    old_chunks ? lsm_tree->old_chunks : lsm_tree->chunk,
		    nchunks * sizeof(*cookie->chunk_array));

err:	WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

	if (ret == 0)
		cookie->nchunks = nchunks;
	return (ret);
}

/*
 * __wt_lsm_merge_worker --
 *	The merge worker thread for an LSM tree, responsible for merging
 *	on-disk trees.
 */
void *
__wt_lsm_merge_worker(void *vargs)
{
	WT_DECL_RET;
	WT_LSM_WORKER_ARGS *args;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	uint64_t saved_gen;
	u_int id, stallms;
	int aggressive, passive, progress;

	args = vargs;
	lsm_tree = args->lsm_tree;
	id = args->id;
	session = lsm_tree->worker_sessions[id];
	__wt_free(session, args);

	saved_gen = lsm_tree->dsk_gen;
	aggressive = 0;
	stallms = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		progress = 0;

		/* Clear any state from previous worker thread iterations. */
		session->dhandle = NULL;

		if (__wt_lsm_merge(session, lsm_tree, id, aggressive) == 0)
			progress = 1;

		/* Clear any state from previous worker thread iterations. */
		WT_CLEAR_BTREE_IN_SESSION(session);

		/*
		 * Only have one thread freeing old chunks, and only if there
		 * are chunks to free.
		 */
		if (id == 0 && lsm_tree->nold_chunks > 0 &&
		    __lsm_free_chunks(session, lsm_tree) == 0)
			progress = 1;

		if (progress || saved_gen != lsm_tree->dsk_gen) {
			aggressive = 0;
			stallms = 0;
			saved_gen = lsm_tree->dsk_gen;
		} else if (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
			/*
			 * The "main" thread polls 10 times per second,
			 * secondary threads once per second.
			 */
			WT_ERR(__wt_cond_wait(
			    session, lsm_tree->work_cond,
			    id == 0 ? 100000 : 1000000));
			stallms += (id == 0) ? 100 : 1000;

			/*
			 * Get aggressive if more than enough chunks for a
			 * merge should have been created while we waited.
			 * Use 30 seconds as a default if we don't have an
			 * estimate.
			 */
			passive = !aggressive;
			aggressive = (stallms >
			    (lsm_tree->chunk_fill_ms == 0 ? 30000 :
			    lsm_tree->merge_min * lsm_tree->chunk_fill_ms));

			if (passive && aggressive)
				WT_VERBOSE_ERR(session, lsm,
				     "LSM merge got aggressive, "
				     "%u / %" PRIu64,
				     stallms, lsm_tree->chunk_fill_ms);
		}
	}

	if (0) {
err:		__wt_err(session, ret, "LSM merge worker failed");
	}

	return (NULL);
}

/*
 * __wt_lsm_bloom_worker --
 *	A worker thread for an LSM tree, responsible for creating Bloom filters
 *	for the newest on-disk chunks.
 */
void *
__wt_lsm_bloom_worker(void *arg)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_LSM_WORKER_COOKIE cookie;
	WT_SESSION_IMPL *session;
	u_int i, j;

	lsm_tree = arg;
	session = lsm_tree->bloom_session;

	WT_CLEAR(cookie);

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		WT_ERR(__lsm_copy_chunks(session, lsm_tree, &cookie, 0));

		/* Create bloom filters in all checkpointed chunks. */
		for (i = 0, j = 0; i < cookie.nchunks; i++) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
				goto err;

			chunk = cookie.chunk_array[i];

			/*
			 * Skip if a thread is still active in the chunk or it
			 * isn't suitable.
			 */
			if (!F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ||
			    F_ISSET(chunk,
				WT_LSM_CHUNK_BLOOM | WT_LSM_CHUNK_MERGING) ||
			    chunk->generation > 0 ||
			    chunk->count == 0)
				continue;

			/*
			 * If a bloom filter create fails, restart at the
			 * beginning of the chunk array. Don't exit the thread.
			 */
			if (__lsm_bloom_create(session, lsm_tree, chunk) != 0)
				break;
			++j;
		}
		if (j == 0 && F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
			WT_ERR(__wt_cond_wait(
			    session, lsm_tree->work_cond, 100000));
	}

err:	__wt_free(session, cookie.chunk_array);
	/*
	 * The thread will only exit with failure if we run out of memory or
	 * there is some other system driven failure. We can't keep going
	 * after such a failure - ensure WiredTiger shuts down.
	 */
	if (ret != 0)
		WT_PANIC_ERR(session, ret,
		    "Shutting down LSM bloom utility thread");
	return (NULL);
}

/*
 * __wt_lsm_checkpoint_worker --
 *	A worker thread for an LSM tree, responsible for flushing new chunks to
 *	disk.
 */
void *
__wt_lsm_checkpoint_worker(void *arg)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_LSM_WORKER_COOKIE cookie;
	WT_SESSION_IMPL *session;
	u_int i, j;

	lsm_tree = arg;
	session = lsm_tree->ckpt_session;

	WT_CLEAR(cookie);

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
			WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
			if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
				WT_WITH_SCHEMA_LOCK(session, ret =
				    __wt_lsm_tree_switch(session, lsm_tree));
			WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
			WT_ERR(ret);
		}

		WT_ERR(__lsm_copy_chunks(session, lsm_tree, &cookie, 0));

		/* Write checkpoints in all completed files. */
		for (i = 0, j = 0; i < cookie.nchunks - 1; i++) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
				goto err;

			if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
				break;

			chunk = cookie.chunk_array[i];

			/* Stop if a running transaction needs the chunk. */
			__wt_txn_refresh_force(session);
			if (!__wt_txn_visible_all(session, chunk->txnid_max))
				break;

			/*
			 * If the chunk is already checkpointed, make sure it
			 * is also evicted.  Either way, there is no point
			 * trying to checkpoint it again.
			 */
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
				if (F_ISSET(chunk, WT_LSM_CHUNK_EVICTED))
					continue;

				if ((ret = __lsm_discard_handle(
				    session, chunk->uri, NULL)) == 0)
					F_SET(chunk, WT_LSM_CHUNK_EVICTED);
				else if (ret == EBUSY)
					ret = 0;
				else
					WT_ERR_MSG(session, ret,
					    "discard handle");
				continue;
			}

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker flushing %u", i);

			/*
			 * Flush the file before checkpointing: this is the
			 * expensive part in terms of I/O: do it without
			 * holding the schema lock.  We need to hold the
			 * checkpoint lock, otherwise this sync can interfere
			 * with an application checkpoint.
			 */
			WT_ERR(__wt_session_get_btree(
			    session, chunk->uri, NULL, NULL, 0));
			if (__wt_spin_trylock(
			    session, &S2C(session)->checkpoint_lock) == 0) {
				ret = __wt_sync_file(
				    session, WT_SYNC_WRITE_LEAVES);
				__wt_spin_unlock(
				   session, &S2C(session)->checkpoint_lock);
			}

			/*
			 * Clear the "cache resident" flag so the primary can
			 * be evicted and eventually closed.
			 */
			if (ret == 0)
				__wt_btree_evictable(session, 1);
			WT_TRET(__wt_session_release_btree(session));
			WT_ERR(ret);

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointing %u", i);

			F_SET(lsm_tree, WT_LSM_TREE_LOCKED);
			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, NULL, NULL, 0));
			F_CLR(lsm_tree, WT_LSM_TREE_LOCKED);

			if (ret != 0) {
				__wt_err(session, ret, "LSM checkpoint");
				break;
			}

			++j;
			WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
			F_SET(chunk, WT_LSM_CHUNK_ONDISK);
			ret = __wt_lsm_meta_write(session, lsm_tree);
			++lsm_tree->dsk_gen;
			WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

			if (ret != 0) {
				__wt_err(session, ret,
				    "LSM checkpoint metadata write");
				break;
			}

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointed %u", i);
		}
		if (j == 0 && F_ISSET(lsm_tree, WT_LSM_TREE_WORKING) &&
		    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
			WT_ERR(__wt_cond_wait(
			    session, lsm_tree->work_cond, 100000));
	}
err:	__wt_free(session, cookie.chunk_array);
	/*
	 * The thread will only exit with failure if we run out of memory or
	 * there is some other system driven failure. We can't keep going
	 * after such a failure - ensure WiredTiger shuts down.
	 */
	if (ret != 0 && ret != WT_NOTFOUND)
		WT_PANIC_ERR(session, ret,
		    "Shutting down LSM checkpoint utility thread");
	return (NULL);
}

/*
 * __lsm_bloom_create --
 *	Create a bloom filter for a chunk of the LSM tree that has been
 *	checkpointed but not yet been merged.
 */
static int
__lsm_bloom_create(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src;
	WT_DECL_RET;
	WT_ITEM buf, key;
	WT_SESSION *wt_session;
	const char *cur_cfg[3];
	uint64_t insert_count;
	int exist;

	/*
	 * Normally, the Bloom URI is populated when the chunk struct is
	 * allocated.  After an open, however, it may not have been.
	 * Deal with that here.
	 */
	if (chunk->bloom_uri == NULL) {
		WT_CLEAR(buf);
		WT_RET(__wt_lsm_tree_bloom_name(
		    session, lsm_tree, chunk->id, &buf));
		chunk->bloom_uri = __wt_buf_steal(session, &buf, NULL);
	}

	/*
	 * Drop the bloom filter first - there may be some content hanging over
	 * from an aborted merge or checkpoint.
	 */
	wt_session = &session->iface;
	WT_RET(__wt_exist(session, chunk->bloom_uri + strlen("file:"), &exist));
	if (exist)
		WT_RET(wt_session->drop(wt_session, chunk->bloom_uri, "force"));

	bloom = NULL;

	WT_RET(__wt_bloom_create(session, chunk->bloom_uri,
	    lsm_tree->bloom_config, chunk->count,
	    lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));

	cur_cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	cur_cfg[1] = "checkpoint=WiredTigerCheckpoint,raw";
	cur_cfg[2] = NULL;
	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cur_cfg, &src));

	F_SET(session, WT_SESSION_NO_CACHE);
	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		WT_ERR(src->get_key(src, &key));
		WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_ERR_NOTFOUND_OK(ret);
	WT_TRET(src->close(src));
	F_CLR(session, WT_SESSION_NO_CACHE);

	WT_TRET(__wt_bloom_finalize(bloom));
	WT_ERR(ret);

	WT_VERBOSE_ERR(session, lsm,
	    "LSM worker created bloom filter %s. "
	    "Expected %" PRIu64 " items, got %" PRIu64,
	    chunk->bloom_uri, chunk->count, insert_count);

	/* Ensure the bloom filter is in the metadata. */
	WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
	ret = __wt_lsm_meta_write(session, lsm_tree);
	F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	++lsm_tree->dsk_gen;
	WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

	if (ret != 0)
		WT_ERR_MSG(session, ret, "LSM bloom worker metadata write");

err:	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	F_CLR(session, WT_SESSION_NO_CACHE);
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
	WT_DECL_RET;
	int locked;

	/*
	 * We need to grab the schema lock to drop the file, so first try to
	 * discard the handle so there is minimal work to do while holding the
	 * schema lock.
	 *
	 * This will fail with EBUSY if the file is still in use.
	 */
	WT_RET(__wt_session_get_btree(session, uri, checkpoint, NULL,
	    WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

	WT_ASSERT(session, S2BT(session)->modified == 0);

	/*
	 * We need the checkpoint lock here: if a checkpoint is in progress, it
	 * may already know about this handle.  We *can't* get the checkpoint
	 * lock earlier or it will deadlock with the schema lock.
	 */
	locked = 0;
	if ((ret =
	    __wt_spin_trylock(session, &S2C(session)->checkpoint_lock)) == 0) {
		locked = 1;
		F_SET(session->dhandle, WT_DHANDLE_DISCARD);
	}
	WT_TRET(__wt_session_release_btree(session));
	if (locked)
		__wt_spin_unlock(session, &S2C(session)->checkpoint_lock);

	return (ret);
}

/*
 * __lsm_drop_file --
 *	Helper function to drop part of an LSM tree.
 */
static int
__lsm_drop_file(WT_SESSION_IMPL *session, const char *uri)
{
	WT_DECL_RET;
	const char *drop_cfg[] = {
	    WT_CONFIG_BASE(session, session_drop), "remove_files=false", NULL
	};

	/*
	 * We need to grab the schema lock to drop the file, so first try to
	 * make sure there is minimal work to freeing space in the cache.
	 * This will fail with EBUSY if the file is still in use.
	 */
	WT_RET(__lsm_discard_handle(session, uri, NULL));
	WT_RET(__lsm_discard_handle(session, uri, "WiredTigerCheckpoint"));

	/*
	 * Take the schema lock for the drop operation. Play games with the
	 * hot backup lock. Since __wt_schema_drop results in the hot backup
	 * lock being taken when it updates the metadata (which would be too
	 * late to prevent our drop).
	 */
	WT_WITH_SCHEMA_LOCK(session,
	    ret = __wt_schema_drop(session, uri, drop_cfg));

	if (ret == 0)
		ret = __wt_remove(session, uri + strlen("file:"));

	return (ret);
}

/*
 * __lsm_free_chunks --
 *	Try to drop chunks from the tree that are no longer required.
 */
static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	u_int i, skipped;
	int progress;

	/*
	 * Take a copy of the current state of the LSM tree and look for chunks
	 * to drop.  We do it this way to avoid holding the LSM tree lock while
	 * doing I/O or waiting on the schema lock.
	 *
	 * This is safe because only one thread will be in this function at a
	 * time (the first merge thread).  Merges may complete concurrently,
	 * and the old_chunks array may be extended, but we shuffle down the
	 * pointers each time we free one to keep the non-NULL slots at the
	 * beginning of the array.
	 */
	WT_CLEAR(cookie);
	WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, 1));
	for (i = skipped = 0, progress = 0; i < cookie.nchunks; i++) {
		chunk = cookie.chunk_array[i];
		WT_ASSERT(session, chunk != NULL);
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if ((ret = __lsm_drop_file(
			    session, chunk->bloom_uri)) == EBUSY) {
				WT_VERBOSE_ERR(session, lsm,
				    "LSM worker bloom drop busy: %s.",
				    chunk->bloom_uri);
				++skipped;
				continue;
			} else
				WT_ERR(ret);

			F_CLR(chunk, WT_LSM_CHUNK_BLOOM);
		}
		if (chunk->uri != NULL) {
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if ((ret = __lsm_drop_file(
			    session, chunk->uri)) == EBUSY) {
				WT_VERBOSE_ERR(session, lsm,
				    "LSM worker drop busy: %s.",
				    chunk->uri);
				++skipped;
				continue;
			} else
				WT_ERR(ret);
		}

		progress = 1;

		/* Lock the tree to clear out the old chunk information. */
		WT_ERR(__wt_writelock(session, lsm_tree->rwlock));

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
		WT_ERR(__wt_rwunlock(session, lsm_tree->rwlock));
	}

err:	__wt_free(session, cookie.chunk_array);
	if (progress)
		WT_TRET(__wt_lsm_meta_write(session, lsm_tree));

	/* Returning non-zero means there is no work to do. */
	if (!progress)
		WT_TRET(WT_NOTFOUND);

	return (ret);
}
