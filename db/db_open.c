/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008 WiredTiger Software.
 *	All rights reserved.
 *
 * $Id$
 */

#include "wt_internal.h"

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

	/* Open the underlying Btree. */
	if ((ret = __wt_bt_open(db)) != 0)
		goto err;
	
	return (0);

err:	(void)__wt_idb_destroy(db, 1);
	return (WT_ERROR);
}
