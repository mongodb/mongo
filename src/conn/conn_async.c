/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_config --
 *	Parse and setup the async API options.
 */
static int
__async_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG_ITEM cval, sval;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);

	/*
	 * The async configuration is off by default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "async.enabled", &cval));
	*runp = cval.val != 0;
	if (*runp == 0)
		return (0);

	WT_RET(__wt_config_gets(session, cfg, "async.ops_max", &cval));
	conn->async_size = cval.val;

	WT_RET(__wt_config_gets(session, cfg, "async.threads", &cval));
	conn->async_workers = cval.val;

	ret = 0;
err:
	return (ret);
}

/*
 * __wt_async_create --
 *	Start the async subsystem and worker threads.
 */
int
__wt_async_create(WT_CONNECTION_IMPL *conn, const char *cfg[])
{
	WT_ASYNC *async;
	WT_SESSION_IMPL *session;
	int i, run;

	session = conn->default_session;

	/* Handle configuration. */
	WT_RET(__async_config(session, cfg, &run));

	/* If async is not configured, we're done. */
	if (!run)
		return (0);

	conn->async_cfg = 1;
	/*
	 * Async is on, allocate the WT_ASYNC structure and initialize the ops.
	 */
	WT_RET(__wt_calloc(session, 1, sizeof(WT_ASYNC), &conn->async));
	async = conn->async;
	WT_RET(__wt_spin_init(session, &async->ops_lock, "ops"));
	WT_RET(__wt_spin_init(session, &async->opsq_lock, "ops queue"));
	WT_RET(__wt_cond_alloc(session, "async op", 0, &async->ops_cond));
	WT_RET(__wt_cond_alloc(session, "async flush", 0, &async->flush_cond));
	WT_RET(__wt_async_init(session));

	/*
	 * Start up the worker threads.
	 */
	for (i = 0; i < conn->async_workers; i++) {
		/*
		 * Each worker has its own session.
		 */
		WT_RET(__wt_open_session(
		    conn, 1, NULL, NULL, &async->worker_session[i]));
		async->worker_session[i]->name = "async-worker";
		/*
		 * Start the threads.
		 */
		WT_RET(__wt_thread_create(session, &async->worker_tids[i],
		    __wt_async_worker, async->worker_sessions[i]));
	}

	return (0);
}

/*
 * __wt_async_destroy --
 *	Destroy the async worker threads and async subsystem.
 */
int
__wt_async_destroy(WT_CONNECTION_IMPL *conn)
{
	WT_ASYNC *async;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	int i;

	session = conn->default_session;
	async = conn->async;

	if (!conn->async_cfg)
		return (0);
	for (i = 0; i < conn->async_workers; i++)
		if (async->worker_tids[i] != 0) {
			WT_TRET(__wt_cond_signal(session, async->ops_cond));
			WT_TRET(__wt_thread_join(
			    session, async->worker_tids[i]));
			async->worker_tids[i] = 0;
		}
	WT_TRET(__wt_cond_destroy(session, &async->ops_cond));
	WT_TRET(__wt_cond_destroy(session, &async->flush_cond));

	/* Close the server thread's session. */
	for (i = 0; i < conn->async_workers; i++)
		if (async->worker_session[i] != NULL) {
			wt_session = &async->worker_sessions[i]->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
			async->worker_sessions[i] = NULL;
		}

	__wt_spin_destroy(session, &async->ops_lock);
	__wt_spin_destroy(session, &async->opsq_lock);
	__wt_free(session, conn->async);

	return (ret);
}

int
__wt_async_flush(WT_CONNECTION_IMPL *conn)
{
	return (0);
}

int
__wt_async_new_op(WT_CONNECTION_IMPL *conn, const char *uri, const char *cfg[],
    WT_ASYNC_CALLBACK *callback, WT_ASYNC_OP **asyncopp)
{
	return (0);
}
