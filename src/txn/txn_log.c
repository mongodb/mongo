/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* XXX -- should be auto-generated. */
static int
__txn_log_op(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_TXN_OP *op)
{
	const char *fmt = "iuu";
	size_t size;
	int optype = 1;	/* XXX script should generate type IDs. */
	WT_ITEM value;

	value.data = WT_UPDATE_DATA(op->upd);
	value.size = op->upd->size;

	WT_RET(__wt_struct_size(
	    session, &size, fmt, optype, op->key, &value));
	WT_RET(__wt_buf_grow(session, logrec, logrec->size + size));
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    optype, op->key, &value));
	logrec->size += size;
	return (0);
}

/*
 * __wt_txn_log_commit --
 *	Write the operations of a transaction to the log at commit time.
 */
int
__wt_txn_log_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_ITEM *logrec;
	WT_TXN *txn;
	WT_TXN_OP *op;
	u_int i;

	WT_UNUSED(cfg);
	txn = &session->txn;

	WT_RET(__wt_scr_alloc(session, 0, &logrec));

	/* Write updates to the log. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		if (op->upd != NULL)
			WT_RET(__txn_log_op(session, logrec, op));

		/* We can't handle physical truncate yet. */
		if (op->ref != NULL)
			return (ENOTSUP);
	}

	return (0);
}
