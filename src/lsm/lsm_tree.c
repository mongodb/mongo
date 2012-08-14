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
		__wt_free(session, lsm_tree->chunk[i]);
	__wt_free(session, lsm_tree->chunk);

	__wt_free(session, lsm_tree);
}

/*
 * __wt_lsm_tree_close --
 *	Close an LSM tree structure.
 */
int
__wt_lsm_tree_close(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	WT_DECL_RET;

	if (F_ISSET(lsm_tree, WT_LSM_TREE_OPEN)) {
		F_CLR(lsm_tree, WT_LSM_TREE_OPEN);
		WT_TRET(__wt_thread_join(lsm_tree->worker_tid));
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
 * __wt_lsm_tree_get --
 *	Get an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_create(
    WT_SESSION_IMPL *session, const char *uri, const char *config)
{
	WT_CONFIG_ITEM cval;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	const char *cfg[] = API_CONF_DEFAULTS(session, create, config);

	/*
	 * XXX this call should just insert the metadata: most of this should
	 * move to __wt_lsm_tree_open.
	 */
	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));
	__wt_spin_init(session, &lsm_tree->lock);
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);

	WT_RET(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = uri + strlen("lsm:");

	WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->key_format));
	WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->value_format));

	/* TODO: make this configurable. */
	lsm_tree->threshhold = 2 * WT_MEGABYTE;

	WT_ERR(__wt_scr_alloc(session, 0, &buf));
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s,key_format=u,value_format=u", config));
	lsm_tree->file_config = __wt_buf_steal(session, buf, NULL);

	/* Create the initial chunk. */
	WT_ERR(__wt_lsm_tree_switch(session, lsm_tree));

	/* XXX This should definitely only happen when opening the tree. */
	lsm_tree->conn = S2C(session);
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
 * __wt_lsm_tree_get --
 *	get an LSM tree structure for the given name.
 */
int
__wt_lsm_tree_get(
    WT_SESSION_IMPL *session, const char *uri, WT_LSM_TREE **treep)
{
	WT_LSM_TREE *lsm_tree;

	TAILQ_FOREACH(lsm_tree, &S2C(session)->lsmqh, q)
		if (strcmp(uri, lsm_tree->name) == 0) {
			*treep = lsm_tree;
			return (0);
		}

	return (ENOENT);
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

	__wt_spin_lock(session, &lsm_tree->lock);

	lsm_tree->old_cursors += lsm_tree->ncursor;
	++lsm_tree->dsk_gen;

	/* TODO more sensible realloc */
	if ((lsm_tree->nchunks + 1) * sizeof(*lsm_tree->chunk) >
	    lsm_tree->chunk_allocated)
		WT_ERR(__wt_realloc(session,
		    &lsm_tree->chunk_allocated,
		    (lsm_tree->nchunks + 1) * sizeof(*lsm_tree->chunk),
		    &lsm_tree->chunk));

	WT_ERR(__wt_lsm_tree_create_chunk(session,
	    lsm_tree, lsm_tree->last++,
	    &lsm_tree->chunk[lsm_tree->nchunks].uri));
	++lsm_tree->nchunks;

	if (lsm_tree->memsizep != NULL)
		printf("Switched to %d because %d > %d\n", lsm_tree->last,
		    (int)*lsm_tree->memsizep, (int)lsm_tree->threshhold);
	lsm_tree->memsizep = NULL;

	/* TODO: update metadata. */

err:	__wt_spin_unlock(session, &lsm_tree->lock);
	/* TODO: mark lsm_tree bad on error(?) */
	return (ret);
}
