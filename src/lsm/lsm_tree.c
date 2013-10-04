/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __lsm_tree_open_check(WT_SESSION_IMPL *, WT_LSM_TREE *);
static int __lsm_tree_open(WT_SESSION_IMPL *, const char *, WT_LSM_TREE **);
static int __lsm_tree_set_name(
	WT_SESSION_IMPL *, WT_LSM_TREE *, const char *);

/*
 * __lsm_tree_discard --
 *	Free an LSM tree structure.
 */
static int
__lsm_tree_discard(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	u_int i;

	/* We may be destroying an lsm_tree before it was added. */
	if (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN))
		TAILQ_REMOVE(&S2C(session)->lsmqh, lsm_tree, q);

	__wt_free(session, lsm_tree->name);
	__wt_free(session, lsm_tree->config);
	__wt_free(session, lsm_tree->key_format);
	__wt_free(session, lsm_tree->value_format);
	__wt_free(session, lsm_tree->collator_name);
	__wt_free(session, lsm_tree->bloom_config);
	__wt_free(session, lsm_tree->file_config);

	WT_TRET(__wt_rwlock_destroy(session, &lsm_tree->rwlock));
	WT_TRET(__wt_cond_destroy(session, &lsm_tree->work_cond));

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
__lsm_tree_close(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *s;
	uint32_t i;

	if (F_ISSET(lsm_tree, WT_LSM_TREE_WORKING)) {
		F_CLR(lsm_tree, WT_LSM_TREE_WORKING);
		/*
		 * Signal twice to wake up all threads, even if they are racing
		 * to check the "working" flag.
		 */
		if (F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
			for (i = 0; i < lsm_tree->merge_threads; i++) {
				WT_TRET(__wt_cond_signal(
				    session, lsm_tree->work_cond));
				WT_TRET(__wt_thread_join(
				    session, lsm_tree->worker_tids[i]));
			}
		WT_TRET(__wt_cond_signal(session, lsm_tree->work_cond));
		WT_TRET(__wt_thread_join(session, lsm_tree->ckpt_tid));
	}

	/*
	 * Close the worker thread sessions and free their hazard arrays
	 * (necessary because we set WT_SESSION_INTERNAL to simplify shutdown
	 * ordering).
	 *
	 * Do this in the main thread to avoid deadlocks.
	 */
	for (i = 0; i < lsm_tree->merge_threads; i++) {
		if ((s = lsm_tree->worker_sessions[i]) == NULL)
			continue;
		lsm_tree->worker_sessions[i] = NULL;
		F_SET(s, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));
		wt_session = &s->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		/*
		 * This is safe after the close because session handles are
		 * not freed, but are managed by the connection.
		 */
		__wt_free(NULL, s->hazard);
	}

	if (lsm_tree->ckpt_session != NULL) {
		F_SET(lsm_tree->ckpt_session,
		    F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

		wt_session = &lsm_tree->ckpt_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		/*
		 * This is safe after the close because session handles are
		 * not freed, but are managed by the connection.
		 */
		__wt_free(NULL, lsm_tree->ckpt_session->hazard);
	}
	if (ret != 0) {
		__wt_err(session, ret, "shutdown error while cleaning up LSM");
		(void)__wt_panic(session);
	}

	return (ret);
}

/*
 * __wt_lsm_tree_close_all --
 *	Close an LSM tree structure.
 */
int
__wt_lsm_tree_close_all(WT_SESSION_IMPL *session)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	while ((lsm_tree = TAILQ_FIRST(&S2C(session)->lsmqh)) != NULL) {
		WT_TRET(__lsm_tree_close(session, lsm_tree));
		WT_TRET(__lsm_tree_discard(session, lsm_tree));
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
	if (lsm_tree->name != NULL)
		__wt_free(session, lsm_tree->name);
	WT_RET(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = lsm_tree->name + strlen("lsm:");
	return (0);
}

/*
 * __wt_lsm_tree_bloom_name --
 *	Get the URI of the Bloom filter for a given chunk.
 */
int
__wt_lsm_tree_bloom_name(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id, WT_ITEM *buf)
{
	WT_RET(__wt_buf_fmt(session, buf, "file:%s-%06" PRIu32 ".bf",
	    lsm_tree->filename, id));
	return (0);
}

/*
 * __wt_lsm_tree_chunk_name --
 *	Get the URI of the file for a given chunk.
 */
int
__wt_lsm_tree_chunk_name(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, uint32_t id, WT_ITEM *buf)
{
	WT_RET(__wt_buf_fmt(session, buf, "file:%s-%06" PRIu32 ".lsm",
	    lsm_tree->filename, id));
	return (0);
}

/*
 * __wt_lsm_tree_setup_chunk --
 *	Initialize a chunk of an LSM tree.
 */
int
__wt_lsm_tree_setup_chunk(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, WT_LSM_CHUNK *chunk)
{
	WT_ITEM buf;
	const char *cfg[] =
	    { WT_CONFIG_BASE(session, session_drop), "force", NULL };
	int exists;

	WT_CLEAR(buf);

	WT_RET(__wt_epoch(session, &chunk->create_ts));

	WT_RET(__wt_lsm_tree_chunk_name(session, lsm_tree, chunk->id, &buf));
	chunk->uri = __wt_buf_steal(session, &buf, NULL);

	/*
	 * If the underlying file exists, drop the chunk first - there may be
	 * some content hanging over from an aborted merge or checkpoint.
	 *
	 * Don't do this for the very first chunk: we are called during
	 * WT_SESSION::create, and doing a drop inside there does interesting
	 * things with handle locks and metadata tracking.  It can never have
	 * been the result of an interrupted merge, anyway.
	 */
	if (chunk->id > 1) {
		WT_RET(__wt_exist(
		    session, chunk->uri + strlen("file:"), &exists));
		if (exists)
			WT_RET(__wt_schema_drop(session, chunk->uri, cfg));
	}
	return (__wt_schema_create(session, chunk->uri, lsm_tree->file_config));
}

/*
 * __wt_lsm_start_worker --
 *	Start the worker thread for an LSM tree.
 */
static int
__lsm_tree_start_worker(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_CONNECTION *wt_conn;
	WT_LSM_WORKER_ARGS *wargs;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *s;
	uint32_t i;

	wt_conn = &S2C(session)->iface;

	/*
	 * All the LSM worker threads do their operations on read-only files.
	 * Use read-uncommitted isolation to avoid keeping updates in cache
	 * unnecessarily.
	 */
	WT_RET(wt_conn->open_session(
	    wt_conn, NULL, "isolation=read-uncommitted", &wt_session));
	lsm_tree->ckpt_session = (WT_SESSION_IMPL *)wt_session;
	F_SET(lsm_tree->ckpt_session, WT_SESSION_INTERNAL);

	F_SET(lsm_tree, WT_LSM_TREE_WORKING);
	/* The new thread will rely on the WORKING value being visible. */
	WT_FULL_BARRIER();
	if (F_ISSET(S2C(session), WT_CONN_LSM_MERGE))
		for (i = 0; i < lsm_tree->merge_threads; i++) {
			WT_RET(wt_conn->open_session(
			    wt_conn, NULL, "isolation=read-uncommitted",
			    &wt_session));
			s = (WT_SESSION_IMPL *)wt_session;
			F_SET(s, WT_SESSION_INTERNAL);
			lsm_tree->worker_sessions[i] = s;

			WT_RET(__wt_calloc_def(session, 1, &wargs));
			wargs->lsm_tree = lsm_tree;
			wargs->id = i;
			WT_RET(__wt_thread_create(session,
			    &lsm_tree->worker_tids[i],
			    __wt_lsm_merge_worker, wargs));
		}
	WT_RET(__wt_thread_create(session,
	    &lsm_tree->ckpt_tid, __wt_lsm_checkpoint_worker, lsm_tree));

	return (0);
}

/*
 * __wt_lsm_tree_create --
 *	Create an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_create(WT_SESSION_IMPL *session,
    const char *uri, int exclusive, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	const char *cfg[] =
	    { WT_CONFIG_BASE(session, session_create), config, NULL };
	const char *tmpconfig;

	/* If the tree is open, it already exists. */
	if ((ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree)) == 0) {
		__wt_lsm_tree_release(session, lsm_tree);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	/*
	 * If the tree has metadata, it already exists.
	 *
	 * !!!
	 * Use a local variable: we don't care what the existing configuration
	 * is, but we don't want to overwrite the real config.
	 */
	if (__wt_metadata_search(session, uri, &tmpconfig) == 0) {
		__wt_free(session, tmpconfig);
		return (exclusive ? EEXIST : 0);
	}
	WT_RET_NOTFOUND_OK(ret);

	WT_RET(__wt_config_gets(session, cfg, "key_format", &cval));
	if (WT_STRING_MATCH("r", cval.str, cval.len))
		WT_RET_MSG(session, EINVAL,
		    "LSM trees cannot be configured as column stores");

	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));

	WT_RET(__lsm_tree_set_name(session, lsm_tree, uri));

	WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->key_format));
	WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->value_format));

	WT_ERR(__wt_config_gets(session, cfg, "collator", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->collator_name));

	WT_ERR(__wt_config_gets(session, cfg, "lsm_auto_throttle", &cval));
	if (cval.val)
		F_SET(lsm_tree, WT_LSM_TREE_THROTTLE);
	else
		F_CLR(lsm_tree, WT_LSM_TREE_THROTTLE);
	WT_ERR(__wt_config_gets(session, cfg, "lsm_bloom", &cval));
	FLD_SET(lsm_tree->bloom,
	    (cval.val == 0 ? WT_LSM_BLOOM_OFF : WT_LSM_BLOOM_MERGED));
	WT_ERR(__wt_config_gets(session, cfg, "lsm_bloom_oldest", &cval));
	if (cval.val != 0)
		FLD_SET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST);

	if (FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OFF) &&
	    FLD_ISSET(lsm_tree->bloom, WT_LSM_BLOOM_OLDEST))
		WT_ERR_MSG(session, EINVAL,
		    "Bloom filters can only be created on newest and oldest "
		    "chunks if bloom filters are enabled");

	WT_ERR(__wt_config_gets(session, cfg, "lsm_bloom_config", &cval));
	if (cval.type == WT_CONFIG_ITEM_STRUCT) {
		cval.str++;
		cval.len -= 2;
	}
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->bloom_config));

	WT_ERR(__wt_config_gets(session, cfg, "lsm_bloom_bit_count", &cval));
	lsm_tree->bloom_bit_count = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm_bloom_hash_count", &cval));
	lsm_tree->bloom_hash_count = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm_chunk_size", &cval));
	lsm_tree->chunk_size = (uint32_t)cval.val;
	WT_ERR(__wt_config_gets(session, cfg, "lsm_merge_max", &cval));
	lsm_tree->merge_max = (uint32_t)cval.val;
	lsm_tree->merge_min = lsm_tree->merge_max / 2;
	WT_ERR(__wt_config_gets(session, cfg, "lsm_merge_threads", &cval));
	lsm_tree->merge_threads = (uint32_t)cval.val;
	/* Sanity check that api_data.py is in sync with lsm.h */
	WT_ASSERT(session, lsm_tree->merge_threads <= WT_LSM_MAX_WORKERS);

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s,key_format=u,value_format=u", config));
	lsm_tree->file_config = __wt_buf_steal(session, buf, NULL);

	/* Create the first chunk and flush the metadata. */
	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));

	/* Discard our partially populated handle. */
	ret = __lsm_tree_discard(session, lsm_tree);
	lsm_tree = NULL;

	/*
	 * Open our new tree and add it to the handle cache. Don't discard on
	 * error: the returned handle is NULL on error, and the metadata
	 * tracking macros handle cleaning up on failure.
	 */
	if (ret == 0)
		ret = __lsm_tree_open(session, uri, &lsm_tree);
	if (ret == 0)
		__wt_lsm_tree_release(session, lsm_tree);

	if (0) {
err:		WT_TRET(__lsm_tree_discard(session, lsm_tree));
	}
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __lsm_tree_open_check --
 *	Validate the configuration of an LSM tree.
 */
static int
__lsm_tree_open_check(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_CONFIG_ITEM cval;
	uint64_t required;
	uint32_t maxleafpage;
	const char *cfg[] = { WT_CONFIG_BASE(
	    session, session_create), lsm_tree->file_config, NULL };

	WT_RET(__wt_config_gets(session, cfg, "leaf_page_max", &cval));
	maxleafpage = (uint32_t)cval.val;

	/* Three chunks, plus one page for each participant in a merge. */
	required = 3 * lsm_tree->chunk_size +
	    lsm_tree->merge_threads * (lsm_tree->merge_max *  maxleafpage);
	if (S2C(session)->cache_size < required)
		WT_RET_MSG(session, EINVAL,
		    "The LSM configuration requires a cache size of at least %"
		    PRIu64 ". Configured size is %" PRIu64,
		    required, S2C(session)->cache_size);
	return (0);
}

/*
 * __lsm_tree_open --
 *	Open an LSM tree structure.
 */
static int
__lsm_tree_open(
    WT_SESSION_IMPL *session, const char *uri, WT_LSM_TREE **treep)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	WT_ASSERT(session, F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

	/* Make sure no one beat us to it. */
	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if (strcmp(uri, lsm_tree->name) == 0) {
			*treep = lsm_tree;
			return (0);
		}

	/* Try to open the tree. */
	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));
	WT_ERR(__wt_rwlock_alloc(session, "lsm tree", &lsm_tree->rwlock));
	WT_ERR(__wt_cond_alloc(session, "lsm ckpt", 0, &lsm_tree->work_cond));
	WT_ERR(__lsm_tree_set_name(session, lsm_tree, uri));

	WT_ERR(__wt_lsm_meta_read(session, lsm_tree));

	/*
	 * Sanity check the configuration. Do it now since this is the first
	 * time we have the LSM tree configuration.
	 */
	WT_ERR(__lsm_tree_open_check(session, lsm_tree));

	if (lsm_tree->nchunks == 0)
		WT_ERR(__wt_lsm_tree_switch(session, lsm_tree));

	/* Set the generation number so cursors are opened on first usage. */
	lsm_tree->dsk_gen = 1;

	/* Now the tree is setup, make it visible to others. */
	lsm_tree->refcnt = 1;
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);
	F_SET(lsm_tree, WT_LSM_TREE_OPEN);

	WT_ERR(__lsm_tree_start_worker(session, lsm_tree));
	*treep = lsm_tree;

	if (0) {
err:		WT_TRET(__lsm_tree_discard(session, lsm_tree));
	}
	return (ret);
}

/*
 * __wt_lsm_tree_get --
 *	get an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_get(WT_SESSION_IMPL *session,
    const char *uri, int exclusive, WT_LSM_TREE **treep)
{
	WT_LSM_TREE *lsm_tree;

	/* See if the tree is already open. */
	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if (strcmp(uri, lsm_tree->name) == 0) {
			if (exclusive && lsm_tree->refcnt)
				return (EBUSY);

			(void)WT_ATOMIC_ADD(lsm_tree->refcnt, 1);
			*treep = lsm_tree;
			return (0);
		}

	/* Open a new tree. */
	return (__lsm_tree_open(session, uri, treep));
}

/*
 * __wt_lsm_tree_get --
 *	get an LSM tree structure for the given name.
 */
void
__wt_lsm_tree_release(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_ASSERT(session, lsm_tree->refcnt > 0);
	(void)WT_ATOMIC_SUB(lsm_tree->refcnt, 1);
}

/*
 * __lsm_tree_throttle --
 *	Calculate whether LSM updates need to be throttled.
 */
static void
__lsm_tree_throttle(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_LSM_CHUNK *chunk, **cp;
	uint64_t cache_sz, cache_used, record_count;
	uint32_t in_memory;

	cache_sz = S2C(session)->cache_size;

	/*
	 * In the steady state, we expect that the checkpoint worker thread
	 * will keep up with inserts.  If not, we throttle the insert rate to
	 * avoid filling the cache with in-memory chunks.  Threads sleep every
	 * 100 operations, so take that into account in the calculation.
	 */
	record_count = 1;
	for (in_memory = 1, cp = lsm_tree->chunk + lsm_tree->nchunks - 1;
	    in_memory < lsm_tree->nchunks && !F_ISSET(*cp, WT_LSM_CHUNK_ONDISK);
	    ++in_memory, --cp)
		record_count += (*cp)->count;

	chunk = lsm_tree->chunk[lsm_tree->nchunks - 1];

	if (!F_ISSET(lsm_tree, WT_LSM_TREE_THROTTLE) || in_memory <= 3)
		lsm_tree->throttle_sleep = 0;
	else if (in_memory == lsm_tree->nchunks ||
	    F_ISSET(*cp, WT_LSM_CHUNK_STABLE)) {
		/*
		 * No checkpoint has completed this run.  Keep slowing down
		 * inserts until one does.
		 */
		lsm_tree->throttle_sleep =
		    WT_MAX(20, 2 * lsm_tree->throttle_sleep);
	} else {
		lsm_tree->throttle_sleep = (long)((in_memory - 2) *
		    WT_TIMEDIFF(chunk->create_ts, (*cp)->create_ts) /
		    (20 * record_count));

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
			lsm_tree->throttle_sleep *= 5;
	}

	/*
	 * Update our estimate of how long each in-memory chunk stays active.
	 * Filter out some noise by keeping a weighted history of the
	 * calculated value.
	 */
	if (in_memory > 1)
		lsm_tree->chunk_fill_ms = (3 * lsm_tree->chunk_fill_ms +
		    WT_TIMEDIFF(chunk->create_ts, (*cp)->create_ts) / 1000000) /
		    (3 + in_memory - 1);
}

/*
 * __wt_lsm_tree_switch --
 *	Switch to a new in-memory tree.
 */
int
__wt_lsm_tree_switch(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	uint32_t new_id;

	new_id = WT_ATOMIC_ADD(lsm_tree->last, 1);

	if (lsm_tree->nchunks > 0)
		__lsm_tree_throttle(session, lsm_tree);

	WT_ERR(__wt_realloc_def(session, &lsm_tree->chunk_alloc,
	    lsm_tree->nchunks + 1, &lsm_tree->chunk));

	WT_VERBOSE_ERR(session, lsm, "Tree switch to: %d, throttle %d",
	    new_id, (int)lsm_tree->throttle_sleep);

	WT_ERR(__wt_calloc_def(session, 1, &chunk));
	chunk->id = new_id;
	chunk->txnid_max = WT_TXN_NONE;
	lsm_tree->chunk[lsm_tree->nchunks++] = chunk;
	WT_ERR(__wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));
	F_CLR(lsm_tree, WT_LSM_TREE_NEED_SWITCH);
	++lsm_tree->dsk_gen;

err:	/* TODO: mark lsm_tree bad on error(?) */
	return (ret);
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
	u_int i;
	int locked;

	locked = 0;

	/* Get the LSM tree. */
	WT_RET(__wt_lsm_tree_get(session, name, 1, &lsm_tree));

	/* Shut down the LSM worker. */
	WT_ERR(__lsm_tree_close(session, lsm_tree));

	/* Prevent any new opens. */
	WT_ERR(__wt_try_writelock(session, lsm_tree->rwlock));
	locked = 1;

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

	ret = __wt_rwunlock(session, lsm_tree->rwlock);
	locked = 0;
	if (ret == 0)
		ret = __wt_metadata_remove(session, name);

err:	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	WT_TRET(__lsm_tree_discard(session, lsm_tree));
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
	WT_ITEM buf;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	const char *old;
	u_int i;
	int locked;

	old = NULL;
	WT_CLEAR(buf);
	locked = 0;

	/* Get the LSM tree. */
	WT_RET(__wt_lsm_tree_get(session, olduri, 1, &lsm_tree));

	/* Shut down the LSM worker. */
	WT_ERR(__lsm_tree_close(session, lsm_tree));

	/* Prevent any new opens. */
	WT_ERR(__wt_try_writelock(session, lsm_tree->rwlock));
	locked = 1;

	/* Set the new name. */
	WT_ERR(__lsm_tree_set_name(session, lsm_tree, newuri));

	/* Rename the chunks. */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		old = chunk->uri;
		chunk->uri = NULL;

		WT_ERR(__wt_lsm_tree_chunk_name(
		    session, lsm_tree, chunk->id, &buf));
		chunk->uri = __wt_buf_steal(session, &buf, NULL);
		WT_ERR(__wt_schema_rename(session, old, chunk->uri, cfg));
		__wt_free(session, old);

		if (F_ISSET(chunk, WT_LSM_CHUNK_BLOOM)) {
			old = chunk->bloom_uri;
			chunk->bloom_uri = NULL;
			WT_ERR(__wt_lsm_tree_bloom_name(
			    session, lsm_tree, chunk->id, &buf));
			chunk->bloom_uri = __wt_buf_steal(session, &buf, NULL);
			F_SET(chunk, WT_LSM_CHUNK_BLOOM);
			WT_ERR(__wt_schema_rename(
			    session, old, chunk->uri, cfg));
			__wt_free(session, old);
		}
	}

	ret = __wt_rwunlock(session, lsm_tree->rwlock);
	locked = 0;
	if (ret == 0)
		ret = __wt_lsm_meta_write(session, lsm_tree);
	if (ret == 0)
		ret = __wt_metadata_remove(session, olduri);

err:	if (locked)
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	if (old != NULL)
		__wt_free(session, old);
	/*
	 * Discard this LSM tree structure. The first operation on the renamed
	 * tree will create a new one.
	 */
	WT_TRET(__lsm_tree_discard(session, lsm_tree));
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
	int locked;

	WT_UNUSED(cfg);
	locked = 0;

	/* Get the LSM tree. */
	WT_RET(__wt_lsm_tree_get(session, name, 1, &lsm_tree));

	/* Shut down the LSM worker. */
	WT_RET(__lsm_tree_close(session, lsm_tree));

	/* Prevent any new opens. */
	WT_RET(__wt_try_writelock(session, lsm_tree->rwlock));
	locked = 1;

	/* Create the new chunk. */
	WT_ERR(__wt_calloc_def(session, 1, &chunk));
	chunk->id = WT_ATOMIC_ADD(lsm_tree->last, 1);
	WT_ERR(__wt_lsm_tree_setup_chunk(session, lsm_tree, chunk));

	/* Mark all chunks old. */
	WT_ERR(__wt_lsm_merge_update_tree(
	    session, lsm_tree, 0, lsm_tree->nchunks, chunk));

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));

	WT_ERR(__lsm_tree_start_worker(session, lsm_tree));
	ret = __wt_rwunlock(session, lsm_tree->rwlock);
	locked = 0;
	if (ret == 0)
		__wt_lsm_tree_release(session, lsm_tree);

err:	if (locked) 
		WT_TRET(__wt_rwunlock(session, lsm_tree->rwlock));
	/*
	 * Don't discard the LSM tree structure unless there has been an
	 * error. The handle remains valid for future operations.
	 */
	if (ret != 0)
		WT_TRET(__lsm_tree_discard(session, lsm_tree));
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
   int (*name_func)(WT_SESSION_IMPL *, const char *),
   const char *cfg[], uint32_t open_flags)
{
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	u_int i;

	WT_RET(__wt_lsm_tree_get(session, uri,
	    FLD_ISSET(open_flags, WT_DHANDLE_EXCLUSIVE) ? 1 : 0, &lsm_tree));
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = lsm_tree->chunk[i];
		if (file_func == __wt_checkpoint &&
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			continue;
		WT_ERR(__wt_schema_worker(session, chunk->uri,
		    file_func, name_func, cfg, open_flags));
		if (name_func == __wt_backup_list_uri_append &&
		    chunk->bloom_uri != NULL)
			WT_ERR(__wt_schema_worker(session, chunk->bloom_uri,
			    file_func, name_func, cfg, open_flags));
	}
err:	__wt_lsm_tree_release(session, lsm_tree);
	return (ret);
}
