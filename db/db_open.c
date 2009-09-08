/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static int __wt_db_idb_open(DB *, const char *, mode_t, u_int32_t);

/*
 * __wt_db_open --
 *	Open a DB handle.
 */
int
__wt_db_open(WT_TOC *toc)
{
	wt_args_db_open_unpack;
	ENV *env;
	IENV *ienv;
	WT_STOC *stoc;
	IDB *idb;

	env = toc->env;
	ienv = env->ienv;
	idb = db->idb;

	WT_DB_FCHK(db, "Db.open", flags, WT_APIMASK_DB_OPEN);

	/* Initialize the IDB structure. */
	WT_RET((__wt_db_idb_open(db, dbname, mode, flags)));

	/*
	 * If we're using a single thread, reference it.   Otherwise create
	 * a server thread.
	 */
	if (F_ISSET(ienv, WT_SINGLE_THREADED))
		stoc = ienv->sq;
	else {
		stoc = ienv->sq + ienv->sq_next;
		stoc->id = ++ienv->sq_next;
		stoc->running = 1;
		stoc->ienv = ienv;
		WT_RET(__wt_thread_create(env, &stoc->tid, __wt_workq, stoc));
	}
	idb->stoc = stoc;
	stoc->idb = idb;

	/* Open the underlying Btree. */
	WT_RET((__wt_bt_open(db)));

	/* Turn on the methods that require open. */
	__wt_db_config_methods_open(db);

	return (0);
}

/*
 * __wt_db_idb_open --
 *	Routine to intialize any IDB values based on a DB value during open.
 */
static int
__wt_db_idb_open(DB *db, const char *dbname, mode_t mode, u_int32_t flags)
{
	ENV *env;
	IENV *ienv;
	IDB *idb;

	env = db->env;
	ienv = env->ienv;
	idb = db->idb;

	WT_RET((__wt_strdup(env, dbname, &idb->dbname)));
	idb->mode = mode;

	idb->file_id = ++ienv->file_id;

	if (LF_ISSET(WT_CREATE))
		F_SET(idb, WT_CREATE);

	return (0);
}
