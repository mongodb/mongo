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
 * __wt_bt_sync --
 *	Sync a Btree.
 */
int
__wt_bt_sync(DB *db)
{
	return (__wt_cache_sync(db));
}
