/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_notsup --
 *	WT_CURSOR->XXX method for unsupported cursor actions.
 */
int
__wt_cursor_notsup(WT_CURSOR *cursor)
{
	WT_UNUSED(cursor);
	return (ENOTSUP);
}
