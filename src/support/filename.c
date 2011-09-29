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
__wt_filename(WT_SESSION_IMPL *session, const char *name, WT_BUF **retp)
{
	WT_BUF *tmp;
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);

	WT_RET(__wt_scr_alloc(
	    session, (uint32_t)(strlen(conn->home) + strlen(name) + 2), &tmp));
	(void)snprintf(tmp->mem, tmp->memsize, "%s/%s", conn->home, name);
	tmp->size = (uint32_t)strlen(tmp->data);

	*retp = tmp;
	return (0);
}
