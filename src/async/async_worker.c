/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_worker --
 *	The async worker threads.
 */
void *
__wt_async_worker(void *arg)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	session = arg;
	conn = S2C(session);

	while (F_ISSET(conn, WT_CONN_SERVER_RUN)) {
		/* Wait until the next event. */
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(session, async->ops_cond, 1000000));
	}

	if (0) {
err:		__wt_err(session, ret, "async worker error");
	}
	return (NULL);
}
