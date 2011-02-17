/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_db_sync --
 *	Flush pages to the backing file.
 */
int
__wt_db_sync(WT_TOC *toc, void (*f)(const char *, uint64_t), uint32_t flags)
{
	return (__wt_bt_sync(toc));
}
