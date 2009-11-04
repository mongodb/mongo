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
 * __wt_api_db_open --
 *	Open a DB handle.
 */
int
__wt_api_db_open(DB *db, const char *dbname, mode_t mode, u_int32_t flags)
{
	ENV *env;

	env = db->env;

	WT_DB_FCHK(db, "Db.open", flags, WT_APIMASK_DB_OPEN);

	WT_STAT_INCR(env->ienv->stats, DATABASE_OPEN, "database open");

	/* Initialize the IDB structure. */
	WT_RET(__wt_db_idb_open(db, dbname, mode, flags));

	/* Open the underlying Btree. */
	WT_RET(__wt_bt_open(db));

	/* Turn on the methods that require open. */
	__wt_methods_db_open_on(db);

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

	WT_RET(__wt_strdup(env, dbname, &idb->dbname));
	idb->mode = mode;

	__wt_lock(env, &ienv->mtx);
	idb->file_id = ++ienv->next_file_id;
	__wt_unlock(&ienv->mtx);

	if (LF_ISSET(WT_CREATE))
		F_SET(idb, WT_CREATE);

	return (0);
}
