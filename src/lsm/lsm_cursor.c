/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define	FORALL_CURSORS(clsm, c, i)					\
	for (i = 0; i < clsm->nchunks && (c = clsm->cursors[i]) != NULL; i++)

/*
 * __clsm_next --
 *	WT_CURSOR->next method for the LSM cursor type.
 */
static int
__clsm_next(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, next, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_prev --
 *	WT_CURSOR->prev method for the LSM cursor type.
 */
static int
__clsm_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, prev, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_reset --
 *	WT_CURSOR->reset method for the LSM cursor type.
 */
static int
__clsm_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, reset, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_search --
 *	WT_CURSOR->search method for the LSM cursor type.
 */
static int
__clsm_search(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, search, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_search_near --
 *	WT_CURSOR->search_near method for the LSM cursor type.
 */
static int
__clsm_search_near(WT_CURSOR *cursor, int *exact)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, search_near, NULL);
	WT_UNUSED(exact);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_insert --
 *	WT_CURSOR->insert method for the LSM cursor type.
 */
static int
__clsm_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *primary;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, insert, NULL);
	primary = clsm->cursors[0];
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_update --
 *	WT_CURSOR->update method for the LSM cursor type.
 */
static int
__clsm_update(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, update, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_remove --
 *	WT_CURSOR->remove method for the LSM cursor type.
 */
static int
__clsm_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, remove, NULL);
	ret = ENOTSUP;
	API_END(session);

	return (ret);
}

/*
 * __clsm_close --
 *	WT_CURSOR->close method for the LSM cursor type.
 */
static int
__clsm_close(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL_NOCONF(cursor, session, close, NULL);
	__wt_free(session, clsm->cursors);
	/* The WT_LSM_TREE owns the URI. */
	cursor->uri = NULL;
	WT_TRET(__wt_cursor_close(cursor));
	API_END(session);

	return (ret);
}

static int
__clsm_open_cursors(WT_CURSOR_LSM *clsm, const char *cfg[])
{
	WT_CURSOR **cp;
	WT_DECL_RET;
	WT_LSM_TREE *lsmtree;
	WT_SESSION_IMPL *session;
	WT_SESSION *wt_session;
	const char *cfg_no_overwrite[4], *config;
	int i;

	session = (WT_SESSION_IMPL *)clsm->iface.session;
	wt_session = &session->iface;
	lsmtree = clsm->lsmtree;

	/* Underlying column groups are always opened without overwrite */
	cfg_no_overwrite[0] = cfg[0];
	cfg_no_overwrite[1] = cfg[1];
	cfg_no_overwrite[2] = "overwrite=false";
	cfg_no_overwrite[3] = NULL;

	WT_RET(__wt_config_collapse(session, cfg, &config));
	WT_ERR(__wt_calloc_def(session, lsmtree->chunks, &clsm->cursors));

	for (i = 0, cp = clsm->cursors;
	    i < lsmtree->chunks;
	    i++, cp++)
		WT_ERR(wt_session->open_cursor(
		    wt_session, lsmtree->chunk[i], NULL, config, cp));

err:	__wt_free(session, config);
	return (ret);
}

/*
 * __wt_clsm_open --
 *	WT_SESSION->open_cursor method for LSM cursors.
 */
int
__wt_clsm_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	static WT_CURSOR iface = {
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		NULL,
		__clsm_next,
		__clsm_prev,
		__clsm_reset,
		__clsm_search,
		__clsm_search_near,
		__clsm_insert,
		__clsm_update,
		__clsm_remove,
		__clsm_close,
		NULL,
		{ NULL, NULL },		/* TAILQ_ENTRY q */
		0,			/* recno key */
		{ 0 },                  /* raw recno buffer */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM key */
		{ NULL, 0, 0, NULL, 0 },/* WT_ITEM value */
		0,			/* int saved_err */
		0			/* uint32_t flags */
	};
	WT_CURSOR *cursor;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_LSM_TREE *lsmtree;
	size_t size;
	const char *treename;

	clsm = NULL;

	treename = uri;
	if (!WT_PREFIX_SKIP(treename, "lsm:"))
		return (EINVAL);
	size = strlen(treename);

	WT_RET(__wt_calloc_def(session, 1, &clsm));

	cursor = &clsm->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = lsmtree->name;
	cursor->key_format = lsmtree->key_format;
	cursor->value_format = lsmtree->value_format;

	clsm->lsmtree = lsmtree;

	/*
	 * Open the cursors immediately: we're going to need them for any
	 * operation.
	 */
	WT_ERR(__clsm_open_cursors(clsm, cfg));

	STATIC_ASSERT(offsetof(WT_CURSOR_LSM, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, 0, cfg, cursorp));

	if (0) {
err:		(void)__clsm_close(cursor);
	}

	return (ret);
}
