/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_cursor_key_not_set --
 *	WT_CURSOR->XXX method error when a key hasn't been set.
 */
int
__wt_cursor_key_not_set(WT_CURSOR *cursor, const char *method)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)cursor->session;
	__wt_errx(session,
	    "cursor %s method not supported until a key is set", method);
	return (WT_ERROR);
}

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
