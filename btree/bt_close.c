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
 * __wt_bt_close --
 *	Close a Btree.
 */
int
__wt_bt_close(DB *db)
{
	/* Close the underlying database file. */
	return (__wt_cache_db_close(db));
}
