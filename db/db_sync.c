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
 * __wt_api_db_sync --
 *	Flush a database to the backing file.
 */
int
__wt_api_db_sync(DB *db, u_int32_t flags)
{
	WT_DB_FCHK(db, "Db.sync", flags, WT_APIMASK_DB_SYNC);

	/* Close the underlying Btree. */
	return (__wt_bt_sync(db));
}
