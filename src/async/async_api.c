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
__async_get_key(WT_ASYNC_OP *op, ...)
{
	return (0);
}

/*
 * __async_get_value --
 *	WT_ASYNC_OP->get_value implementation for op handles.
 */
static int
__async_get_value(WT_ASYNC_OP *op, ...)
{
	return (0);
}

/*
 * __async_set_key --
 *	WT_ASYNC_OP->set_key implementation for op handles.
 */
static int
__async_set_key(WT_ASYNC_OP *op, ...)
{
	return (0);
}

/*
 * __async_set_value --
 *	WT_ASYNC_OP->set_value implementation for op handles.
 */
static int
__async_set_value(WT_ASYNC_OP *op, ...)
{
	return (0);
}

/*
 * __async_search --
 *	WT_ASYNC_OP->search implementation for op handles.
 */
static int
__async_search(WT_ASYNC_OP *op)
{
	return (0);
}

/*
 * __async_insert --
 *	WT_ASYNC_OP->insert implementation for op handles.
 */
static int
__async_insert(WT_ASYNC_OP *op)
{
	return (0);
}

/*
 * __async_update --
 *	WT_ASYNC_OP->update implementation for op handles.
 */
static int
__async_update(WT_ASYNC_OP *op)
{
	return (0);
}

/*
 * __async_remove --
 *	WT_ASYNC_OP->remove implementation for op handles.
 */
static int
__async_remove(WT_ASYNC_OP *op)
{
	return (0);
}

/*
 * __async_get_id --
 *	WT_ASYNC_OP->get_id implementation for op handles.
 */
static uint64_t
__async_get_id(WT_ASYNC_OP *op)
{
	return (op->unique_id);
}

/*
 * Initialize all the op handles.
 */
static int
__async_op_init(WT_CONNECTION_IMPL *conn)
{
	WT_ASYNC_OP_STATIC_INIT(iface,
	    __async_get_key,		/* get-key */
	    __async_get_value,		/* get-value */
	    __async_set_key,		/* set-key */
	    __async_set_value,		/* set-value */
	    __async_search,		/* search */
	    __async_insert,		/* insert */
	    __async_update,		/* update */
	    __async_remove,		/* remove */
	    __async_get_id);		/* get-id */
	WT_ASYNC *async;
	WT_ASYNC_OP *op;
	uint32_t i;
	int ret;

	async = conn->async;
	for (i = 0; i < WT_ASYNC_MAX_OPS; i++) {
		op = &async->async_ops[i];
		*op = iface;
		op->conn = conn;
		op->internal_id = i;
		op->state = WT_ASYNCOP_FREE;
	}
}

int
__wt_async_init(WT_SESSION_IMPL *session)
{
	
}
