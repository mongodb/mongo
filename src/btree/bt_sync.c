/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_btree_sync --
 *	Sync the tree.
 */
int
__wt_btree_sync(WT_SESSION_IMPL *session)
{
	/* Ask the eviction thread to flush any dirty pages. */
	return (__wt_evict_file_serial(session, 0));
}
