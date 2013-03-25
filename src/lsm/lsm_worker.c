/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_bloom_create(
    WT_SESSION_IMPL *, WT_LSM_TREE *, WT_LSM_CHUNK *);
static int __lsm_free_chunks(WT_SESSION_IMPL *, WT_LSM_TREE *);

/*
 * __wt_lsm_merge_worker --
 *	The merge worker thread for an LSM tree, responsible for merging
 *	on-disk trees.
 */
void *
__wt_lsm_merge_worker(void *vargs)
{
	WT_LSM_WORKER_ARGS *args;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	u_int id;
	int progress, stalls;

	args = vargs;
	lsm_tree = args->lsm_tree;
	id = args->id;
	session = lsm_tree->worker_sessions[id];
	__wt_free(session, args);
	stalls = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		progress = 0;

		/* Clear any state from previous worker thread iterations. */
		session->dhandle = NULL;

		/* Report stalls to merge in seconds. */
		if (__wt_lsm_merge(session, lsm_tree, id, stalls / 1000) == 0)
			progress = 1;

		/* Clear any state from previous worker thread iterations. */
		WT_CLEAR_BTREE_IN_SESSION(session);

		/*
		 * Only have one thread freeing old chunks, and only if there
		 * are chunks to free.
		 */
		if (id == 0 &&
		    lsm_tree->nold_chunks != lsm_tree->old_avail &&
		    __lsm_free_chunks(session, lsm_tree) == 0)
			progress = 1;

		if (progress)
			stalls = 0;
		else {
			__wt_sleep(0, 1000);
			++stalls;
		}
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

	for (;;) {
		WT_ERR(__wt_lsm_copy_chunks(session, lsm_tree, &cookie));
		if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
			goto err;

		/* Create bloom filters in all checkpointed chunks. */
		for (i = 0, j = 0; i < cookie.nchunks; i++) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
				goto err;

			chunk = cookie.chunk_array[i];
			/* Stop if a thread is still active in the chunk. */
			if (chunk->ncursor != 0 ||
			    !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				break;

			if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM) ||
			    F_ISSET(chunk, WT_LSM_CHUNK_MERGING) ||
			    chunk->generation > 0 ||
			    chunk->count == 0)
				continue;

			/*
			 * If a bloom filter create fails restart at the
			 * beginning of the chunk array. Don't exit the thread.
			 */
			if (__lsm_bloom_create(session, lsm_tree, chunk) != 0)
				break;
			++j;
		}
		if (j == 0)
			__wt_sleep(0, 100000);
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

	for (;;) {
		WT_ERR(__wt_lsm_copy_chunks(session, lsm_tree, &cookie));
		if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
			goto err;

		/* Write checkpoints in all completed files. */
		for (i = 0, j = 0; i < cookie.nchunks - 1; i++) {
			if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
				goto err;

			chunk = cookie.chunk_array[i];
			/* Stop if a thread is still active in the chunk. */
			if (chunk->ncursor != 0)
				break;

			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				continue;

			WT_WITH_SCHEMA_LOCK(session,
			    ret = __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, NULL, WT_DHANDLE_DISCARD_CLOSE));

			if (ret != 0) {
				__wt_err(session, ret,
				    "LSM checkpoint failed");
				break;
			}

			++j;
			WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
			F_SET(chunk, WT_LSM_CHUNK_ONDISK);
			++lsm_tree->dsk_gen;
			ret = __wt_lsm_meta_write(session, lsm_tree);
			WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

			if (ret != 0) {
				__wt_err(session, ret,
				    "LSM checkpoint metadata write failed");
				break;
			}

			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointed %u", i);
		}
		if (j == 0)
			__wt_sleep(0, 10000);
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
 * __wt_lsm_copy_chunks --
 *	 Take a copy of part of the LSM tree chunk array so that we can work on
 *	 the contents without holding the LSM tree handle lock long term.
 */
int
__wt_lsm_copy_chunks(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_WORKER_COOKIE *cookie)
{
	WT_DECL_RET;
	u_int nchunks;

	/* Always return zero chunks on error. */
	cookie->nchunks = 0;

	WT_RET(__wt_readlock(session, lsm_tree->rwlock));
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING))
		return (__wt_rwunlock(session, lsm_tree->rwlock));

	/* Take a copy of the current state of the LSM tree. */
	nchunks = lsm_tree->nchunks;

	/*
	 * If the tree array of active chunks is larger than our current buffer,
	 * increase the size of our current buffer to match.
	 */
	if (cookie->chunk_alloc < lsm_tree->chunk_alloc)
		WT_ERR(__wt_realloc(session,
		    &cookie->chunk_alloc, lsm_tree->chunk_alloc,
		    &cookie->chunk_array));
	if (nchunks > 0)
		memcpy(cookie->chunk_array, lsm_tree->chunk,
		    nchunks * sizeof(*lsm_tree->chunk));

err:	WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

	if (ret == 0)
		cookie->nchunks = nchunks;
	return (ret);
}

/*
 * Create a bloom filter for a chunk of the LSM tree that has not yet been
 * merged. Uses a cursor on the yet to be checkpointed in-memory chunk, so
 * the cache should not be excessively churned.
 */
static int
__lsm_bloom_create(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_BLOOM *bloom;
	WT_CURSOR *src;
	WT_DECL_RET;
	WT_ITEM buf, key;
	WT_SESSION *wt_session;
	const char *cur_cfg[] = API_CONF_DEFAULTS(session, open_cursor, "raw");
	uint64_t insert_count;

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
	WT_RET(wt_session->drop(wt_session, chunk->bloom_uri, "force"));

	bloom = NULL;

	WT_RET(__wt_bloom_create(session, chunk->bloom_uri,
	    lsm_tree->bloom_config, chunk->count,
	    lsm_tree->bloom_bit_count, lsm_tree->bloom_hash_count, &bloom));

	WT_ERR(__wt_open_cursor(session, chunk->uri, NULL, cur_cfg, &src));

	for (insert_count = 0; (ret = src->next(src)) == 0; insert_count++) {
		WT_ERR(src->get_key(src, &key));
		WT_ERR(__wt_bloom_insert(bloom, &key));
	}
	WT_ERR_NOTFOUND_OK(ret);
	WT_TRET(src->close(src));

	WT_TRET(__wt_bloom_finalize(bloom));
	WT_ERR(ret);

	WT_VERBOSE_ERR(session, lsm,
	    "LSM worker created bloom filter %s. "
	    "Expected %" PRIu64 " items, got %" PRIu64,
	    chunk->bloom_uri, chunk->count, insert_count);

	F_SET(chunk, WT_LSM_CHUNK_BLOOM);

	/* Ensure the bloom filter is in the metadata. */
	WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
	++lsm_tree->dsk_gen;
	ret = __wt_lsm_meta_write(session, lsm_tree);
	WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

	if (ret != 0)
		WT_ERR_MSG(session, ret,
		    "LSM bloom worker metadata write failed");

err:	if (bloom != NULL)
		WT_TRET(__wt_bloom_close(bloom));
	return (ret);
}

static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	const char *drop_cfg[] = API_CONF_DEFAULTS(session, drop, NULL);
	u_int i;
	int locked, progress;

	locked = progress = 0;
	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		if ((chunk = lsm_tree->old_chunks[i]) == NULL)
			continue;
		if (!locked) {
			locked = 1;
			/* TODO: Do we need the lsm_tree lock for all drops? */
			WT_ERR(__wt_writelock(session, lsm_tree->rwlock));
		}
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_drop(
			    session, chunk->bloom_uri, drop_cfg));
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if (ret == EBUSY) {
				WT_VERBOSE_ERR(session, lsm,
				    "LSM worker bloom drop busy: %s.",
				    chunk->bloom_uri);
				continue;
			} else
				WT_ERR(ret);

			F_CLR(chunk, WT_LSM_CHUNK_BLOOM);
		}
		if (chunk->uri != NULL) {
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_schema_drop(session, chunk->uri, drop_cfg));
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if (ret == EBUSY) {
				WT_VERBOSE_ERR(session, lsm,
				    "LSM worker drop busy: %s.",
				    chunk->uri);
				continue;
			} else
				WT_ERR(ret);
		}

		progress = 1;
		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, lsm_tree->old_chunks[i]);
		++lsm_tree->old_avail;
	}
err:	if (progress)
		WT_TRET(__wt_lsm_meta_write(session, lsm_tree));
	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));

	/* Returning non-zero means there is no work to do. */
	if (!progress)
		WT_TRET(WT_NOTFOUND);

	return (ret);
}
