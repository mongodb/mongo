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
 * __wt_db_sync --
 *	Flush a database to the backing file.
 */
int
__wt_db_sync(DB *db, u_int32_t flags)
{
	DB_FLAG_CHK(db, "Db.sync", flags, WT_APIMASK_DB_SYNC);

	return (__wt_db_page_sync(db));
}
