/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void
__lsm_tree_discard(WT_SESSION_IMPL *session, WT_LSM_TREE *lsm_tree)
{
	int i;

	TAILQ_REMOVE(&S2C(session)->lsmqh, lsm_tree, q);

	__wt_free(session, lsm_tree->name);
	for (i = 0; i < lsm_tree->nchunks; i++)
		__wt_free(session, lsm_tree->chunk[i]);
	__wt_free(session, lsm_tree->chunk);
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

	WT_RET(__wt_calloc_def(session, 1, &lsm_tree));
	TAILQ_INSERT_HEAD(&S2C(session)->lsmqh, lsm_tree, q);

	WT_RET(__wt_strdup(session, uri, &lsm_tree->name));
	lsm_tree->filename = uri + strlen("lsm:");

	WT_ERR(__wt_scr_alloc(session, 0, &buf));

	WT_ERR(__wt_config_gets(session, cfg, "key_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->key_format));
	WT_ERR(__wt_config_gets(session, cfg, "value_format", &cval));
	WT_ERR(__wt_strndup(session, cval.str, cval.len,
	    &lsm_tree->value_format));

	lsm_tree->nchunks = 1;
	WT_ERR(__wt_calloc_def(session, lsm_tree->nchunks, &lsm_tree->chunk));
	WT_ERR(__wt_buf_fmt(session, buf, "file:%s-%06d.lsm",
	    lsm_tree->filename, 1));
	lsm_tree->chunk[0] = __wt_buf_steal(session, buf, NULL);

	/* value_format=u, including a byte of status */
	WT_ERR(__wt_buf_fmt(session, buf,
	    "%s,key_format=u,value_format=u", config));

	WT_ERR(__wt_schema_create(session, lsm_tree->chunk[0], buf->data));

	if (0) {
err:		__lsm_tree_discard(session, lsm_tree);
	}
	__wt_scr_free(&buf);
	return (ret);
}
/*
 * __wt_lsm_tree_get --
 *	Get an LSM tree structure for the given name.
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
