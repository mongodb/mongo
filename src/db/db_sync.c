/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_sync --
 *	Flush pages to the backing file.
 */
int
__wt_btree_sync(SESSION *session, uint32_t flags)
{
	WT_UNUSED(flags);

	return (__wt_bt_sync(session));
}
