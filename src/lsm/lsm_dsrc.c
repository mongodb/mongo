/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __lsm_create --
 *	Implementation of the create operation for LSM trees.
 */
static int
__lsm_create(WT_DATA_SOURCE *dsrc, WT_SESSION *wt_session,
    const char *uri, int exclusive, const char *config)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(dsrc);

	session = (WT_SESSION_IMPL *)wt_session;
	return (__wt_lsm_tree_create(session, uri, exclusive, config));
}

/*
 * __lsm_drop --
 *	Implementation of the drop operation for LSM trees.
 */
static int
__lsm_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *wt_session,
    const char *uri, const char *cfg[])
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(dsrc);
	session = (WT_SESSION_IMPL *)wt_session;

	return (__wt_lsm_tree_drop(session, uri, cfg));
}

/*
 * __lsm_open_cursor --
 *	Implementation of the open_cursor operation for LSM trees.
 */
static int
__lsm_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *wt_session,
    const char *obj, WT_CURSOR *owner, const char *cfg[],
    WT_CURSOR **new_cursor)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;
	WT_UNUSED(dsrc);

	return (__wt_clsm_open(session, obj, owner, cfg, new_cursor));
}

/*
 * __lsm_rename --
 *	Implementation of the rename operation for LSM trees.
 */
static int
__lsm_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *wt_session,
    const char *oldname, const char *newname, const char *cfg[])
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(dsrc);
	session = (WT_SESSION_IMPL *)wt_session;

	if (!WT_PREFIX_MATCH(newname, "lsm:"))
		WT_RET_MSG(session, EINVAL,
		    "rename target type must match URI: %s to %s",
		    oldname, newname);

	return (__wt_lsm_tree_rename(session, oldname, newname, cfg));
}

/*
 * __lsm_truncate --
 *	Implementation of the truncate operation for LSM trees.
 */
static int
__lsm_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *wt_session,
    const char *uri, const char *cfg[])
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(dsrc);
	session = (WT_SESSION_IMPL *)wt_session;

	return (__wt_lsm_tree_truncate(session, uri, cfg));
}

/*
 * __wt_lsm_init --
 *	Initialize LSM structures during wiredtiger_open.
 */
int
__wt_lsm_init(WT_CONNECTION *wt_conn, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	WT_LSM_DATA_SOURCE *lsm_dsrc;
	WT_SESSION_IMPL *session;
	static WT_DATA_SOURCE iface = {
		__lsm_create,
		__lsm_drop,
		__lsm_open_cursor,
		__lsm_rename,
		__lsm_truncate
	};

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session = conn->default_session;

	WT_RET(__wt_calloc_def(session, 1, &lsm_dsrc));

	lsm_dsrc->iface = iface;
	WT_RET(
	    __wt_rwlock_alloc(session, "lsm data source", &lsm_dsrc->rwlock));

	return (wt_conn->add_data_source(wt_conn,
	    "lsm:", &lsm_dsrc->iface, config));
}

/*
 * __wt_lsm_cleanup --
 *	Clean up LSM structures during connection close.
 */
int
__wt_lsm_cleanup(WT_CONNECTION *wt_conn)
{
	WT_CONNECTION_IMPL *conn;
	WT_DATA_SOURCE *dsrc;
	WT_DECL_RET;
	WT_LSM_DATA_SOURCE *lsm_dsrc;
	WT_SESSION_IMPL *session;

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session = conn->default_session;

	if ((ret = __wt_schema_get_source(session, "lsm:", &dsrc)) == 0) {
		lsm_dsrc = (WT_LSM_DATA_SOURCE *)dsrc;
		ret = __wt_rwlock_destroy(session, &lsm_dsrc->rwlock);
		__wt_free(session, dsrc);
	}
	if (ret == WT_NOTFOUND)
		ret = 0;

	WT_TRET(__wt_lsm_tree_close_all(session));
	return (ret);
}
