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
 * __wt_db_sync --
 *	Flush a database to the backing file.
 */
int
__wt_db_sync(WT_TOC *toc, void (*f)(const char *, u_int64_t), u_int32_t flags)
{
	return (__wt_bt_sync(toc, f, flags));
}
