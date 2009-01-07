/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

static void __wt_db_idb_setup(DB *);

/*
 * __wt_db_open --
 *	Open a DB handle.
 */
int
__wt_db_open(DB *db, const char *file_name, mode_t mode, u_int32_t flags)
{
	IDB *idb;
	IENV *ienv;
	int ret;

	ienv = db->ienv;
	idb = db->idb;

	DB_FLAG_CHK(db, "Db.open", flags, WT_APIMASK_DB_OPEN);

	if ((ret = __wt_strdup(ienv, file_name, &idb->file_name)) != 0)
		goto err;
	idb->mode = mode;
	F_SET(idb, LF_ISSET(WT_CREATE));

	/* Initialize the IDB structure. */
	__wt_db_idb_setup(db);

	/* Open the underlying Btree. */
	if ((ret = __wt_bt_open(db)) != 0)
		goto err;

	/* Turn on the methods that require open. */
	__wt_db_config_methods_open(db);
	
	return (0);

err:	__wt_idb_destroy(db, 1);
	return (ret);
}

/*
 * __wt_db_idb_setup --
 *	Routine to intialize any IDB values based on a DB value during open.
 */
static void
__wt_db_idb_setup(DB *db)
{
	IDB *idb;

	idb = db->idb;

	idb->fileid = ++WT_GLOBAL(file_id);
}
