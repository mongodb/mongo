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
__wt_db_open(DB *db, const char *file_name, mode_t mode, u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = db->env;

	DB_FLAG_CHK(db, "Db.open", flags, WT_APIMASK_DB_OPEN);

	/* Initialize the IDB structure. */
	if ((ret = __wt_db_idb_open(db, file_name, mode, flags)) != 0)
		return (ret);

	/* Open any private environment. */
	if (F_ISSET(env, WT_PRIVATE_ENV) &&
	    (ret = env->open(env, NULL, 0, 0)) != 0)
		return (ret);

	/* Insert the database on the environment's list. */
	TAILQ_INSERT_TAIL(&env->dbqh, db, q);

	/* Open the underlying Btree. */
	if ((ret = __wt_bt_open(db)) != 0)
		return (ret);

	/* Turn on the methods that require open. */
	__wt_db_config_methods_open(db);

	return (0);
}

/*
 * __wt_db_idb_open --
 *	Routine to intialize any IDB values based on a DB value during open.
 */
static int
__wt_db_idb_open(DB *db, const char *file_name, mode_t mode, u_int32_t flags)
{
	ENV *env;
	IDB *idb;
	int ret;

	env = db->env;
	idb = db->idb;

	if ((ret = __wt_strdup(env, file_name, &idb->file_name)) != 0)
		return (ret);
	idb->mode = mode;

	idb->file_id = ++WT_GLOBAL(file_id);

	if (LF_ISSET(WT_CREATE))
		F_SET(idb, WT_CREATE);

	return (0);
}
