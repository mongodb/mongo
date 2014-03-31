/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_get_key --
 *	WT_ASYNC_OP->get_key implementation for op handles.
 */
static int
__async_get_key(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	fprintf(stderr, "async_get_key: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return (0);
}

/*
 * __async_get_value --
 *	WT_ASYNC_OP->get_value implementation for op handles.
 */
static int
__async_get_value(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	fprintf(stderr, "async_get_value: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return (0);
}

/*
 * __async_set_key --
 *	WT_ASYNC_OP->set_key implementation for op handles.
 */
static void
__async_set_key(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	fprintf(stderr, "async_set_key: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return;
}

/*
 * __async_set_value --
 *	WT_ASYNC_OP->set_value implementation for op handles.
 */
static void
__async_set_value(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	fprintf(stderr, "async_set_value: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return;
}

/*
 * __async_op_wrap --
 *	Common wrapper for all async operations.
 */
static int
__async_op_wrap(WT_ASYNC_OP_IMPL *op, WT_ASYNC_OPTYPE type)
{
	op->optype = type;
	return (__wt_async_op_enqueue(O2C(op), op, 0));
}

/*
 * __async_search --
 *	WT_ASYNC_OP->search implementation for op handles.
 */
static int
__async_search(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_search);
	fprintf(stderr, "async_search: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return (__async_op_wrap(op, WT_AOP_SEARCH));
}

/*
 * __async_insert --
 *	WT_ASYNC_OP->insert implementation for op handles.
 */
static int
__async_insert(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;

	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_insert);
	fprintf(stderr, "async_insert: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	return (__async_op_wrap(op, WT_AOP_INSERT));
}

/*
 * __async_update --
 *	WT_ASYNC_OP->update implementation for op handles.
 */
static int
__async_update(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_update);
	fprintf(stderr, "async_update: called\n");
	return (__async_op_wrap(op, WT_AOP_UPDATE));
}

/*
 * __async_remove --
 *	WT_ASYNC_OP->remove implementation for op handles.
 */
static int
__async_remove(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_remove);
	fprintf(stderr, "async_remove: called\n");
	return (__async_op_wrap(op, WT_AOP_REMOVE));
}

/*
 * __async_get_id --
 *	WT_ASYNC_OP->get_id implementation for op handles.
 */
static uint64_t
__async_get_id(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	fprintf(stderr, "async_get_id: returning %" PRIu64 "\n", op->unique_id);
	return (op->unique_id);
}

/*
 * __async_op_init --
 *	Initialize all the op handle fields.
 */
static int
__async_op_init(WT_CONNECTION_IMPL *conn, WT_ASYNC_OP_IMPL *op, uint32_t id)
{
	static const WT_ASYNC_OP stds = {
	    NULL, NULL, NULL,
	    __async_get_key,		/* get-key */
	    __async_get_value,		/* get-value */
	    __async_set_key,		/* set-key */
	    __async_set_value,		/* set-value */
	    __async_search,		/* search */
	    __async_insert,		/* insert */
	    __async_update,		/* update */
	    __async_remove,		/* remove */
	    __async_get_id		/* get-id */
	};

	op->iface = stds;
	op->iface.connection = (WT_CONNECTION *)conn;
	op->internal_id = id;
	op->state = WT_ASYNCOP_FREE;
	return (0);
}

/*
 * __wt_async_op_enqueue --
 *	Enqueue an operation onto the work queue.
 */
int
__wt_async_op_enqueue(WT_CONNECTION_IMPL *conn,
    WT_ASYNC_OP_IMPL *op, int locked)
{
	WT_ASYNC *async;
	WT_DECL_RET;

	async = conn->async;
	if (!locked)
		__wt_spin_lock(conn->default_session, &async->opsq_lock);
	/*
	 * Enqueue op at the tail of the work queue.
	 */
	WT_ASSERT(conn->default_session, op->state == WT_ASYNCOP_READY);
	STAILQ_INSERT_TAIL(&async->opqh, op, q);
	fprintf(stderr, "Enqueue op %d %" PRIu64 "\n", op->internal_id, op->unique_id);
	op->state = WT_ASYNCOP_ENQUEUED;
	if (++async->cur_queue > async->max_queue)
		async->max_queue = async->cur_queue;
	/*
	 * Signal the worker threads something is on the queue.
	 */
	__wt_spin_unlock(conn->default_session, &async->opsq_lock);
	WT_ERR(__wt_cond_signal(conn->default_session, async->ops_cond));
	/*
	 * Lock again if we need to for the caller.
	 */
err:
	if (locked)
		__wt_spin_lock(conn->default_session, &async->opsq_lock);
	return (ret);
}

/*
 * __wt_async_op_init --
 *	Initialize all the op handles.
 */
int
__wt_async_op_init(WT_CONNECTION_IMPL *conn)
{
	WT_ASYNC *async;
	WT_ASYNC_OP_IMPL *op;
	uint32_t i;

	async = conn->async;
	op = &async->flush_op;
	__async_op_init(conn, op, OPS_INVALID_INDEX);
	for (i = 0; i < WT_ASYNC_MAX_OPS; i++) {
		op = &async->async_ops[i];
		__async_op_init(conn, op, i);
	}
	return (0);
}
