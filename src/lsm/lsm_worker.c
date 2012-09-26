/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_free_chunks(WT_SESSION_IMPL *, WT_LSM_TREE *);
static int __lsm_copy_chunks(
    WT_LSM_TREE *, size_t *, WT_LSM_CHUNK ***, int *, int);

/*
 * __wt_lsm_worker --
 *	The worker thread for an LSM tree, responsible for writing in-memory
 *	trees to disk and merging on-disk trees.
 */
void *
__wt_lsm_worker(void *arg)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, **chunk_array;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	const char *cfg[] = API_CONF_DEFAULTS(session, checkpoint, NULL);
	size_t chunk_alloc;
	int i, nchunks, progress;

	lsm_tree = arg;
	session = lsm_tree->worker_session;

	chunk_array = NULL;
	chunk_alloc = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		progress = 0;

		WT_ERR(__lsm_copy_chunks(
		    lsm_tree, &chunk_alloc, &chunk_array, &nchunks, 0));

		/*
		 * Write checkpoints in all completed files, then find
		 * something to merge.
		 */
		for (i = 0; i < nchunks; i++) {
			chunk = chunk_array[i];
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) ||
			    chunk->ncursor > 0)
				continue;

			/* XXX durability: need to checkpoint the metadata? */
			/*
			 * NOTE: we pass a non-NULL config, because otherwise
			 * __wt_checkpoint thinks we're closing the file.
			 */
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, cfg, 0));
			if (ret == 0) {
				__wt_spin_lock(session, &lsm_tree->lock);
				F_SET(lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK);
				lsm_tree->dsk_gen++;
				__wt_spin_unlock(session, &lsm_tree->lock);
				progress = 1;
			}
		}

		/* Clear any state from previous worker thread iterations. */
		session->btree = NULL;

		if (__wt_lsm_merge(session, lsm_tree) == 0)
			progress = 1;

		/* Clear any state from previous worker thread iterations. */
		session->btree = NULL;

		if (lsm_tree->nold_chunks != lsm_tree->old_avail &&
		    __lsm_free_chunks(session, lsm_tree) == 0)
			progress = 1;

		if (!progress)
			__wt_sleep(0, 10);
	}

err:	__wt_free(session, chunk_array);

	return (NULL);
}

/*
 * __wt_lsm_checkpoint_worker --
 *	A worker thread for an LSM tree, responsible for checkpointing chunks
 *	once they become read only.
 */
void *
__wt_lsm_checkpoint_worker(void *arg)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, **chunk_array;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	const char *cfg[] = { "name=,drop=", NULL };
	size_t chunk_alloc;
	int i, j, nchunks;

	lsm_tree = arg;
	session = lsm_tree->ckpt_session;

	chunk_array = NULL;
	chunk_alloc = 0;

	while (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		WT_ERR(__lsm_copy_chunks(
		    lsm_tree, &chunk_alloc, &chunk_array, &nchunks, 1));

		/* Write checkpoints in all completed files. */
		for (i = 0, j = 0; i < nchunks; i++) {
			chunk = chunk_array[i];
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
				continue;
			++j;

			/*
			 * NOTE: we pass a non-NULL config, because otherwise
			 * __wt_checkpoint thinks we're closing the file.
			 */
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_schema_worker(session, chunk->uri,
			    __wt_checkpoint, cfg, 0));
			if (ret == 0) {
				__wt_spin_lock(session, &lsm_tree->lock);
				F_SET(lsm_tree->chunk[i], WT_LSM_CHUNK_ONDISK);
				lsm_tree->dsk_gen++;
				__wt_spin_unlock(session, &lsm_tree->lock);
			}
		}
		if (j != 0)
			WT_VERBOSE_ERR(session, lsm,
			     "LSM worker checkpointed %d.", j);
		__wt_sleep(0, 10);
	}
err:	__wt_free(session, chunk_array);

	return (NULL);
}

static int
__lsm_copy_chunks(WT_LSM_TREE *lsm_tree,
    size_t *allocp, WT_LSM_CHUNK ***chunkp, int *nchunkp, int checkpoint)
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	WT_LSM_CHUNK **chunk_array;
	size_t chunk_alloc;
	int nchunks;

	if (checkpoint == 1)
		session = lsm_tree->ckpt_session;
	else
		session = lsm_tree->worker_session;
	chunk_array = *chunkp;
	chunk_alloc = *allocp;

	__wt_spin_lock(session, &lsm_tree->lock);
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		__wt_spin_unlock(session, &lsm_tree->lock);
		/* The actual error value is ignored. */
		return (WT_ERROR);
	}
	/*
	 * Take a copy of the current state of the LSM tree. Skip
	 * the last chunk - since it is the active one and not relevant
	 * to merge operations.
	 */
	nchunks = lsm_tree->nchunks - 1;
	/* Checkpoint doesn't care if there are active cursors, merge does. */
	if (checkpoint == 0) {
		for (; nchunks > 0 && lsm_tree->chunk[nchunks - 1]->ncursor > 0;
		    --nchunks)
			;
	}
	/*
	 * If the tree array of active chunks is larger than our current buffer,
	 * increase the size of our current buffer to match.
	 */
	if (chunk_alloc < lsm_tree->chunk_alloc)
		ret = __wt_realloc(session,
		    &chunk_alloc, lsm_tree->chunk_alloc,
		    &chunk_array);
	if (ret == 0 && nchunks > 0)
		memcpy(chunk_array, lsm_tree->chunk,
		    nchunks * sizeof(*lsm_tree->chunk));
	__wt_spin_unlock(session, &lsm_tree->lock);

	if (ret == 0) {
		*chunkp = chunk_array;
		*allocp = chunk_alloc;
		*nchunkp = nchunks;
	}
	return (ret);
}

static int
__lsm_free_chunks(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	const char *drop_cfg[] = { NULL };
	int found, i;

	found = 0;
	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		if ((chunk = lsm_tree->old_chunks[i]) == NULL)
			continue;
		if (!found) {
			found = 1;
			/* TODO: Do we need the lsm_tree lock for all drops? */
			__wt_spin_lock(session, &lsm_tree->lock);
		}
		if (chunk->bloom_uri != NULL) {
			WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_drop(
			    session, chunk->bloom_uri, drop_cfg));
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if (ret == 0) {
				__wt_free(session, chunk->bloom_uri);
				chunk->bloom_uri = NULL;
			} else if (ret != EBUSY)
				goto err;
			if (ret == EBUSY)
				WT_VERBOSE_ERR(session, lsm,
				    "LSM worker bloom drop busy: %s.",
				    chunk->bloom_uri);
		}
		if (chunk->uri != NULL) {
			WT_WITH_SCHEMA_LOCK(session, ret =
			    __wt_schema_drop(session, chunk->uri, drop_cfg));
			/*
			 * An EBUSY return is acceptable - a cursor may still
			 * be positioned on this old chunk.
			 */
			if (ret == 0) {
				__wt_free(session, chunk->uri);
				chunk->uri = NULL;
			} else if (ret != EBUSY)
				goto err;
		}

		if (chunk->uri == NULL && chunk->bloom_uri == NULL) {
			__wt_free(session, lsm_tree->old_chunks[i]);
			++lsm_tree->old_avail;
		}
	}
	if (found) {
err:		ret = __wt_lsm_meta_write(session, lsm_tree);
		__wt_spin_unlock(session, &lsm_tree->lock);
	}
	/* Returning non-zero means there is no work to do. */
	return (found ? 0 : WT_NOTFOUND);
}
