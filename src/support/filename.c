/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_filename --
 *	Build a filename in a scratch buffer.
 */
int
__wt_filename(WT_SESSION_IMPL *session, const char *name, const char **path)
{
	WT_BUF tmp;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_CLEAR(tmp);
	WT_RET(__wt_buf_sprintf(session, &tmp, "%s/%s", conn->home, name));
	*path = __wt_buf_steal(session, &tmp, NULL);
	return (0);
}
