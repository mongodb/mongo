/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __lsm_tree_discard --
 *	Free an LSM tree structure.
 */
static void
__lsm_tree_discard(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	int i;

	TAILQ_REMOVE(&S2C(session)->lsmqh, lsm_tree, q);
	__wt_spin_destroy(session, &lsm_tree->lock);

	__wt_free(session, lsm_tree->name);
	for (i = 0; i < lsm_tree->nchunks; i++)
		__wt_free(session, lsm_tree->chunk[i].uri);
	__wt_free(session, lsm_tree->chunk);

	__wt_free(session, lsm_tree);
}

/*
 * __wt_lsm_tree_bump_gen --
 *	Update the generation number of a LSM tree.
 */
int
__wt_lsm_tree_bump_gen(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_UNUSED(session);
	++lsm_tree->dsk_gen;
	return (0);
}

/*
 * __wt_lsm_tree_close --
 *	Close an LSM tree structure.
 */
int
__wt_lsm_tree_close(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;
	WT_SESSION *wt_session;

	if (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
		F_CLR(lsm_tree, WT_LSM_TREE_OPEN);
		WT_TRET(__wt_thread_join(lsm_tree->worker_tid));
	}

	/*
	 * Close the session and free its hazard array (necessary because
	 * we set WT_SESSION_INTERNAL to simplify shutdown ordering.
	 *
	 * Do this in the main thread to avoid deadlocks.
	 */
	if (lsm_tree->worker_session != NULL) {
		F_SET(lsm_tree->worker_session,
		    F_ISSET(session, WT_SESSION_SCHEMA_LOCKED));

		wt_session = &lsm_tree->worker_session->iface;
		WT_TRET(wt_session->close(wt_session, NULL));

		/*
		 * This is safe after the close because session handles are
		 * not freed, but are managed by the connection.
		 */
		__wt_free(NULL, lsm_tree->worker_session->hazard);
	}

	__lsm_tree_discard(session, lsm_tree);
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

	while ((lsm_tree = TAILQ_FIRST(&S2C(session)->lsmqh)) != NULL)
		WT_TRET(__wt_lsm_tree_close(session, lsm_tree));

	return (ret);
}

/*
 * __wt_lsm_tree_create_chunk --
 *	Create a chunk of an LSM tree.
 */
int
__wt_lsm_tree_create_chunk(
    WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree, int i, const char **urip)
{
	WT_DECL_ITEM(buf);
	WT_DECL_RET;

	WT_RET(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf, "file:%s-%06d.lsm",
	    lsm_tree->filename, i + 1));
	WT_ERR(__wt_schema_create(session,
	    buf->data, lsm_tree->file_config));
	*urip = __wt_buf_steal(session, buf, NULL);

err:	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_lsm_tree_create --
 *	Create an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_CONNECTION *wt_conn;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION *wt_session;
	const char *cfg[] = API_CONF_DEFAULTS(session, create, config);

	/* If the tree is open, it already exists. */
	if ((ret = __wt_lsm_tree_get(session, uri, &lsm_tree)) == 0)
		return (EEXIST);
	WT_RET_NOTFOUND_OK(ret);

	/* If the tree has metadata, it already exists. */
	if (__wt_metadata_read(session, uri, &config) == 0) {
		__wt_free(session, config);
		return (EEXIST);
	}
	WT_RET_NOTFOUND_OK(ret);

	/*
	 * XXX this call should just insert the metadata: most of this should
	 * move to __wt_lsm_tree_open.
	 */
	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));
	__wt_spin_init(session, &lsm_tree->lock);
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);

	WT_RET(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = lsm_tree->name + strlen("lsm:");

	WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->key_format));
	WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->value_format));

	/* TODO: make this configurable. */
	lsm_tree->threshold = 2 * WT_MEGABYTE;

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s,key_format=u,value_format=u", config));
	lsm_tree->file_config = __wt_buf_steal(session, buf, NULL);

	/* Create the initial chunk. */
	WT_ERR(__wt_lsm_tree_switch(session, lsm_tree));

	/* XXX This should definitely only happen when opening the tree. */
	wt_conn = &S2C(session)->iface;
	WT_ERR(wt_conn->open_session(wt_conn, NULL, NULL, &wt_session));
	lsm_tree->worker_session = (WT_SESSION_IMPL *)wt_session;
	F_SET(lsm_tree->worker_session, WT_SESSION_INTERNAL);

	WT_ERR(__wt_thread_create(
	    &lsm_tree->worker_tid, __wt_lsm_worker, lsm_tree));
	F_SET(lsm_tree, WT_LSM_TREE_OPEN);

	if (0) {
err:		__lsm_tree_discard(session, lsm_tree);
	}
	__wt_scr_free(&buf);
	return (ret);
}

/*
 * __wt_lsm_tree_open --
 *	Open an LSM tree structure.
 */
static int
__lsm_tree_open(
    WT_SESSION_IMPL *session, const char *uri, WT_LSM_TREE **treep)
{
	WT_CONNECTION *wt_conn;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION *wt_session;

	/* Try to open the tree. */
	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));
	__wt_spin_init(session, &lsm_tree->lock);
	/* TODO: acquire the lock before the tree is visible? */
	WT_ERR(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = lsm_tree->name + strlen("lsm:");
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);

	WT_ERR(__wt_lsm_meta_read(session, lsm_tree));

	/* Set the generation number so cursors are opened on first usage. */
	lsm_tree->dsk_gen = 1;

	wt_conn = &S2C(session)->iface;
	WT_ERR(wt_conn->open_session(wt_conn, NULL, NULL, &wt_session));
	lsm_tree->worker_session = (WT_SESSION_IMPL *)wt_session;
	F_SET(lsm_tree->worker_session, WT_SESSION_INTERNAL);

	WT_ERR(__wt_thread_create(
	    &lsm_tree->worker_tid, __wt_lsm_worker, lsm_tree));
	F_SET(lsm_tree, WT_LSM_TREE_OPEN);
	*treep = lsm_tree;

	if (0) {
err:		__lsm_tree_discard(session, lsm_tree);
	}
	return (ret);
}

/*
 * __wt_lsm_tree_get --
 *	get an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_get(
    WT_SESSION_IMPL *session, const char *uri, WT_LSM_TREE **treep)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if (strcmp(uri, lsm_tree->name) == 0) {
			*treep = lsm_tree;
			return (0);
		}

	/*
	 * If we don't already hold the schema lock, get it now so that we
	 * can find and/or open the handle.
	 */
	WT_WITH_SCHEMA_LOCK_OPT(session,
	    ret = __lsm_tree_open(session, uri, treep));
	return (ret);
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

	if (lsm_tree->memsizep != NULL)
		printf("Switched to %d because %d > %d\n", lsm_tree->last,
		    (int)*lsm_tree->memsizep, (int)lsm_tree->threshold);
	lsm_tree->memsizep = NULL;

	WT_ERR(__wt_lsm_tree_bump_gen(session, lsm_tree));

	/* TODO more sensible realloc */
	if ((lsm_tree->nchunks + 1) * sizeof(*lsm_tree->chunk) >
	    lsm_tree->chunk_alloc)
		WT_ERR(__wt_realloc(session,
		    &lsm_tree->chunk_alloc,
		    WT_MAX(10 * sizeof(*lsm_tree->chunk),
		    2 * lsm_tree->chunk_alloc),
		    &lsm_tree->chunk));

	WT_ERR(__wt_lsm_tree_create_chunk(session,
	    lsm_tree, lsm_tree->last++,
	    &lsm_tree->chunk[lsm_tree->nchunks].uri));
	++lsm_tree->nchunks;

	WT_ERR(__wt_lsm_meta_write(session, lsm_tree));

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
	int i;

	/* Get the LSM tree. */
	WT_RET(__wt_lsm_tree_get(session, name, &lsm_tree));
	__wt_spin_lock(session, &lsm_tree->lock);

	/* Drop the chunks. */
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = &lsm_tree->chunk[i];
		WT_ERR(__wt_schema_drop(session, chunk->uri, cfg));
	}

	WT_ERR(__wt_metadata_remove(session, lsm_tree->name));
	__wt_spin_unlock(session, &lsm_tree->lock);
	ret = __wt_lsm_tree_close(session, lsm_tree);

	if (0) {
err:		__wt_spin_unlock(session, &lsm_tree->lock);
	}
	return (ret);
}

/*
 * __wt_lsm_tree_worker --
 *	Run a schema worker operation on each level of a LSM tree.
 */
int
__wt_lsm_tree_worker(WT_SESSION_IMPL *session,
   const char *uri,
   int (*func)(WT_SESSION_IMPL *, const char *[]),
   const char *cfg[], uint32_t open_flags)
{
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	int i;

	WT_RET(__wt_lsm_tree_get(session, uri, &lsm_tree));
	for (i = 0; i < lsm_tree->nchunks; i++) {
		chunk = &lsm_tree->chunk[i];
		if (func == __wt_checkpoint &&
		    F_ISSET(chunk, WT_LSM_CHUNK_ONDISK))
			continue;
		WT_RET(__wt_schema_worker(
		    session, chunk->uri, func, cfg, open_flags));
	}
	return (0);
}
