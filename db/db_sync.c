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
__wt_db_sync(DB *db, void (*f)(const char *, u_int32_t), u_int32_t flags)
{
	/* Close the underlying Btree. */
	return (__wt_bt_sync(db, f));
}
