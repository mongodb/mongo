/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __async_kv_not_set --
 *	Standard error message for op key/values not set.
 */
static int
__async_kv_not_set(WT_ASYNC_OP_IMPL *op, int key)
{
	WT_SESSION_IMPL *session;

	session = O2S(op);
	WT_RET_MSG(session,
	    op->saved_err == 0 ? EINVAL : op->saved_err,
	    "requires %s be set", key ? "key" : "value");
}

/*
 * __async_get_keyv --
 *	WT_ASYNC_OP->get_key implementation for op handles.
 */
static int
__async_get_keyv(WT_ASYNC_OP_IMPL *op, uint32_t flags, va_list ap)
{
	WT_ITEM *key;

	WT_UNUSED(flags);
	if (!F_ISSET(op, WT_ASYNCOP_KEY_EXT | WT_ASYNCOP_KEY_INT))
		WT_RET(__async_kv_not_set(op, 1));

	key = va_arg(ap, WT_ITEM *);
	key->data = op->key.data;
	key->size = op->key.size;
	return (0);
}

/*
 * __async_set_keyv --
 *	WT_ASYNC_OP->set_key implementation for op handles.
 */
static void
__async_set_keyv(WT_ASYNC_OP_IMPL *op, uint32_t flags, va_list ap)
{
	WT_ITEM *item;
	size_t sz;

	F_CLR(op, WT_ASYNCOP_KEY_SET);
	/*
	 * Default everything to raw for now.
	 */
	if (1 || LF_ISSET(WT_ASYNCOP_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		op->key.data = item->data;
		op->key.size = sz;
		op->saved_err = 0;
		F_SET(op, WT_ASYNCOP_KEY_EXT);
	}
}

/*
 * __async_get_valuev --
 *	WT_ASYNC_OP->get_value implementation for op handles.
 */
static int
__async_get_valuev(WT_ASYNC_OP_IMPL *op, uint32_t flags, va_list ap)
{
	WT_ITEM *value;

	WT_UNUSED(flags);
	if (!F_ISSET(op, WT_ASYNCOP_VALUE_EXT | WT_ASYNCOP_VALUE_INT))
		WT_RET(__async_kv_not_set(op, 0));

	value = va_arg(ap, WT_ITEM *);
	value->data = op->value.data;
	value->size = op->value.size;
	return (0);
}

/*
 * __async_set_valuev --
 *	WT_ASYNC_OP->set_value implementation for op handles.
 */
static void
__async_set_valuev(WT_ASYNC_OP_IMPL *op, uint32_t flags, va_list ap)
{
	WT_ITEM *item;
	size_t sz;

	F_CLR(op, WT_ASYNCOP_VALUE_SET);
	/*
	 * Default everything to raw for now.
	 */
	if (1 || LF_ISSET(WT_ASYNCOP_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		op->value.data = item->data;
		op->value.size = sz;
		op->saved_err = 0;
		F_SET(op, WT_ASYNCOP_VALUE_EXT);
	}
}

/*
 * __async_get_key --
 *	WT_ASYNC_OP->get_key implementation for op handles.
 */
static int
__async_get_key(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, get_key);
	va_start(ap, asyncop);
	fprintf(stderr, "async_get_key: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	ret = __async_get_keyv(op, op->flags, ap);
	va_end(ap);
	API_END(session);
err:
	return (ret);
}

/*
 * __async_get_value --
 *	WT_ASYNC_OP->get_value implementation for op handles.
 */
static int
__async_get_value(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, get_value);
	va_start(ap, asyncop);
	fprintf(stderr, "async_get_value: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	ret = __async_get_valuev(op, op->flags, ap);
	va_end(ap);
	API_END(session);
err:
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, set_key);
	va_start(ap, asyncop);
	__async_set_keyv(op, op->flags, ap);
	fprintf(stderr, "async_set_key: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	va_end(ap);
	if (0) {
err:		op->saved_err = ret;
	}
	API_END(session);
}

/*
 * __async_set_value --
 *	WT_ASYNC_OP->set_value implementation for op handles.
 */
static void
__async_set_value(WT_ASYNC_OP *asyncop, ...)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, set_value);
	va_start(ap, asyncop);
	__async_set_valuev(op, op->flags, ap);
	fprintf(stderr, "async_set_value: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	va_end(ap);
	if (0) {
err:		op->saved_err = ret;
	}
	API_END(session);
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
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, search);
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_search);
	fprintf(stderr, "async_search: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	WT_ERR(__async_op_wrap(op, WT_AOP_SEARCH));
err:	API_END(session);
	return (ret);
}

/*
 * __async_insert --
 *	WT_ASYNC_OP->insert implementation for op handles.
 */
static int
__async_insert(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, insert);

	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_insert);
	fprintf(stderr, "async_insert: called id %d uniq %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	WT_ERR(__async_op_wrap(op, WT_AOP_INSERT));
err:	API_END(session);
	return (ret);
}

/*
 * __async_update --
 *	WT_ASYNC_OP->update implementation for op handles.
 */
static int
__async_update(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, update);
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_update);
	fprintf(stderr, "async_update: called\n");
	WT_ERR(__async_op_wrap(op, WT_AOP_UPDATE));
err:	API_END(session);
	return (ret);
}

/*
 * __async_remove --
 *	WT_ASYNC_OP->remove implementation for op handles.
 */
static int
__async_remove(WT_ASYNC_OP *asyncop)
{
	WT_ASYNC_OP_IMPL *op;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	ASYNCOP_API_CALL(O2C(op), session, remove);
	WT_STAT_FAST_CONN_INCR(O2S(op), async_op_remove);
	fprintf(stderr, "async_remove: called\n");
	WT_ERR(__async_op_wrap(op, WT_AOP_REMOVE));
err:	API_END(session);
	return (ret);
}

/*
 * __async_get_id --
 *	WT_ASYNC_OP->get_id implementation for op handles.
 */
static uint64_t
__async_get_id(WT_ASYNC_OP *asyncop)
{
	return (((WT_ASYNC_OP_IMPL *)asyncop)->unique_id);
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
	fprintf(stderr, "STATE FREE %d %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
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
	op->state = WT_ASYNCOP_ENQUEUED;
	fprintf(stderr, "STATE ENQUEUE %d %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
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
