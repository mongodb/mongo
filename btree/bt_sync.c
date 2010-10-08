/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2010 WiredTiger, Inc.
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
__wt_bt_sync(WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags)
{
	IDB *idb;

	idb = toc->db->idb;
	if (WT_UNOPENED_DATABASE(idb))
		return (0);

	return (__wt_cache_sync(toc, f, flags));
}
