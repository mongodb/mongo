/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_get_format --
 *	Find or allocate the uri/config/format structure.
 */
static int
__async_get_format(WT_CONNECTION_IMPL *conn, const char *uri,
    const char *config, WT_ASYNC_OP_IMPL *op)
{
	WT_ASYNC *async;
	WT_ASYNC_FORMAT *af;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	WT_SESSION_IMPL *session;
	uint64_t cfg_hash, uri_hash;

	async = conn->async;
	c = NULL;
	op->format = NULL;

	if (uri != NULL)
		uri_hash = __wt_hash_city64(uri, strlen(uri));
	else
		uri_hash = 0;
	if (config != NULL)
		cfg_hash = __wt_hash_city64(config, strlen(config));
	else
		cfg_hash = 0;

	/*
	 * We don't need to hold a lock around this walk.  The list is
	 * permanent and always valid.  We might race an insert and there
	 * is a possibility a duplicate entry might be inserted, but
	 * that is not harmful.
	 */
	TAILQ_FOREACH(af, &async->formatqh, q) {
		if (af->uri_hash == uri_hash && af->cfg_hash == cfg_hash)
			goto setup;
	}
	/*
	 * We didn't find one in the cache.  Allocate and initialize one.
	 * Insert it at the head expecting LRU usage.  We need a real session
	 * for the cursor.
	 */
	WT_RET(__wt_open_internal_session(
	    conn, "async-cursor", true, 0, &session));
	__wt_spin_lock(session, &async->ops_lock);
	WT_ERR(__wt_calloc_one(session, &af));
	WT_ERR(__wt_strdup(session, uri, &af->uri));
	WT_ERR(__wt_strdup(session, config, &af->config));
	af->uri_hash = uri_hash;
	af->cfg_hash = cfg_hash;
	/*
	 * Get the key_format and value_format for this URI and store
	 * it in the structure so that async->set_key/value work.
	 */
	wt_session = &session->iface;
	WT_ERR(wt_session->open_cursor(wt_session, uri, NULL, NULL, &c));
	WT_ERR(__wt_strdup(session, c->key_format, &af->key_format));
	WT_ERR(__wt_strdup(session, c->value_format, &af->value_format));
	WT_ERR(c->close(c));
	c = NULL;

	TAILQ_INSERT_HEAD(&async->formatqh, af, q);
	__wt_spin_unlock(session, &async->ops_lock);
	WT_ERR(wt_session->close(wt_session, NULL));

setup:	op->format = af;
	/*
	 * Copy the pointers for the formats.  Items in the async format
	 * queue remain there until the connection is closed.  We must
	 * initialize the format fields in the async_op, which are publicly
	 * visible, and its internal cursor used by internal key/value
	 * functions.
	 */
	op->iface.c.key_format = op->iface.key_format = af->key_format;
	op->iface.c.value_format = op->iface.value_format = af->value_format;
	return (0);

err:
	if (c != NULL)
		(void)c->close(c);
	__wt_free(session, af->uri);
	__wt_free(session, af->config);
	__wt_free(session, af->key_format);
	__wt_free(session, af->value_format);
	__wt_free(session, af);
	return (ret);
}

/*
 * __async_new_op_alloc --
 *	Find and allocate the next available async op handle.
 */
static int
__async_new_op_alloc(WT_SESSION_IMPL *session, const char *uri,
    const char *config, WT_ASYNC_OP_IMPL **opp)
{
	WT_ASYNC *async;
	WT_ASYNC_OP_IMPL *op;
	WT_CONNECTION_IMPL *conn;
	uint32_t i, save_i, view;

	conn = S2C(session);
	async = conn->async;
	WT_STAT_FAST_CONN_INCR(session, async_op_alloc);
	*opp = NULL;

retry:
	op = NULL;
	WT_ORDERED_READ(save_i, async->ops_index);
	/*
	 * Look after the last one allocated for a free one.  We'd expect
	 * ops to be freed mostly FIFO so we should quickly find one.
	 */
	for (view = 1, i = save_i; i < conn->async_size; i++, view++) {
		op = &async->async_ops[i];
		if (op->state == WT_ASYNCOP_FREE)
			break;
	}

	/*
	 * Loop around back to the beginning if we need to.
	 */
	if (op == NULL || op->state != WT_ASYNCOP_FREE)
		for (i = 0; i < save_i; i++, view++) {
			op = &async->async_ops[i];
			if (op->state == WT_ASYNCOP_FREE)
				break;
		}

	/*
	 * We still haven't found one.  Return an error.
	 */
	if (op == NULL || op->state != WT_ASYNCOP_FREE) {
		WT_STAT_FAST_CONN_INCR(session, async_full);
		WT_RET(EBUSY);
	}
	/*
	 * Set the state of this op handle as READY for the user to use.
	 * If we can set the state then the op entry is ours.
	 * Start the next search at the next entry after this one.
	 */
	if (!__wt_atomic_cas32(&op->state, WT_ASYNCOP_FREE, WT_ASYNCOP_READY)) {
		WT_STAT_FAST_CONN_INCR(session, async_alloc_race);
		goto retry;
	}
	WT_STAT_FAST_CONN_INCRV(session, async_alloc_view, view);
	WT_RET(__async_get_format(conn, uri, config, op));
	op->unique_id = __wt_atomic_add64(&async->op_id, 1);
	op->optype = WT_AOP_NONE;
	(void)__wt_atomic_store32(
	    &async->ops_index, (i + 1) % conn->async_size);
	*opp = op;
	return (0);
}

/*
 * __async_config --
 *	Parse and setup the async API options.
 */
static int
__async_config(WT_SESSION_IMPL *session,
    WT_CONNECTION_IMPL *conn, const char **cfg, bool *runp)
{
	WT_CONFIG_ITEM cval;

	/*
	 * The async configuration is off by default.
	 */
	WT_RET(__wt_config_gets(session, cfg, "async.enabled", &cval));
	*runp = cval.val != 0;

	/*
	 * Even if async is turned off, we want to parse and store the default
	 * values so that reconfigure can just enable them.
	 *
	 * Bound the minimum maximum operations at 10.
	 */
	WT_RET(__wt_config_gets(session, cfg, "async.ops_max", &cval));
	conn->async_size = (uint32_t)WT_MAX(cval.val, 10);

	WT_RET(__wt_config_gets(session, cfg, "async.threads", &cval));
	conn->async_workers = (uint32_t)cval.val;
	/* Sanity check that api_data.py is in sync with async.h */
	WT_ASSERT(session, conn->async_workers <= WT_ASYNC_MAX_WORKERS);

	return (0);
}

/*
 * __wt_async_stats_update --
 *	Update the async stats for return to the application.
 */
void
__wt_async_stats_update(WT_SESSION_IMPL *session)
{
	WT_ASYNC *async;
	WT_CONNECTION_IMPL *conn;
	WT_CONNECTION_STATS **stats;

	conn = S2C(session);
	async = conn->async;
	if (async == NULL)
		return;
	stats = conn->stats;
	WT_STAT_SET(session, stats, async_cur_queue, async->cur_queue);
	WT_STAT_SET(session, stats, async_max_queue, async->max_queue);
	F_SET(conn, WT_CONN_SERVER_ASYNC);
}

/*
 * __async_start --
 *	Start the async subsystem.  All configuration processing has
 *	already been done by the caller.
 */
static int
__async_start(WT_SESSION_IMPL *session)
{
	WT_ASYNC *async;
	WT_CONNECTION_IMPL *conn;
	uint32_t i, session_flags;

	conn = S2C(session);
	conn->async_cfg = 1;
	/*
	 * Async is on, allocate the WT_ASYNC structure and initialize the ops.
	 */
	WT_RET(__wt_calloc_one(session, &conn->async));
	async = conn->async;
	TAILQ_INIT(&async->formatqh);
	WT_RET(__wt_spin_init(session, &async->ops_lock, "ops"));
	WT_RET(__wt_cond_alloc(
	    session, "async flush", false, &async->flush_cond));
	WT_RET(__wt_async_op_init(session));

	/*
	 * Start up the worker threads.
	 */
	F_SET(conn, WT_CONN_SERVER_ASYNC);
	for (i = 0; i < conn->async_workers; i++) {
		/*
		 * Each worker has its own session.  We set both a general
		 * server flag in the connection and an individual flag
		 * in the session.  The user may reconfigure the number of
		 * workers and we may want to selectively stop some workers
		 * while leaving the rest running.
		 */
		session_flags = WT_SESSION_SERVER_ASYNC;
		WT_RET(__wt_open_internal_session(conn, "async-worker",
		    true, session_flags, &async->worker_sessions[i]));
	}
	for (i = 0; i < conn->async_workers; i++) {
		/*
		 * Start the threads.
		 */
		WT_RET(__wt_thread_create(session, &async->worker_tids[i],
		    __wt_async_worker, async->worker_sessions[i]));
	}
	__wt_async_stats_update(session);
	return (0);
}

/*
 * __wt_async_create --
 *	Start the async subsystem and worker threads.
 */
int
__wt_async_create(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_CONNECTION_IMPL *conn;
	bool run;

	conn = S2C(session);

	/* Handle configuration. */
	run = false;
	WT_RET(__async_config(session, conn, cfg, &run));

	/* If async is not configured, we're done. */
	if (!run)
		return (0);
	return (__async_start(session));
}

/*
 * __wt_async_reconfig --
 *	Start the async subsystem and worker threads.
 */
int
__wt_async_reconfig(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_ASYNC *async;
	WT_CONNECTION_IMPL *conn, tmp_conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	bool run;
	uint32_t i, session_flags;

	conn = S2C(session);
	async = conn->async;
	memset(&tmp_conn, 0, sizeof(tmp_conn));
	tmp_conn.async_cfg = conn->async_cfg;
	tmp_conn.async_workers = conn->async_workers;
	tmp_conn.async_size = conn->async_size;

	/* Handle configuration. */
	run = conn->async_cfg;
	WT_RET(__async_config(session, &tmp_conn, cfg, &run));

	/*
	 * There are some restrictions on the live reconfiguration of async.
	 * Unlike other subsystems where we simply destroy anything existing
	 * and restart with the new configuration, async is not so easy.
	 * If the user is just changing the number of workers, we want to
	 * allow the existing op handles and other information to remain in
	 * existence.  So we must handle various combinations of changes
	 * individually.
	 *
	 * One restriction is that if async is currently on, the user cannot
	 * change the number of async op handles available.  The user can try
	 * but we do nothing with it.  However we must allow the ops_max config
	 * string so that a user can completely start async via reconfigure.
	 */

	/*
	 * Easy cases:
	 * 1. If async is on and the user wants it off, shut it down.
	 * 2. If async is off, and the user wants it on, start it.
	 * 3. If not a toggle and async is off, we're done.
	 */
	if (conn->async_cfg > 0 && !run) {
		/* Case 1 */
		WT_TRET(__wt_async_flush(session));
		ret = __wt_async_destroy(session);
		conn->async_cfg = 0;
		return (ret);
	} else if (conn->async_cfg == 0 && run)
		/* Case 2 */
		return (__async_start(session));
	else if (conn->async_cfg == 0)
		/* Case 3 */
		return (0);

	/*
	 * Running async worker modification cases:
	 * 4. If number of workers didn't change, we're done.
	 * 5. If more workers, start new ones.
	 * 6. If fewer workers, kill some.
	 */
	if (conn->async_workers == tmp_conn.async_workers)
		/* No change in the number of workers. */
		return (0);
	if (conn->async_workers < tmp_conn.async_workers) {
		/* Case 5 */
		/*
		 * The worker_sessions array is allocated for the maximum
		 * allowed number of workers, so starting more is easy.
		 */
		for (i = conn->async_workers; i < tmp_conn.async_workers; i++) {
			/*
			 * Each worker has its own session.
			 */
			session_flags = WT_SESSION_SERVER_ASYNC;
			WT_RET(__wt_open_internal_session(conn, "async-worker",
			    true, session_flags, &async->worker_sessions[i]));
		}
		for (i = conn->async_workers; i < tmp_conn.async_workers; i++) {
			/*
			 * Start the threads.
			 */
			WT_RET(__wt_thread_create(session,
			    &async->worker_tids[i], __wt_async_worker,
			    async->worker_sessions[i]));
		}
		conn->async_workers = tmp_conn.async_workers;
	}
	if (conn->async_workers > tmp_conn.async_workers) {
		/* Case 6 */
		/*
		 * Stopping an individual async worker is the most complex case.
		 * We clear the session async flag on the targeted worker thread
		 * so that only that thread stops, and the others keep running.
		 */
		for (i = conn->async_workers - 1;
		    i >= tmp_conn.async_workers; i--) {
			/*
			 * Join any worker we're stopping.
			 * After the thread is stopped, close its session.
			 */
			WT_ASSERT(session, async->worker_tids[i] != 0);
			WT_ASSERT(session, async->worker_sessions[i] != NULL);
			F_CLR(async->worker_sessions[i],
			    WT_SESSION_SERVER_ASYNC);
			WT_TRET(__wt_thread_join(
			    session, async->worker_tids[i]));
			async->worker_tids[i] = 0;
			wt_session = &async->worker_sessions[i]->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
			async->worker_sessions[i] = NULL;
		}
		conn->async_workers = tmp_conn.async_workers;
	}

	return (0);
}

/*
 * __wt_async_destroy --
 *	Destroy the async worker threads and async subsystem.
 */
int
__wt_async_destroy(WT_SESSION_IMPL *session)
{
	WT_ASYNC *async;
	WT_ASYNC_FORMAT *af, *afnext;
	WT_ASYNC_OP *op;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	uint32_t i;

	conn = S2C(session);
	async = conn->async;

	if (!conn->async_cfg)
		return (0);

	F_CLR(conn, WT_CONN_SERVER_ASYNC);
	for (i = 0; i < conn->async_workers; i++)
		if (async->worker_tids[i] != 0) {
			WT_TRET(__wt_thread_join(
			    session, async->worker_tids[i]));
			async->worker_tids[i] = 0;
		}
	WT_TRET(__wt_cond_destroy(session, &async->flush_cond));

	/* Close the server threads' sessions. */
	for (i = 0; i < conn->async_workers; i++)
		if (async->worker_sessions[i] != NULL) {
			wt_session = &async->worker_sessions[i]->iface;
			WT_TRET(wt_session->close(wt_session, NULL));
			async->worker_sessions[i] = NULL;
		}
	/* Free any op key/value buffers. */
	for (i = 0; i < conn->async_size; i++) {
		op = (WT_ASYNC_OP *)&async->async_ops[i];
		if (op->c.key.data != NULL)
			__wt_buf_free(session, &op->c.key);
		if (op->c.value.data != NULL)
			__wt_buf_free(session, &op->c.value);
	}

	/* Free format resources */
	af = TAILQ_FIRST(&async->formatqh);
	while (af != NULL) {
		afnext = TAILQ_NEXT(af, q);
		__wt_free(session, af->uri);
		__wt_free(session, af->config);
		__wt_free(session, af->key_format);
		__wt_free(session, af->value_format);
		__wt_free(session, af);
		af = afnext;
	}
	__wt_free(session, async->async_queue);
	__wt_free(session, async->async_ops);
	__wt_spin_destroy(session, &async->ops_lock);
	__wt_free(session, conn->async);

	return (ret);
}

/*
 * __wt_async_flush --
 *	Implementation of the WT_CONN->async_flush method.
 */
int
__wt_async_flush(WT_SESSION_IMPL *session)
{
	WT_ASYNC *async;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	if (!conn->async_cfg)
		return (0);

	async = conn->async;
	WT_STAT_FAST_CONN_INCR(session, async_flush);
	/*
	 * We have to do several things.  First we have to prevent
	 * other callers from racing with us so that only one
	 * flush is happening at a time.  Next we have to wait for
	 * the worker threads to notice the flush and indicate
	 * that the flush is complete on their side.  Then we
	 * clear the flush flags and return.
	 */
retry:
	while (async->flush_state != WT_ASYNC_FLUSH_NONE)
		/*
		 * We're racing an in-progress flush.  We need to wait
		 * our turn to start our own.  We need to convoy the
		 * racing calls because a later call may be waiting for
		 * specific enqueued ops to be complete before this returns.
		 */
		__wt_sleep(0, 100000);

	if (!__wt_atomic_cas32(&async->flush_state, WT_ASYNC_FLUSH_NONE,
	    WT_ASYNC_FLUSH_IN_PROGRESS))
		goto retry;
	/*
	 * We're the owner of this flush operation.  Set the
	 * WT_ASYNC_FLUSH_IN_PROGRESS to block other callers.
	 * We're also preventing all worker threads from taking
	 * things off the work queue with the lock.
	 */
	async->flush_count = 0;
	(void)__wt_atomic_add64(&async->flush_gen, 1);
	WT_ASSERT(session, async->flush_op.state == WT_ASYNCOP_FREE);
	async->flush_op.state = WT_ASYNCOP_READY;
	WT_ERR(__wt_async_op_enqueue(session, &async->flush_op));
	while (async->flush_state != WT_ASYNC_FLUSH_COMPLETE)
		WT_ERR(__wt_cond_wait(NULL, async->flush_cond, 100000));
	/*
	 * Flush is done.  Clear the flags.
	 */
	async->flush_op.state = WT_ASYNCOP_FREE;
	WT_PUBLISH(async->flush_state, WT_ASYNC_FLUSH_NONE);
err:
	return (ret);
}

/*
 * __async_runtime_config --
 *	Configure runtime fields at allocation.
 */
static int
__async_runtime_config(WT_ASYNC_OP_IMPL *op, const char *cfg[])
{
	WT_ASYNC_OP *asyncop;
	WT_CONFIG_ITEM cval;
	WT_SESSION_IMPL *session;

	session = O2S(op);
	asyncop = (WT_ASYNC_OP *)op;
	WT_RET(__wt_config_gets_def(session, cfg, "append", 0, &cval));
	if (cval.val)
		F_SET(&asyncop->c, WT_CURSTD_APPEND);
	else
		F_CLR(&asyncop->c, WT_CURSTD_APPEND);
	WT_RET(__wt_config_gets_def(session, cfg, "overwrite", 1, &cval));
	if (cval.val)
		F_SET(&asyncop->c, WT_CURSTD_OVERWRITE);
	else
		F_CLR(&asyncop->c, WT_CURSTD_OVERWRITE);
	WT_RET(__wt_config_gets_def(session, cfg, "raw", 0, &cval));
	if (cval.val)
		F_SET(&asyncop->c, WT_CURSTD_RAW);
	else
		F_CLR(&asyncop->c, WT_CURSTD_RAW);
	return (0);

}

/*
 * __wt_async_new_op --
 *	Implementation of the WT_CONN->async_new_op method.
 */
int
__wt_async_new_op(WT_SESSION_IMPL *session, const char *uri,
    const char *config, const char *cfg[], WT_ASYNC_CALLBACK *cb,
    WT_ASYNC_OP_IMPL **opp)
{
	WT_ASYNC_OP_IMPL *op;
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	*opp = NULL;

	conn = S2C(session);
	if (!conn->async_cfg)
		return (ENOTSUP);

	op = NULL;
	WT_ERR(__async_new_op_alloc(session, uri, config, &op));
	WT_ERR(__async_runtime_config(op, cfg));
	op->cb = cb;
	*opp = op;
	return (0);

err:
	/*
	 * If we get an error after allocating op, set its state to free.
	 */
	if (op != NULL)
		op->state = WT_ASYNCOP_FREE;
	return (ret);
}
