/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/* XXX -- should be auto-generated. */
static int
__txn_op_log(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_TXN_OP *op)
{
	const char *fmt = "IIuu";
	size_t size;
	uint32_t optype = 1;	/* XXX script should generate type IDs. */
	WT_ITEM value;

	value.data = WT_UPDATE_DATA(op->upd);
	value.size = op->upd->size;

	WT_RET(__wt_struct_size(
	    session, &size, fmt, 0, optype, &op->key, &value));
	/* We assumed we were packing zero into the size above, fix that. */
	size += __wt_vsize_posint(size) - 1;
	WT_RET(__wt_buf_grow(session, logrec, logrec->size + size));
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    (uint32_t)size, optype, &op->key, &value));
	logrec->size += size;
	return (0);
}

static int
__txn_op_printlog(WT_SESSION_IMPL *session, WT_ITEM *logrec, FILE *out)
{
	WT_UNUSED(session);

	if (fprintf(out, "%.*s%s\n",
	    (int)WT_MIN(logrec->size, 40), logrec->data,
	    (logrec->size > 40) ? "..."  : "") < 0)
		return (errno);

	return (0);
}

static int
__txn_commit_printlog(WT_SESSION_IMPL *session, WT_ITEM *logrec, FILE *out)
{
	WT_RET(__txn_op_printlog(session, logrec, out));
	return (0);
}

/*
 * __wt_txn_log_commit --
 *	Write the operations of a transaction to the log at commit time.
 */
int
__wt_txn_commit_log(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_ITEM *logrec;
	WT_LSN lsn;
	WT_TXN *txn;
	WT_TXN_OP *op;
	const char *header_fmt = "I";
	size_t header_size;
	uint32_t rectype = 1;
	u_int i;

	WT_UNUSED(cfg);
	txn = &session->txn;

	WT_RET(__wt_struct_size(session, &header_size, header_fmt, rectype));
	WT_RET(__wt_scr_alloc(
	    session, sizeof(WT_LOG_RECORD) + header_size, &logrec));
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record),
	    header_size, header_fmt, rectype));
	logrec->size = offsetof(WT_LOG_RECORD, record) + header_size;

	/* Write updates to the log. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		if (op->upd != NULL)
			WT_ERR(__txn_op_log(session, logrec, op));

		/* We can't handle physical truncate yet. */
		if (op->ref != NULL)
			return (ENOTSUP);
	}

	WT_ERR(__wt_log_write(session, logrec, &lsn, 0));

err:	__wt_scr_free(&logrec);
	return (ret);
}

/*
 * __wt_txn_printlog --
 *	Print the log in a human-readable format.
 */
int
__wt_txn_printlog(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	FILE *out;
	WT_ITEM item;
	const char *header_fmt = "I";
	const uint8_t *end, *p;
	int rectype;

	out = cookie;

	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* Every log record must start with the type. */
	WT_RET(__wt_struct_unpack(
	    session, p, WT_PTRDIFF(end, p), header_fmt, &rectype));

	if (fprintf(out, "[%" PRIu32 "/%" PRIu32 "] type %d\n",
	    lsnp->file, lsnp->offset, rectype))
		return (errno);

	switch (rectype) {
	case TXN_LOG_COMMIT:
		WT_RET(__txn_commit_printlog(session, logrec, out));
		break;
	}

	return (0);
}
