/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_bloom_create(
    WT_SESSION_IMPL *, WT_LSM_TREE *, WT_LSM_CHUNK *, u_int);
static int __lsm_bloom_work(WT_SESSION_IMPL *, WT_LSM_TREE *);
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
	u_int i, nchunks;
	size_t alloc;

	/* Always return zero chunks on error. */
	cookie->nchunks = 0;

	WT_RET(__wt_lsm_tree_lock(session, lsm_tree, 0));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
		return (__wt_lsm_tree_unlock(session, lsm_tree));

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
		(void)WT_ATOMIC_ADD(cookie->chunk_array[i]->refcnt, 1);

err:	WT_TRET(__wt_lsm_tree_unlock(session, lsm_tree));

	if (ret == 0)
		cookie->nchunks = nchunks;
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
		(void)WT_ATOMIC_SUB(cookie->chunk_array[i]->refcnt, 1);
	}
	/* Ensure subsequent calls don't double decrement. */
	cookie->nchunks = 0;
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
	u_int aggressive, chunk_wait, id, old_aggressive, stallms;
	int progress;

	args = vargs;
	lsm_tree = args->lsm_tree;
	id = args->id;
	session = lsm_tree->worker_sessions[id];
	__wt_free(session, args);

	aggressive = chunk_wait = stallms = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		/*
		 * Help out with switching chunks in case the checkpoint worker
		 * is busy.
		 */
		if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_lsm_tree_switch(session, lsm_tree));
			WT_ERR(ret);
		}

		progress = 0;

		/* Clear any state from previous worker thread iterations. */
		session->dhandle = NULL;

		/* Try to create a Bloom filter. */
		if (__lsm_bloom_work(session, lsm_tree) == 0)
			progress = 1;

		/* If we didn't create a Bloom filter, try to merge. */
		if ((id != 0 || progress == 0) &&
		    __wt_lsm_merge(session, lsm_tree, id, aggressive) == 0)
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

		if (progress)
			stallms = 0;
		else if (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING) &&
		    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
			/* Poll 10 times per second. */
			WT_ERR_TIMEDOUT_OK(__wt_cond_wait(
			    session, lsm_tree->work_cond, 100000));
			/*
			 * Randomize the tracking of stall time so that with
			 * multiple LSM trees open, they don't all get
			 * aggressive in lock-step.
			 */
			stallms += __wt_random() % 200;

			/*
			 * Get aggressive if more than enough chunks for a
			 * merge should have been created while we waited.
			 * Use 10 seconds as a default if we don't have an
			 * estimate.
			 */
			chunk_wait = stallms / (lsm_tree->chunk_fill_ms == 0 ?
			    10000 : lsm_tree->chunk_fill_ms);
			old_aggressive = aggressive;
			aggressive = chunk_wait / lsm_tree->merge_min;

			if (aggressive > old_aggressive)
				WT_VERBOSE_ERR(session, lsm,
				     "LSM merge got aggressive (%u), "
				     "%u / %" PRIu64,
				     aggressive, stallms,
				     lsm_tree->chunk_fill_ms);
		}
	}

	if (0) {
err:		__wt_err(session, ret, "LSM merge worker failed");
	}

	return (NULL);
}

/*
 * __lsm_bloom_work --
 *	Try to create a Bloom filter for the newest on-disk chunk that doesn't
 *	have one.
 */
static int
__lsm_bloom_work(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_WORKER_COOKIE cookie;
	u_int i;

	WT_CLEAR(cookie);
	/* If no work is done, tell our caller by returning WT_NOTFOUND. */
	ret = WT_NOTFOUND;

	WT_RET(__lsm_copy_chunks(session, lsm_tree, &cookie, 0));

	/* Create bloom filters in all checkpointed chunks. */
	for (i = 0; i < cookie.nchunks; i++) {
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
		 * See if we win the race to switch on the "busy" flag and
		 * recheck that the chunk still needs a Bloom filter.
		 */
		if (WT_ATOMIC_CAS(chunk->bloom_busy, 0, 1)) {
			if (!F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
				ret = __lsm_bloom_create(
				    session, lsm_tree, chunk, (u_int)i);
			chunk->bloom_busy = 0;
			break;
		}
	}

	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);
	return (ret);
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
	WT_TXN_ISOLATION saved_isolation;
	u_int i, j;
	int locked;
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	lsm_tree = arg;
	session = lsm_tree->ckpt_session;

	WT_CLEAR(cookie);

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)) {
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_lsm_tree_switch(session, lsm_tree));
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
			__wt_txn_update_oldest(session);
			if (!__wt_txn_visible_all(session, chunk->txnid_max))
				break;

			/*
			 * If the chunk is already checkpointed, make sure it
			 * is also evicted.  Either way, there is no point
			 * trying to checkpoint it again.
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
					WT_ERR_MSG(session, ret,
					    "discard handle");
			}
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				continue;

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker flushing %u", i);

			/*
			 * Flush the file before checkpointing: this is the
			 * expensive part in terms of I/O: do it without
			 * holding the schema lock.
			 *
			 * Use the special eviction isolation level to avoid
			 * interfering with an application checkpoint: we have
			 * already checked that all of the updates in this
			 * chunk are globally visible.
			 *
			 * !!! We can wait here for checkpoints and fsyncs to
			 * complete, which can be a long time.
			 *
			 * Don't keep waiting for the lock if application
			 * threads are waiting for a switch.  Don't skip
			 * flushing the leaves either: that just means we'll
			 * hold the schema lock for (much) longer, which blocks
			 * the world.
			 */
			WT_ERR(__wt_session_get_btree(
			    session, chunk->uri, NULL, NULL, 0));
			for (locked = 0;
			    !locked && ret == 0 &&
			    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH);) {
				if ((ret = __wt_spin_trylock(session,
				    &S2C(session)->checkpoint_lock, &id)) == 0)
					locked = 1;
				else if (ret == EBUSY) {
					__wt_yield();
					ret = 0;
				}
			}
			if (locked) {
				saved_isolation = session->txn.isolation;
				session->txn.isolation = TXN_ISO_EVICTION;
				ret = __wt_bt_cache_op(
				    session, NULL, WT_SYNC_WRITE_LEAVES);
				session->txn.isolation = saved_isolation;
				__wt_spin_unlock(
				    session, &S2C(session)->checkpoint_lock);
			}
			WT_TRET(__wt_session_release_btree(session));
			WT_ERR(ret);

			if (F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
				break;

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointing %u", i);

			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, NULL, NULL, 0));

			if (ret != 0) {
				__wt_err(session, ret, "LSM checkpoint");
				break;
			}

			WT_ERR(__wt_lsm_tree_set_chunk_size(session, chunk));
			/*
			 * Clear the "cache resident" flag so the primary can
			 * be evicted and eventually closed.  Only do this once
			 * the checkpoint has succeeded: otherwise, accessing
			 * the leaf page during the checkpoint can trigger
			 * forced eviction.
			 */
			WT_ERR(__wt_session_get_btree(
			    session, chunk->uri, NULL, NULL, 0));
			__wt_btree_evictable(session, 1);
			WT_ERR(__wt_session_release_btree(session));

			++j;
			WT_ERR(__wt_lsm_tree_lock(session, lsm_tree, 1));
			F_SET(chunk, WT_LSM_CHUNK_ONDISK);
			ret = __wt_lsm_meta_write(session, lsm_tree);
			++lsm_tree->dsk_gen;

			/* Update the throttle time. */
			__wt_lsm_tree_throttle(session, lsm_tree, 1);
			WT_TRET(__wt_lsm_tree_unlock(session, lsm_tree));

			/* Make sure we aren't pinning a transaction ID. */
			__wt_txn_release_snapshot(session);

			if (ret != 0) {
				__wt_err(session, ret,
				    "LSM checkpoint metadata write");
				break;
			}

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointed %u", i);
		}
		__lsm_unpin_chunks(session, &cookie);
		if (j == 0 && F_ISSET(lsm_tree, WT_LSM_TREE_WORKING) &&
		    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
			WT_ERR_TIMEDOUT_OK(__wt_cond_wait(
			    session, lsm_tree->work_cond, 100000));
	}
err:	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);
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
__lsm_bloom_create(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk, u_int chunk_off)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src;
	WT_DECL_RET;
	WT_ITEM buf, key;
	WT_SESSION *wt_session;
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
		chunk->bloom_uri = __wt_buf_steal(session, &buf);
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

	F_SET(session, WT_SESSION_NO_CACHE);
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

	WT_VERBOSE_ERR(session, lsm,
	    "LSM worker created bloom filter %s. "
	    "Expected %" PRIu64 " items, got %" PRIu64,
	    chunk->bloom_uri, chunk->count, insert_count);

	/* Ensure the bloom filter is in the metadata. */
	WT_ERR(__wt_lsm_tree_lock(session, lsm_tree, 1));
	F_SET(chunk, WT_LSM_CHUNK_BLOOM);
	ret = __wt_lsm_meta_write(session, lsm_tree);
	++lsm_tree->dsk_gen;
	WT_TRET(__wt_lsm_tree_unlock(session, lsm_tree));

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
	WT_DECL_SPINLOCK_ID(id);			/* Must appear last */

	/* This will fail with EBUSY if the file is still in use. */
	WT_RET(__wt_session_get_btree(session, uri, checkpoint, NULL,
	    WT_DHANDLE_EXCLUSIVE | WT_DHANDLE_LOCK_ONLY));

	WT_ASSERT(session, S2BT(session)->modified == 0);

	/*
	 * We need the checkpoint lock to discard in-memory handles: otherwise,
	 * an application checkpoint could see this file locked and fail with
	 * EBUSY.
	 *
	 * We can't get the checkpoint lock earlier or it will deadlock with
	 * the schema lock.
	 */
	locked = 0;
	if (checkpoint == NULL && (ret = __wt_spin_trylock(
	    session, &S2C(session)->checkpoint_lock, &id)) == 0)
		locked = 1;
	if (ret == 0)
		F_SET(session->dhandle, WT_DHANDLE_DISCARD);
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
		/* Skip the chunk if another worker is using it. */
		if (chunk->refcnt > 1) {
			++skipped;
			continue;
		}

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
		WT_ERR(__wt_lsm_tree_lock(session, lsm_tree, 1));

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
		/*
		 * Clear the chunk in the cookie so we don't attempt to
		 * decrement the reference count.
		 */
		cookie.chunk_array[i] = NULL;

		/*
		 * Update the metadata.  We used to try to optimize by only
		 * updating the metadata once at the end, but the error
		 * handling is not straightforward.
		 */
		WT_TRET(__wt_lsm_meta_write(session, lsm_tree));
		WT_ERR(__wt_lsm_tree_unlock(session, lsm_tree));
	}

err:	__lsm_unpin_chunks(session, &cookie);
	__wt_free(session, cookie.chunk_array);

	/* Returning non-zero means there is no work to do. */
	if (!progress)
		WT_TRET(WT_NOTFOUND);

	return (ret);
}
