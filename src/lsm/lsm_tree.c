/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_tree_cleanup_old(WT_SESSION_IMPL *, const char *);
static int __lsm_tree_open_check(WT_SESSION_IMPL *, WT_LSM_TREE *);
static int __lsm_tree_open(
    WT_SESSION_IMPL *, const char *, bool, WT_LSM_TREE **);
static int __lsm_tree_set_name(WT_SESSION_IMPL *, WT_LSM_TREE *, const char *);

/*
 * __lsm_tree_discard --
 *	Free an LSM tree structure.
 */
static int
__lsm_tree_discard(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool final)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	u_int i;

	WT_UNUSED(final);	/* Only used in diagnostic builds */

	WT_ASSERT(session, !lsm_tree->active);
	/*
	 * The work unit queue should be empty, but it's worth checking
	 * since work units use a different locking scheme to regular tree
	 * operations.
	 */
	WT_ASSERT(session, lsm_tree->queue_ref == 0);

	/* We may be destroying an lsm_tree before it was added. */
	if (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
		WT_ASSERT(session, final ||
		    F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));
		TAILQ_REMOVE(&S2C(session)->lsmqh, lsm_tree, q);
	}

	if (lsm_tree->collator_owned &&
	    lsm_tree->collator->terminate != NULL)
		WT_TRET(lsm_tree->collator->terminate(
		    lsm_tree->collator, &session->iface));

	__wt_free(session, lsm_tree->name);
	__wt_free(session, lsm_tree->config);
	__wt_free(session, lsm_tree->key_format);
	__wt_free(session, lsm_tree->value_format);
	__wt_free(session, lsm_tree->collator_name);
	__wt_free(session, lsm_tree->bloom_config);
	__wt_free(session, lsm_tree->file_config);

	WT_TRET(__wt_rwlock_destroy(session, &lsm_tree->rwlock));

	for (i = 0; i < lsm_tree->nchunks; i++) {
		if ((chunk = lsm_tree->chunk[i]) == NULL)
			continue;

		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);
	}
	__wt_free(session, lsm_tree->chunk);

	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		chunk = lsm_tree->old_chunks[i];
		WT_ASSERT(session, chunk != NULL);

		__wt_free(session, chunk->bloom_uri);
		__wt_free(session, chunk->uri);
		__wt_free(session, chunk);
	}
	__wt_free(session, lsm_tree->old_chunks);
	__wt_free(session, lsm_tree);

	return (ret);
}

/*
 * __lsm_tree_close --
 *	Close an LSM tree structure.
 */
static int
__lsm_tree_close(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool final)
{
	WT_DECL_RET;
	int i;

	/*
	 * Stop any new work units being added. The barrier is necessary
	 * because we rely on the state change being visible before checking
	 * the tree queue state.
	 */
	lsm_tree->active = false;
	WT_READ_BARRIER();

	/*
	 * Wait for all LSM operations to drain. If WiredTiger is shutting
	 * down also wait for the tree reference count to go to zero, otherwise
	 * we know a user is holding a reference to the tree, so exclusive
	 * access is not available.
	 */
	for (i = 0;
	    lsm_tree->queue_ref > 0 || (final && lsm_tree->refcnt > 1); ++i) {
		/*
		 * Remove any work units from the manager queues. Do this step
		 * repeatedly in case a work unit was in the process of being
		 * created when we cleared the active flag.
		 *
		 * !!! Drop the schema and handle list locks whilst completing
		 * this step so that we don't block any operations that require
		 * the schema lock to complete. This is safe because any
		 * operation that is closing the tree should first have gotten
		 * exclusive access to the LSM tree via __wt_lsm_tree_get, so
		 * other schema level operations will return EBUSY, even though
		 * we're dropping the schema lock here.
		 */
		if (i % WT_THOUSAND == 0) {
			WT_WITHOUT_LOCKS(session, ret =
			    __wt_lsm_manager_clear_tree(session, lsm_tree));
			WT_ERR(ret);
		}
		__wt_yield();
	}
	return (0);

err:	lsm_tree->active = true;
	return (ret);
}

/*
 * __wt_lsm_tree_close_all --
 *	Close all LSM tree structures.
 */
int
__wt_lsm_tree_close_all(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	/* We are shutting down: the handle list lock isn't required. */

	while ((lsm_tree = TAILQ_FIRST(&S2C(session)->lsmqh)) != NULL) {
		/*
		 * Tree close assumes that we have a reference to the tree
		 * so it can tell when it's safe to do the close. We could
		 * get the tree here, but we short circuit instead. There
		 * is no need to decrement the reference count since discard
		 * is unconditional.
		 */
		(void)__wt_atomic_add32(&lsm_tree->refcnt, 1);
		WT_TRET(__lsm_tree_close(session, lsm_tree, true));
		WT_TRET(__lsm_tree_discard(session, lsm_tree, true));
	}

	return (ret);
}

/*
 * __lsm_tree_set_name --
 *	Set or reset the name of an LSM tree
 */
static int
__lsm_tree_set_name(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, const char *uri)
{
	void *p;

	WT_RET(__wt_strdup(session, uri, &p));

	__wt_free(session, lsm_tree->name);
	lsm_tree->name = p;
	lsm_tree->filename = lsm_tree->name + strlen("lsm:");
	return (0);
}

/*
 * __wt_lsm_tree_bloom_name --
 *	Get the URI of the Bloom filter for a given chunk.
 */
int
__wt_lsm_tree_bloom_name(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, uint32_t id, const char **retp)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_buf_fmt(
	    session, tmp, "file:%s-%06" PRIu32 ".bf", lsm_tree->filename, id));
	WT_ERR(__wt_strndup(session, tmp->data, tmp->size, retp));

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_lsm_tree_chunk_name --
 *	Get the URI of the file for a given chunk.
 */
int
__wt_lsm_tree_chunk_name(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, uint32_t id, const char **retp)
{
	WT_DECL_ITEM(tmp);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &tmp));
	WT_ERR(__wt_buf_fmt(
	    session, tmp, "file:%s-%06" PRIu32 ".lsm", lsm_tree->filename, id));
	WT_ERR(__wt_strndup(session, tmp->data, tmp->size, retp));

err:	__wt_scr_free(session, &tmp);
	return (ret);
}

/*
 * __wt_lsm_tree_set_chunk_size --
 *	Set the size of the chunk. Should only be called for chunks that are
 *	on disk, or about to become on disk.
 */
int
__wt_lsm_tree_set_chunk_size(
    WT_SESSION_IMPL *session, WT_LSM_CHUNK *chunk)
{
	wt_off_t size;
	const char *filename;

	filename = chunk->uri;
	if (!WT_PREFIX_SKIP(filename, "file:"))
		WT_RET_MSG(session, EINVAL,
		    "Expected a 'file:' URI: %s", chunk->uri);
	WT_RET(__wt_fs_size(session, filename, &size));

	chunk->size = (uint64_t)size;

	return (0);
}

/*
 * __lsm_tree_cleanup_old --
 *	Cleanup any old LSM chunks that might conflict with one we are
 *	about to create. Sometimes failed LSM metadata operations can
 *	leave old files and bloom filters behind.
 */
static int
__lsm_tree_cleanup_old(WT_SESSION_IMPL *session, const char *uri)
{
	WT_DECL_RET;
	const char *cfg[] =
	    { WT_CONFIG_BASE(session, WT_SESSION_drop), "force", NULL };
	bool exists;

	WT_RET(__wt_fs_exist(session, uri + strlen("file:"), &exists));
	if (exists)
		WT_WITH_SCHEMA_LOCK(session, ret,
		    ret = __wt_schema_drop(session, uri, cfg));
	return (ret);
}

/*
 * __wt_lsm_tree_setup_chunk --
 *	Initialize a chunk of an LSM tree.
 */
int
__wt_lsm_tree_setup_chunk(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_SCHEMA));
	WT_RET(__wt_epoch(session, &chunk->create_ts));

	WT_RET(__wt_lsm_tree_chunk_name(
	    session, lsm_tree, chunk->id, &chunk->uri));

	/*
	 * If the underlying file exists, drop the chunk first - there may be
	 * some content hanging over from an aborted merge or checkpoint.
	 *
	 * Don't do this for the very first chunk: we are called during
	 * WT_SESSION::create, and doing a drop inside there does interesting
	 * things with handle locks and metadata tracking.  It can never have
	 * been the result of an interrupted merge, anyway.
	 */
	if (chunk->id > 1)
		WT_RET(__lsm_tree_cleanup_old(session, chunk->uri));

	return (__wt_schema_create(session, chunk->uri, lsm_tree->file_config));
}

/*
 * __wt_lsm_tree_setup_bloom --
 *	Initialize a bloom filter for an LSM tree.
 */
int
__wt_lsm_tree_setup_bloom(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_DECL_RET;

	/*
	 * The Bloom URI can be populated when the chunk is created, but
	 * it isn't set yet on open or merge.
	 */
	if (chunk->bloom_uri == NULL)
		WT_RET(__wt_lsm_tree_bloom_name(
		    session, lsm_tree, chunk->id, &chunk->bloom_uri));
	WT_RET(__lsm_tree_cleanup_old(session, chunk->bloom_uri));
	return (ret);
}

/*
 * __wt_lsm_tree_create --
 *	Create an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_create(WT_SESSION_IMPL *session,
    const char *uri, bool exclusive, const char *config)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	const char *cfg[] =
	    { WT_CONFIG_BASE(session, lsm_meta), config, NULL };
	const char *metadata;

	metadata = NULL;

	/* If the tree can be opened, it already exists. */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, uri, false, &lsm_tree));
	if (ret == 0) {
		__wt_lsm_tree_release(session, lsm_tree);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	if (!F_ISSET(S2C(session), WT_CONN_READONLY)) {
		WT_ERR(__wt_config_merge(session, cfg, NULL, &metadata));
		WT_ERR(__wt_metadata_insert(session, uri, metadata));
	}

	/*
	 * Open our new tree and add it to the handle cache. Don't discard on
	 * error: the returned handle is NULL on error, and the metadata
	 * tracking macros handle cleaning up on failure.
	 */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __lsm_tree_open(session, uri, true, &lsm_tree));
	if (ret == 0)
		__wt_lsm_tree_release(session, lsm_tree);

err:	__wt_free(session, metadata);
	return (ret);
}

/*
 * __lsm_tree_find --
 *	Find an LSM tree structure for the given name. Optionally get exclusive
 *	access to the handle. Exclusive access works separately to the LSM tree
 *	lock - since operations that need exclusive access may also need to
 *	take the LSM tree lock for example outstanding work unit operations.
 */
static int
__lsm_tree_find(WT_SESSION_IMPL *session,
    const char *uri, bool exclusive, WT_LSM_TREE **treep)
{
	WT_LSM_TREE *lsm_tree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));

	/* See if the tree is already open. */
	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if (strcmp(uri, lsm_tree->name) == 0) {
			if (exclusive) {
				/*
				 * Make sure we win the race to switch on the
				 * exclusive flag.
				 */
				if (!__wt_atomic_cas_ptr(
				    &lsm_tree->excl_session, NULL, session))
					return (EBUSY);

				/*
				 * Drain the work queue before checking for
				 * open cursors - otherwise we can generate
				 * spurious busy returns.
				 */
				(void)__wt_atomic_add32(&lsm_tree->refcnt, 1);
				if (__lsm_tree_close(
				    session, lsm_tree, false) != 0 ||
				    lsm_tree->refcnt != 1) {
					__wt_lsm_tree_release(
					    session, lsm_tree);
					return (EBUSY);
				}
			} else {
				(void)__wt_atomic_add32(&lsm_tree->refcnt, 1);

				/*
				 * We got a reference, check if an exclusive
				 * lock beat us to it.
				 */
				if (lsm_tree->excl_session != NULL) {
					WT_ASSERT(session,
					    lsm_tree->refcnt > 0);
					__wt_lsm_tree_release(
					    session, lsm_tree);
					return (EBUSY);
				}
			}

			*treep = lsm_tree;
			return (0);
		}

	return (WT_NOTFOUND);
}

/*
 * __lsm_tree_open_check --
 *	Validate the configuration of an LSM tree.
 */
static int
__lsm_tree_open_check(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_CONFIG_ITEM cval;
	uint64_t maxleafpage, required;
	const char *cfg[] = { WT_CONFIG_BASE(
	    session, WT_SESSION_create), lsm_tree->file_config, NULL };

	WT_RET(__wt_config_gets(session, cfg, "leaf_page_max", &cval));
	maxleafpage = (uint64_t)cval.val;

	/*
	 * Three chunks, plus one page for each participant in up to three
	 * concurrent merges.
	 */
	required = 3 * lsm_tree->chunk_size +
	    3 * (lsm_tree->merge_max * maxleafpage);
	if (S2C(session)->cache_size < required)
		WT_RET_MSG(session, EINVAL,
		    "LSM cache size %" PRIu64 " (%" PRIu64 "MB) too small, "
		    "must be at least %" PRIu64 " (%" PRIu64 "MB)",
		    S2C(session)->cache_size,
		    S2C(session)->cache_size / WT_MEGABYTE,
		    required, required / WT_MEGABYTE);
	return (0);
}

/*
 * __lsm_tree_open --
 *	Open an LSM tree structure.
 */
static int
__lsm_tree_open(WT_SESSION_IMPL *session,
    const char *uri, bool exclusive, WT_LSM_TREE **treep)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	conn = S2C(session);
	lsm_tree = NULL;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));

	/* Start the LSM manager thread if it isn't running. */
	if (__wt_atomic_cas32(&conn->lsm_manager.lsm_workers, 0, 1))
		WT_RET(__wt_lsm_manager_start(session));

	/* Make sure no one beat us to it. */
	if ((ret = __lsm_tree_find(
	    session, uri, exclusive, treep)) != WT_NOTFOUND)
		return (ret);

	/* Try to open the tree. */
	WT_RET(__wt_calloc_one(session, &lsm_tree));
	WT_ERR(__wt_rwlock_alloc(session, &lsm_tree->rwlock, "lsm tree"));

	WT_ERR(__lsm_tree_set_name(session, lsm_tree, uri));

	WT_ERR(__wt_lsm_meta_read(session, lsm_tree));

	/*
	 * Sanity check the configuration. Do it now since this is the first
	 * time we have the LSM tree configuration.
	 */
	WT_ERR(__lsm_tree_open_check(session, lsm_tree));

	/* Set the generation number so cursors are opened on first usage. */
	lsm_tree->dsk_gen = 1;

	/*
	 * Setup reference counting. Use separate reference counts for tree
	 * handles and queue entries, so that queue entries don't interfere
	 * with getting handles exclusive.
	 */
	lsm_tree->refcnt = 1;
	lsm_tree->excl_session = exclusive ? session : NULL;
	lsm_tree->queue_ref = 0;

	/* Set a flush timestamp as a baseline. */
	WT_ERR(__wt_epoch(session, &lsm_tree->last_flush_ts));

	/* Now the tree is setup, make it visible to others. */
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);
	if (!exclusive)
		lsm_tree->active = true;
	F_SET(lsm_tree, WT_LSM_TREE_OPEN);

	*treep = lsm_tree;

	if (0) {
err:		WT_TRET(__lsm_tree_discard(session, lsm_tree, false));
	}
	return (ret);
}

/*
 * __wt_lsm_tree_get --
 *	Find an LSM tree handle or open a new one.
 */
int
__wt_lsm_tree_get(WT_SESSION_IMPL *session,
    const char *uri, bool exclusive, WT_LSM_TREE **treep)
{
	WT_DECL_RET;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_LOCKED_HANDLE_LIST));

	ret = __lsm_tree_find(session, uri, exclusive, treep);
	if (ret == WT_NOTFOUND)
		ret = __lsm_tree_open(session, uri, exclusive, treep);

	WT_ASSERT(session, ret != 0 ||
	     (*treep)->excl_session == (exclusive ? session : NULL));
	return (ret);
}

/*
 * __wt_lsm_tree_release --
 *	Release an LSM tree structure.
 */
void
__wt_lsm_tree_release(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_ASSERT(session, lsm_tree->refcnt > 0);
	if (lsm_tree->excl_session == session) {
		/* We cleared the active flag when getting exclusive access. */
		lsm_tree->active = true;
		lsm_tree->excl_session = NULL;
	}
	(void)__wt_atomic_sub32(&lsm_tree->refcnt, 1);
}

/* How aggressively to ramp up or down throttle due to level 0 merging */
#define	WT_LSM_MERGE_THROTTLE_BUMP_PCT	(100 / lsm_tree->merge_max)
/* Number of level 0 chunks that need to be present to throttle inserts */
#define	WT_LSM_MERGE_THROTTLE_THRESHOLD					\
	(2 * lsm_tree->merge_min)
/* Minimal throttling time */
#define	WT_LSM_THROTTLE_START		20

#define	WT_LSM_MERGE_THROTTLE_INCREASE(val)	do {			\
	(val) += ((val) * WT_LSM_MERGE_THROTTLE_BUMP_PCT) / 100;	\
	if ((val) < WT_LSM_THROTTLE_START)				\
		(val) = WT_LSM_THROTTLE_START;				\
	} while (0)

#define	WT_LSM_MERGE_THROTTLE_DECREASE(val)	do {			\
	(val) -= ((val) * WT_LSM_MERGE_THROTTLE_BUMP_PCT) / 100;	\
	if ((val) < WT_LSM_THROTTLE_START)				\
		(val) = 0;						\
	} while (0)

/*
 * __wt_lsm_tree_throttle --
 *	Calculate whether LSM updates need to be throttled. Must be called
 *	with the LSM tree lock held.
 */
void
__wt_lsm_tree_throttle(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, bool decrease_only)
{
	WT_LSM_CHUNK *last_chunk, **cp, *ondisk, *prev_chunk;
	uint64_t cache_sz, cache_used, oldtime, record_count, timediff;
	uint32_t in_memory, gen0_chunks;

	/* Never throttle in small trees. */
	if (lsm_tree->nchunks < 3) {
		lsm_tree->ckpt_throttle = lsm_tree->merge_throttle = 0;
		return;
	}

	cache_sz = S2C(session)->cache_size;

	/*
	 * In the steady state, we expect that the checkpoint worker thread
	 * will keep up with inserts.  If not, throttle the insert rate to
	 * avoid filling the cache with in-memory chunks.  Threads sleep every
	 * 100 operations, so take that into account in the calculation.
	 *
	 * Also throttle based on whether merge threads are keeping up.  If
	 * there are enough chunks that have never been merged we slow down
	 * inserts so that merges have some chance of keeping up.
	 *
	 * Count the number of in-memory chunks, the number of unmerged chunk
	 * on disk, and find the most recent on-disk chunk (if any).
	 */
	record_count = 1;
	gen0_chunks = in_memory = 0;
	ondisk = NULL;
	for (cp = lsm_tree->chunk + lsm_tree->nchunks - 1;
	    cp >= lsm_tree->chunk;
	    --cp)
		if (!F_ISSET(*cp, WT_LSM_CHUNK_ONDISK)) {
			record_count += (*cp)->count;
			++in_memory;
		} else {
			/*
			 * Assign ondisk to the last chunk that has been
			 * flushed since the tree was last opened (i.e it's on
			 * disk and stable is not set).
			 */
			if (ondisk == NULL &&
			    ((*cp)->generation == 0 &&
			    !F_ISSET(*cp, WT_LSM_CHUNK_STABLE)))
				ondisk = *cp;

			if ((*cp)->generation == 0 &&
			    !F_ISSET(*cp, WT_LSM_CHUNK_MERGING))
				++gen0_chunks;
		}

	last_chunk = lsm_tree->chunk[lsm_tree->nchunks - 1];

	/* Checkpoint throttling, based on the number of in-memory chunks. */
	if (!F_ISSET(lsm_tree, WT_LSM_TREE_THROTTLE) || in_memory <= 3)
		lsm_tree->ckpt_throttle = 0;
	else if (decrease_only)
		; /* Nothing to do */
	else if (ondisk == NULL) {
		/*
		 * No checkpoint has completed this run.  Keep slowing down
		 * inserts until one does.
		 */
		lsm_tree->ckpt_throttle =
		    WT_MAX(WT_LSM_THROTTLE_START, 2 * lsm_tree->ckpt_throttle);
	} else {
		WT_ASSERT(session,
		    WT_TIMECMP(last_chunk->create_ts, ondisk->create_ts) >= 0);
		timediff =
		    WT_TIMEDIFF_NS(last_chunk->create_ts, ondisk->create_ts);
		lsm_tree->ckpt_throttle =
		    (in_memory - 2) * timediff / (20 * record_count);

		/*
		 * Get more aggressive as the number of in memory chunks
		 * consumes a large proportion of the cache. In memory chunks
		 * are allowed to grow up to twice as large as the configured
		 * value when checkpoints aren't keeping up. That worst case
		 * is when this calculation is relevant.
		 * There is nothing particularly special about the chosen
		 * multipliers.
		 */
		cache_used = in_memory * lsm_tree->chunk_size * 2;
		if (cache_used > cache_sz * 0.8)
			lsm_tree->ckpt_throttle *= 5;
	}

	/*
	 * Merge throttling, based on the number of on-disk, level 0 chunks.
	 *
	 * Don't throttle if the tree has less than a single level's number
	 * of chunks.
	 */
	if (F_ISSET(lsm_tree, WT_LSM_TREE_MERGES)) {
		if (lsm_tree->nchunks < lsm_tree->merge_max)
			lsm_tree->merge_throttle = 0;
		else if (gen0_chunks < WT_LSM_MERGE_THROTTLE_THRESHOLD)
			WT_LSM_MERGE_THROTTLE_DECREASE(
			    lsm_tree->merge_throttle);
		else if (!decrease_only)
			WT_LSM_MERGE_THROTTLE_INCREASE(
			    lsm_tree->merge_throttle);
	}

	/* Put an upper bound of 1s on both throttle calculations. */
	lsm_tree->ckpt_throttle = WT_MIN(WT_MILLION, lsm_tree->ckpt_throttle);
	lsm_tree->merge_throttle = WT_MIN(WT_MILLION, lsm_tree->merge_throttle);

	/*
	 * Update our estimate of how long each in-memory chunk stays active.
	 * Filter out some noise by keeping a weighted history of the
	 * calculated value.  Wait until we have enough chunks that we can
	 * check that the new value is sane: otherwise, after a long idle
	 * period, we can calculate a crazy value.
	 */
	if (in_memory > 1 && ondisk != NULL) {
		prev_chunk = lsm_tree->chunk[lsm_tree->nchunks - 2];
		WT_ASSERT(session, prev_chunk->generation == 0);
		WT_ASSERT(session, WT_TIMECMP(
		    last_chunk->create_ts, prev_chunk->create_ts) >= 0);
		timediff = WT_TIMEDIFF_NS(
		    last_chunk->create_ts, prev_chunk->create_ts);
		WT_ASSERT(session,
		    WT_TIMECMP(prev_chunk->create_ts, ondisk->create_ts) >= 0);
		oldtime = WT_TIMEDIFF_NS(
		    prev_chunk->create_ts, ondisk->create_ts);
		if (timediff < 10 * oldtime)
			lsm_tree->chunk_fill_ms =
			    (3 * lsm_tree->chunk_fill_ms +
			    timediff / WT_MILLION) / 4;
	}
}

/*
 * __wt_lsm_tree_switch --
 *	Switch to a new in-memory tree.
 */
int
__wt_lsm_tree_switch(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk, *last_chunk;
	uint32_t chunks_moved, nchunks, new_id;
	bool first_switch;

	WT_RET(__wt_lsm_tree_writelock(session, lsm_tree));

	nchunks = lsm_tree->nchunks;

	first_switch = nchunks == 0;

	/*
	 * Check if a switch is still needed: we may have raced while waiting
	 * for a lock.
	 */
	last_chunk = NULL;
	if (!first_switch &&
	    (last_chunk = lsm_tree->chunk[nchunks - 1]) != NULL &&
	    !F_ISSET(last_chunk, WT_LSM_CHUNK_ONDISK) &&
	    !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))
		goto err;

	/* Update the throttle time. */
	__wt_lsm_tree_throttle(session, lsm_tree, false);

	new_id = __wt_atomic_add32(&lsm_tree->last, 1);

	WT_ERR(__wt_realloc_def(session, &lsm_tree->chunk_alloc,
	    nchunks + 1, &lsm_tree->chunk));

	WT_ERR(__wt_verbose(session, WT_VERB_LSM,
	    "Tree %s switch to: %" PRIu32 ", checkpoint throttle %" PRIu64
	    ", merge throttle %" PRIu64, lsm_tree->name,
	    new_id, lsm_tree->ckpt_throttle, lsm_tree->merge_throttle));

	WT_ERR(__wt_calloc_one(session, &chunk));
	chunk->id = new_id;
	chunk->switch_txn = WT_TXN_NONE;
	lsm_tree->chunk[lsm_tree->nchunks++] = chunk;
	WT_ERR(__wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	F_CLR(lsm_tree, WT_LSM_TREE_NEED_SWITCH);
	++lsm_tree->dsk_gen;

	lsm_tree->modified = true;

	/*
	 * Set the switch transaction in the previous chunk unless this is
	 * the first chunk in a new or newly opened tree.
	 */
	if (last_chunk != NULL && last_chunk->switch_txn == WT_TXN_NONE &&
	    !F_ISSET(last_chunk, WT_LSM_CHUNK_ONDISK))
		last_chunk->switch_txn = __wt_txn_id_alloc(session, false);

	/*
	 * If a maximum number of chunks are configured, drop the any chunks
	 * past the limit.
	 */
	if (lsm_tree->chunk_count_limit != 0 &&
	    lsm_tree->nchunks > lsm_tree->chunk_count_limit) {
		chunks_moved = lsm_tree->nchunks - lsm_tree->chunk_count_limit;
		/* Move the last chunk onto the old chunk list. */
		WT_ERR(__wt_lsm_tree_retire_chunks(
		    session, lsm_tree, 0, chunks_moved));

		/* Update the active chunk list. */
		lsm_tree->nchunks -= chunks_moved;
		/* Move the remaining chunks to the start of the active list */
		memmove(lsm_tree->chunk,
		    lsm_tree->chunk + chunks_moved,
		    lsm_tree->nchunks * sizeof(*lsm_tree->chunk));
		/* Clear out the chunks at the end of the tree */
		memset(lsm_tree->chunk + lsm_tree->nchunks,
		    0, chunks_moved * sizeof(*lsm_tree->chunk));

		/* Make sure the manager knows there is work to do. */
		WT_ERR(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_DROP, 0, lsm_tree));
	}

err:	WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	/*
	 * Errors that happen during a tree switch leave the tree in a state
	 * where we can't make progress. Error out of WiredTiger.
	 */
	if (ret != 0)
		WT_PANIC_RET(session, ret, "Failed doing LSM switch");
	else if (!first_switch)
		WT_RET(__wt_lsm_manager_push_entry(
		    session, WT_LSM_WORK_FLUSH, 0, lsm_tree));
	return (ret);
}

/*
 * __wt_lsm_tree_retire_chunks --
 *	Move a set of chunks onto the old chunks list.
 *	It's the callers responsibility to update the active chunks list.
 *	Must be called with the LSM lock held.
 */
int
__wt_lsm_tree_retire_chunks(WT_SESSION_IMPL *session,
    WT_LSM_TREE *lsm_tree, u_int start_chunk, u_int nchunks)
{
	u_int i;

	WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);

	/* Setup the array of obsolete chunks. */
	WT_RET(__wt_realloc_def(session, &lsm_tree->old_alloc,
	    lsm_tree->nold_chunks + nchunks, &lsm_tree->old_chunks));

	/* Copy entries one at a time, so we can reuse gaps in the list. */
	for (i = 0; i < nchunks; i++)
		lsm_tree->old_chunks[lsm_tree->nold_chunks++] =
		    lsm_tree->chunk[start_chunk + i];

	return (0);
}

/*
 * __wt_lsm_tree_drop --
 *	Drop an LSM tree.
 */
int
__wt_lsm_tree_drop(
    WT_SESSION_IMPL *session, const char *name, const char *cfg[])
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	int tret;
	u_int i;
	bool locked;

	locked = false;

	/* Get the LSM tree. */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, name, true, &lsm_tree));
	WT_RET(ret);
	WT_ASSERT(session, !lsm_tree->active);

	/* Prevent any new opens. */
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = true;

	/* Drop the chunks. */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		WT_ERR(__wt_schema_drop(session, chunk->uri, cfg));
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(
			    __wt_schema_drop(session, chunk->bloom_uri, cfg));
	}

	/* Drop any chunks on the obsolete list. */
	for (i = 0; i < lsm_tree->nold_chunks; i++) {
		if ((chunk = lsm_tree->old_chunks[i]) == NULL)
			continue;
		WT_ERR(__wt_schema_drop(session, chunk->uri, cfg));
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(
			    __wt_schema_drop(session, chunk->bloom_uri, cfg));
	}

	locked = false;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));
	ret = __wt_metadata_remove(session, name);

	WT_ASSERT(session, !lsm_tree->active);
err:	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	WT_WITH_HANDLE_LIST_LOCK(session,
	    tret = __lsm_tree_discard(session, lsm_tree, false));
	WT_TRET(tret);
	return (ret);
}

/*
 * __wt_lsm_tree_rename --
 *	Rename an LSM tree.
 */
int
__wt_lsm_tree_rename(WT_SESSION_IMPL *session,
    const char *olduri, const char *newuri, const char *cfg[])
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	const char *old;
	int tret;
	u_int i;
	bool locked;

	old = NULL;
	locked = false;

	/* Get the LSM tree. */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, olduri, true, &lsm_tree));
	WT_RET(ret);

	/* Prevent any new opens. */
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = true;

	/* Set the new name. */
	WT_ERR(__lsm_tree_set_name(session, lsm_tree, newuri));

	/* Rename the chunks. */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		old = chunk->uri;
		chunk->uri = NULL;

		WT_ERR(__wt_lsm_tree_chunk_name(
		    session, lsm_tree, chunk->id, &chunk->uri));
		WT_ERR(__wt_schema_rename(session, old, chunk->uri, cfg));
		__wt_free(session, old);

		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			old = chunk->bloom_uri;
			chunk->bloom_uri = NULL;
			WT_ERR(__wt_lsm_tree_bloom_name(
			    session, lsm_tree, chunk->id, &chunk->bloom_uri));
			F_SET(chunk, WT_LSM_CHUNK_BLOOM);
			WT_ERR(__wt_schema_rename(
			    session, old, chunk->uri, cfg));
			__wt_free(session, old);
		}
	}

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	locked = false;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));
	WT_ERR(__wt_metadata_remove(session, olduri));

err:	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	__wt_free(session, old);

	/*
	 * Discard this LSM tree structure. The first operation on the renamed
	 * tree will create a new one.
	 */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    tret = __lsm_tree_discard(session, lsm_tree, false));
	WT_TRET(tret);
	return (ret);
}

/*
 * __wt_lsm_tree_truncate --
 *	Truncate an LSM tree.
 */
int
__wt_lsm_tree_truncate(
    WT_SESSION_IMPL *session, const char *name, const char *cfg[])
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	int tret;
	bool locked;

	WT_UNUSED(cfg);
	chunk = NULL;
	locked = false;

	/* Get the LSM tree. */
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, name, true, &lsm_tree));
	WT_RET(ret);

	/* Prevent any new opens. */
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = true;

	/* Create the new chunk. */
	WT_ERR(__wt_calloc_one(session, &chunk));
	chunk->id = __wt_atomic_add32(&lsm_tree->last, 1);
	WT_ERR(__wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));

	/* Mark all chunks old. */
	WT_ERR(__wt_lsm_merge_update_tree(
	    session, lsm_tree, 0, lsm_tree->nchunks, chunk));

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));

	locked = false;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));
	__wt_lsm_tree_release(session, lsm_tree);

err:	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));
	if (ret != 0) {
		if (chunk != NULL) {
			(void)__wt_schema_drop(session, chunk->uri, NULL);
			__wt_free(session, chunk);
		}
		/*
		 * Discard the LSM tree structure on error. This will force the
		 * LSM tree to be re-opened the next time it is accessed and
		 * the last good version of the metadata will be used, resulting
		 * in a valid (not truncated) tree.
		 */
		WT_WITH_HANDLE_LIST_LOCK(session,
		    tret = __lsm_tree_discard(session, lsm_tree, false));
		WT_TRET(tret);
	}
	return (ret);
}

/*
 * __wt_lsm_tree_readlock --
 *	Acquire a shared lock on an LSM tree.
 */
int
__wt_lsm_tree_readlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_RET(__wt_readlock(session, lsm_tree->rwlock));

	/*
	 * Diagnostic: avoid deadlocks with the schema lock: if we need it for
	 * an operation, we should already have it.
	 */
	F_SET(session, WT_SESSION_NO_EVICTION | WT_SESSION_NO_SCHEMA_LOCK);
	return (0);
}

/*
 * __wt_lsm_tree_readunlock --
 *	Release a shared lock on an LSM tree.
 */
int
__wt_lsm_tree_readunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;

	F_CLR(session, WT_SESSION_NO_EVICTION | WT_SESSION_NO_SCHEMA_LOCK);

	if ((ret = __wt_readunlock(session, lsm_tree->rwlock)) != 0)
		WT_PANIC_RET(session, ret, "Unlocking an LSM tree");
	return (0);
}

/*
 * __wt_lsm_tree_writelock --
 *	Acquire an exclusive lock on an LSM tree.
 */
int
__wt_lsm_tree_writelock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_RET(__wt_writelock(session, lsm_tree->rwlock));

	/*
	 * Diagnostic: avoid deadlocks with the schema lock: if we need it for
	 * an operation, we should already have it.
	 */
	F_SET(session, WT_SESSION_NO_EVICTION | WT_SESSION_NO_SCHEMA_LOCK);
	return (0);
}

/*
 * __wt_lsm_tree_writeunlock --
 *	Release an exclusive lock on an LSM tree.
 */
int
__wt_lsm_tree_writeunlock(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;

	F_CLR(session, WT_SESSION_NO_EVICTION | WT_SESSION_NO_SCHEMA_LOCK);

	if ((ret = __wt_writeunlock(session, lsm_tree->rwlock)) != 0)
		WT_PANIC_RET(session, ret, "Unlocking an LSM tree");
	return (0);
}

/*
 * __wt_lsm_compact --
 *	Compact an LSM tree called via __wt_schema_worker.
 */
int
__wt_lsm_compact(WT_SESSION_IMPL *session, const char *name, bool *skipp)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	time_t begin, end;
	uint64_t progress;
	uint32_t i;
	bool compacting, flushing, locked, ref;

	compacting = flushing = locked = ref = false;
	chunk = NULL;
	/*
	 * This function is applied to all matching sources: ignore anything
	 * that is not an LSM tree.
	 */
	if (!WT_PREFIX_MATCH(name, "lsm:"))
		return (0);

	/* Tell __wt_schema_worker not to look inside the LSM tree. */
	*skipp = true;

	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, name, false, &lsm_tree));
	WT_RET(ret);

	if (!F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
		WT_ERR_MSG(session, EINVAL,
		    "LSM compaction requires active merge threads");

	/*
	 * There is no work to do if there is only a single chunk in the tree
	 * and it has a bloom filter or is configured to never have a bloom
	 * filter.
	 */
	if (lsm_tree->nchunks == 1 &&
	    (!FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST) ||
	    F_ISSET(lsm_tree->chunk[0], WT_LSM_CHUNK_BLOOM))) {
		__wt_lsm_tree_release(session, lsm_tree);
		return (0);
	}

	WT_ERR(__wt_seconds(session, &begin));

	/*
	 * Compacting has two distinct phases.
	 * 1.  All in-memory chunks up to and including the current
	 * current chunk must be flushed.  Normally, the flush code
	 * does not flush the last, in-use chunk, so we set a force
	 * flag to include that last chunk.  We monitor the state of the
	 * last chunk and periodically push another forced flush work
	 * unit until it is complete.
	 * 2.  After all flushing is done, we move onto the merging
	 * phase for compaction.  Again, we monitor the state and
	 * continue to push merge work units until all merging is done.
	 */

	/* Lock the tree: single-thread compaction. */
	WT_ERR(__wt_lsm_tree_writelock(session, lsm_tree));
	locked = true;

	/* Clear any merge throttle: compact throws out that calculation. */
	lsm_tree->merge_throttle = 0;
	lsm_tree->merge_aggressiveness = 0;
	progress = lsm_tree->merge_progressing;

	/* If another thread started a compact on this tree, we're done. */
	if (F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING))
		goto err;

	/*
	 * Set the switch transaction on the current chunk, if it
	 * hasn't been set before.  This prevents further writes, so it
	 * can be flushed by the checkpoint worker.
	 */
	if (lsm_tree->nchunks > 0 &&
	    (chunk = lsm_tree->chunk[lsm_tree->nchunks - 1]) != NULL) {
		if (chunk->switch_txn == WT_TXN_NONE)
			chunk->switch_txn = __wt_txn_id_alloc(session, false);
		/*
		 * If we have a chunk, we want to look for it to be on-disk.
		 * So we need to add a reference to keep it available.
		 */
		(void)__wt_atomic_add32(&chunk->refcnt, 1);
		ref = true;
	}

	locked = false;
	WT_ERR(__wt_lsm_tree_writeunlock(session, lsm_tree));

	if (chunk != NULL) {
		WT_ERR(__wt_verbose(session, WT_VERB_LSM,
		    "Compact force flush %s flags 0x%" PRIx32
		    " chunk %" PRIu32 " flags 0x%" PRIx32,
		    name, lsm_tree->flags, chunk->id, chunk->flags));
		flushing = true;
		/*
		 * Make sure the in-memory chunk gets flushed do not push a
		 * switch, because we don't want to create a new in-memory
		 * chunk if the tree is being used read-only now.
		 */
		WT_ERR(__wt_lsm_manager_push_entry(session,
		    WT_LSM_WORK_FLUSH, WT_LSM_WORK_FORCE, lsm_tree));
	} else {
		/*
		 * If there is no chunk to flush, go straight to the
		 * compacting state.
		 */
		compacting = true;
		progress = lsm_tree->merge_progressing;
		F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
		WT_ERR(__wt_verbose(session, WT_VERB_LSM,
		    "COMPACT: Start compacting %s", lsm_tree->name));
	}

	/* Wait for the work unit queues to drain. */
	while (lsm_tree->active) {
		/*
		 * The flush flag is cleared when the chunk has been flushed.
		 * Continue to push forced flushes until the chunk is on disk.
		 * Once it is on disk move to the compacting phase.
		 */
		if (flushing) {
			WT_ASSERT(session, chunk != NULL);
			if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
				WT_ERR(__wt_verbose(session,
				    WT_VERB_LSM,
				    "Compact flush done %s chunk %" PRIu32 ". "
				    "Start compacting progress %" PRIu64,
				    name, chunk->id,
				    lsm_tree->merge_progressing));
				(void)__wt_atomic_sub32(&chunk->refcnt, 1);
				flushing = ref = false;
				compacting = true;
				F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
				progress = lsm_tree->merge_progressing;
			} else {
				WT_ERR(__wt_verbose(session, WT_VERB_LSM,
				    "Compact flush retry %s chunk %" PRIu32,
				    name, chunk->id));
				WT_ERR(__wt_lsm_manager_push_entry(session,
				    WT_LSM_WORK_FLUSH, WT_LSM_WORK_FORCE,
				    lsm_tree));
			}
		}

		/*
		 * The compacting flag is cleared when no merges can be done.
		 * Ensure that we push through some aggressive merges before
		 * stopping otherwise we might not do merges that would
		 * span chunks with different generations.
		 */
		if (compacting && !F_ISSET(lsm_tree, WT_LSM_TREE_COMPACTING)) {
			if (lsm_tree->merge_aggressiveness < 10 ||
			    (progress < lsm_tree->merge_progressing) ||
			    lsm_tree->merge_syncing) {
				progress = lsm_tree->merge_progressing;
				F_SET(lsm_tree, WT_LSM_TREE_COMPACTING);
				lsm_tree->merge_aggressiveness = 10;
			} else
				break;
		}
		__wt_sleep(1, 0);
		WT_ERR(__wt_seconds(session, &end));
		if (session->compact->max_time > 0 &&
		    session->compact->max_time < (uint64_t)(end - begin)) {
			WT_ERR(ETIMEDOUT);
		}
		/*
		 * Push merge operations while they are still getting work
		 * done. If we are pushing merges, make sure they are
		 * aggressive, to avoid duplicating effort.
		 */
		if (compacting)
#define	COMPACT_PARALLEL_MERGES	5
			for (i = lsm_tree->queue_ref;
			    i < COMPACT_PARALLEL_MERGES; i++) {
				lsm_tree->merge_aggressiveness = 10;
				WT_ERR(__wt_lsm_manager_push_entry(
				    session, WT_LSM_WORK_MERGE, 0, lsm_tree));
			}
	}
err:
	/* Ensure anything we set is cleared. */
	if (ref)
		(void)__wt_atomic_sub32(&chunk->refcnt, 1);
	if (compacting) {
		F_CLR(lsm_tree, WT_LSM_TREE_COMPACTING);
		lsm_tree->merge_aggressiveness = 0;
	}
	if (locked)
		WT_TRET(__wt_lsm_tree_writeunlock(session, lsm_tree));

	WT_TRET(__wt_verbose(session, WT_VERB_LSM,
	    "Compact %s complete, return %d", name, ret));

	__wt_lsm_tree_release(session, lsm_tree);
	return (ret);
}

/*
 * __wt_lsm_tree_worker --
 *	Run a schema worker operation on each level of a LSM tree.
 */
int
__wt_lsm_tree_worker(WT_SESSION_IMPL *session,
   const char *uri,
   int (*file_func)(WT_SESSION_IMPL *, const char *[]),
   int (*name_func)(WT_SESSION_IMPL *, const char *, bool *),
   const char *cfg[], uint32_t open_flags)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	u_int i;
	bool exclusive, locked;

	locked = false;
	exclusive = FLD_ISSET(open_flags, WT_DHANDLE_EXCLUSIVE);
	WT_WITH_HANDLE_LIST_LOCK(session,
	    ret = __wt_lsm_tree_get(session, uri, exclusive, &lsm_tree));
	WT_RET(ret);

	/*
	 * We mark that we're busy using the tree to coordinate
	 * with merges so that merging doesn't change the chunk
	 * array out from underneath us.
	 */
	WT_ERR(exclusive ?
	    __wt_lsm_tree_writelock(session, lsm_tree) :
	    __wt_lsm_tree_readlock(session, lsm_tree));
	locked = true;
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		/*
		 * If the chunk is on disk, don't include underlying handles in
		 * the checkpoint. Checking the "get handles" function is all
		 * we need to do, no further checkpoint calls are done if the
		 * handle is not gathered.
		 */
		if (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) &&
		    file_func == __wt_checkpoint_get_handles)
			continue;
		WT_ERR(__wt_schema_worker(session, chunk->uri,
		    file_func, name_func, cfg, open_flags));
		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(__wt_schema_worker(session, chunk->bloom_uri,
			    file_func, name_func, cfg, open_flags));
	}
err:	if (locked)
		WT_TRET(exclusive ?
		    __wt_lsm_tree_writeunlock(session, lsm_tree) :
		    __wt_lsm_tree_readunlock(session, lsm_tree));
	__wt_lsm_tree_release(session, lsm_tree);
	return (ret);
}
