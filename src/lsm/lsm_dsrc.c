/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__lsm_create(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__lsm_drop(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__lsm_open_cursor(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *obj, const char *config, WT_CURSOR **new_cursor)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(obj);
	WT_UNUSED(config);
	WT_UNUSED(new_cursor);

	return (ENOTSUP);
}

static int
__lsm_rename(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *oldname, const char *newname, const char *config)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(oldname);
	WT_UNUSED(newname);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__lsm_sync(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (ENOTSUP);
}

static int
__lsm_truncate(WT_DATA_SOURCE *dsrc, WT_SESSION *session,
    const char *name, const char *config)
{
	WT_UNUSED(dsrc);
	WT_UNUSED(session);
	WT_UNUSED(name);
	WT_UNUSED(config);

	return (ENOTSUP);
}

int
__wt_lsm_init(WT_CONNECTION *wt_conn, const char *config)
{
	WT_CONNECTION_IMPL *conn;
	static WT_LSM_DATA_SOURCE *lsm_dsrc;
	WT_SESSION_IMPL *session;
	static WT_DATA_SOURCE iface = {
		__lsm_create,
		__lsm_drop,
		__lsm_open_cursor,
		__lsm_rename,
		__lsm_sync,
		__lsm_truncate
	};

	conn = (WT_CONNECTION_IMPL *)wt_conn;
	session = conn->default_session;

	WT_RET(__wt_calloc_def(session, 1, &lsm_dsrc));

	lsm_dsrc->iface = iface;
	WT_RET(
	    __wt_rwlock_alloc(session, "lsm data source", &lsm_dsrc->rwlock));
	TAILQ_INIT(&lsm_dsrc->trees);

	return (wt_conn->add_data_source(wt_conn,
	    "lsm:", &lsm_dsrc->iface, config));
}
