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
 * __wt_db_close --
 *	Close a DB handle.
 */
int
__wt_db_close(DB *db, u_int32_t flags)
{
	DB_FLAG_CHK(db, "Db.close", flags, WT_APIMASK_DB_CLOSE);

	/* Close the underlying Btree. */
	return (__wt_bt_close(db));
}
