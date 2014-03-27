/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_new_op_alloc --
 *	Find and allocate the next available async op handle.
 */
static int
__async_new_op_alloc(WT_CONNECTION_IMPL *conn, WT_ASYNC_OP_IMPL **opp)
{
	WT_ASYNC *async;
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	uint32_t found, i, save_i;

	async = conn->async;
	session = conn->default_session;
	WT_STAT_FAST_CONN_INCR(conn->default_session, async_op_alloc);
	*opp = NULL;
	ret = 0;
	__wt_spin_lock(session, &async->ops_lock);
	save_i = async->ops_index;
	/*
	 * Look after the last one allocated for a free one.  We'd expect
	 * ops to be freed mostly FIFO so we should quickly find one.
	 */
	for (found = 0, i = save_i; i < conn->async_size; i++) {
		op = &async->async_ops[i];
		if (FLD_ISSET(op->state, WT_ASYNCOP_FREE)) {
			found = 1;
			break;
		}
	}
	/*
	 * Loop around back to the beginning if we need to.
	 */
	if (!found) {
		for (i = 0; i < save_i; i++) {
			op = &async->async_ops[i];
			if (FLD_ISSET(op->state, WT_ASYNCOP_FREE)) {
				found = 1;
				break;
			}
		}
	}
	if (!found) {
		ret = ENOMEM;
		WT_STAT_FAST_CONN_INCR(session, async_full);
		goto err;
	}
	/*
	 * Start the next search at the next entry after this one.
	 * Set the state of this op handle as READY for the user to use.
	 */
	FLD_SET(op->state, WT_ASYNCOP_READY);
	op->unique_id = async->op_id++;
	async->ops_index = (i + 1) % conn->async_size;
	*opp = op;
err:	__wt_spin_unlock(session, &async->ops_lock);
	return (ret);
}

/*
 * __async_config --
 *	Parse and setup the async API options.
 */
static int
__async_config(WT_SESSION_IMPL *session, const char **cfg, int *runp)
{
	WT_CONFIG_ITEM cval;
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
	int run;
	uint32_t i;

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
	WT_RET(__wt_async_op_init(conn));

	/*
	 * Start up the worker threads.
	 */
	for (i = 0; i < conn->async_workers; i++) {
		/*
		 * Each worker has its own session.
		 */
		WT_RET(__wt_open_session(
		    conn, 1, NULL, NULL, &async->worker_sessions[i]));
		async->worker_sessions[i]->name = "async-worker";
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
	WT_SESSION_IMPL *session;
	uint32_t i;

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
		if (async->worker_sessions[i] != NULL) {
			wt_session = &async->worker_sessions[i]->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
			async->worker_sessions[i] = NULL;
		}

	__wt_spin_destroy(session, &async->ops_lock);
	__wt_spin_destroy(session, &async->opsq_lock);
	__wt_free(session, conn->async);

	return (ret);
}

/*
 * __wt_async_flush --
 *	Implementation of the WT_CONN->async_flush method.
 */
int
__wt_async_flush(WT_CONNECTION_IMPL *conn)
{
	WT_ASYNC *async;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	if (!conn->async_cfg)
		return (0);

	async = conn->async;
	session = conn->default_session;
	WT_STAT_FAST_CONN_INCR(session, async_flush);
	fprintf(stderr, "Async flush called\n");
	/*
	 * We have to do several things.  First we have to prevent
	 * other callers from racing with us so that only one
	 * flush is happening at a time.  Next we have to wait for
	 * the worker threads to notice the flush and indicate
	 * that the flush is complete on their side.  Then we
	 * clear the flush flags and return.
	 */
	__wt_spin_lock(session, &async->opsq_lock);
	fprintf(stderr, "Async flush locked, check race\n");
	if (FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSH_IN_PROGRESS))
		goto err;

	/*
	 * We're the owner of this flush operation.  Set the
	 * WT_ASYNC_FLUSH_IN_PROGRESS to prevent other callers.
	 * We're also preventing all worker threads from taking
	 * things off the work queue with the lock.
	 */
	fprintf(stderr, "Async flush: set in progress\n");
	FLD_SET(async->opsq_flush, WT_ASYNC_FLUSH_IN_PROGRESS);
	async->flush_count = 0;
	WT_ERR(__wt_async_op_enqueue(conn, &async->flush_op, 1));
	fprintf(stderr, "Async flush: enqueued op, wait for complete\n");
	while (!FLD_ISSET(async->opsq_flush, WT_ASYNC_FLUSH_COMPLETE)) {
		__wt_spin_unlock(session, &async->opsq_lock);
		WT_ERR_TIMEDOUT_OK(
		    __wt_cond_wait(NULL, async->flush_cond, 100000));
		__wt_spin_lock(session, &async->opsq_lock);
	}
	/*
	 * Flush is done.  Clear the flags.
	 */
	fprintf(stderr, "Async flush: complete.  clear flags\n");
	FLD_CLR(async->opsq_flush,
	   (WT_ASYNC_FLUSH_COMPLETE | WT_ASYNC_FLUSH_IN_PROGRESS));
err:	__wt_spin_unlock(session, &async->opsq_lock);
	return (ret);
}

/*
 * __wt_async_new_op --
 *	Implementation of the WT_CONN->async_new_op method.
 */
int
__wt_async_new_op(WT_CONNECTION_IMPL *conn, const char *uri,
    const char *config, WT_ASYNC_CALLBACK *cb,
    WT_ASYNC_OP_IMPL **opp)
{
	WT_ASYNC *async;
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	if (!conn->async_cfg)
		return (0);

	async = conn->async;
	session = conn->default_session;
	*opp = NULL;

	WT_ERR(__async_new_op_alloc(conn, &op));
	WT_ERR(__wt_strdup(session, uri, &op->uri));
	WT_ERR(__wt_strdup(session, config, &op->config));
	if (uri != NULL)
		op->uri_hash = __wt_hash_city64(uri, strlen(uri));
	else
		op->uri_hash = 0;
	if (config != NULL)
		op->cfg_hash = __wt_hash_city64(config, strlen(config));
	else
		op->cfg_hash = 0;
	op->cb = cb;

	*opp = op;

err:
	return (ret);
}
