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
__async_kv_not_set(WT_ASYNC_OP *asyncop, int key)
{
	WT_ASYNC_OP_IMPL *op;
	WT_SESSION_IMPL *session;

	op = (WT_ASYNC_OP_IMPL *)asyncop;
	session = O2S(op);
	WT_RET_MSG(session,
	    asyncop->saved_err == 0 ? EINVAL : asyncop->saved_err,
	    "requires %s be set", key ? "key" : "value");
}

/*
 * __async_get_keyv --
 *	WT_ASYNC_OP->get_key implementation for op handles.
 */
static int
__async_get_keyv(WT_ASYNC_OP *asyncop, uint32_t flags, va_list ap)
{
	WT_ITEM *key;

	WT_UNUSED(flags);
	if (!F_ISSET(asyncop, WT_ASYNCOP_KEY_SET))
		WT_RET(__async_kv_not_set(asyncop, 1));

	key = va_arg(ap, WT_ITEM *);
	key->data = asyncop->key.data;
	key->size = asyncop->key.size;
	return (0);
}

/*
 * __async_set_keyv --
 *	WT_ASYNC_OP->set_key implementation for op handles.
 */
static void
__async_set_keyv(WT_ASYNC_OP *asyncop, uint32_t flags, va_list ap)
{
	WT_ITEM *item;
	size_t sz;

	F_CLR(asyncop, WT_ASYNCOP_KEY_SET);
	if (LF_ISSET(WT_ASYNCOP_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		asyncop->key.data = item->data;
		asyncop->key.size = sz;
		asyncop->saved_err = 0;
		F_SET(asyncop, WT_ASYNCOP_KEY_EXT);
	}
}

/*
 * __async_get_valuev --
 *	WT_ASYNC_OP->get_value implementation for op handles.
 */
static int
__async_get_valuev(WT_ASYNC_OP *asyncop, uint32_t flags, va_list ap)
{
	WT_ITEM *value;

	WT_UNUSED(flags);
	if (!F_ISSET(asyncop, WT_ASYNCOP_VALUE_SET))
		WT_RET(__async_kv_not_set(asyncop, 0));

	value = va_arg(ap, WT_ITEM *);
	value->data = asyncop->value.data;
	value->size = asyncop->value.size;
	return (0);
}

/*
 * __async_set_valuev --
 *	WT_ASYNC_OP->set_value implementation for op handles.
 */
static void
__async_set_valuev(WT_ASYNC_OP *asyncop, uint32_t flags, va_list ap)
{
	WT_ITEM *item;
	size_t sz;

	F_CLR(asyncop, WT_ASYNCOP_VALUE_SET);
	if (LF_ISSET(WT_ASYNCOP_RAW)) {
		item = va_arg(ap, WT_ITEM *);
		sz = item->size;
		asyncop->value.data = item->data;
		asyncop->value.size = sz;
		asyncop->saved_err = 0;
		F_SET(asyncop, WT_ASYNCOP_VALUE_EXT);
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
	fprintf(stderr, "async_get_key: called id %d unique %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	ret = __async_get_keyv(asyncop, asyncop->flags, ap);
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
	fprintf(stderr, "async_get_value: called id %d unique %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	ret = __async_get_valuev(asyncop, asyncop->flags, ap);
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
	__async_set_keyv(asyncop, asyncop->flags, ap);
	fprintf(stderr, "async_set_key: key_format %s id %d unique %"
	    PRIu64 " key %s\n", op->iface.key_format,
	    op->internal_id, op->unique_id, (char *)asyncop->key.data);
	va_end(ap);
	if (0) {
err:		asyncop->saved_err = ret;
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
	__async_set_valuev(asyncop, asyncop->flags, ap);
	fprintf(stderr, "async_set_value: called id %d unique %" PRIu64 "\n",
	    op->internal_id, op->unique_id);
	va_end(ap);
	if (0) {
err:		asyncop->saved_err = ret;
	}
	API_END(session);
}

/*
 * __wt_async_set_raw_value --
 *	Set value via WT_ITEM.
 */
void
__wt_async_set_raw_value(WT_ASYNC_OP *asyncop, WT_ITEM *value)
{
	int raw_set;

	raw_set = F_ISSET(asyncop, WT_ASYNCOP_RAW) ? 1 : 0;
	if (!raw_set)
		F_SET(asyncop, WT_ASYNCOP_RAW);
	asyncop->set_value(asyncop, value);
	if (!raw_set)
		F_CLR(asyncop, WT_ASYNCOP_RAW);
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
	WT_ASYNC_OP *asyncop;

	asyncop = (WT_ASYNC_OP *)op;
	asyncop->connection = (WT_CONNECTION *)conn;
	asyncop->key_format = asyncop->value_format = NULL;
	asyncop->get_key = __async_get_key;
	asyncop->get_value = __async_get_value;
	asyncop->set_key = __async_set_key;
	asyncop->set_value = __async_set_value;
	asyncop->search = __async_search;
	asyncop->insert = __async_insert;
	asyncop->update = __async_update;
	asyncop->remove = __async_remove;
	asyncop->get_id = __async_get_id;
	asyncop->recno = 0;
	memset(&asyncop->raw_recno_buf, 0, sizeof(asyncop->raw_recno_buf));
	memset(&asyncop->key, 0, sizeof(asyncop->key));
	memset(&asyncop->value, 0, sizeof(asyncop->value));
	asyncop->saved_err = 0;
	asyncop->flags = 0;

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
