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
	const char *fmt = "IISuu";
	size_t size;
	uint32_t optype = 1, recsize;	/* XXX script should generate type IDs. */
	WT_ITEM value;

	/* XXX deal with the distinction between inserts, updates and removes */
	value.data = WT_UPDATE_DATA(op->upd);
	value.size = WT_UPDATE_DELETED_SET(op->upd) ? 0 : op->upd->size;

	WT_RET(__wt_struct_size(
	    session, &size, fmt, 0, optype, op->uri, &op->key, &value));

	/* We assumed we were packing zero into the size above, fix that. */
	size += __wt_vsize_posint(size) - 1;
	WT_RET(__wt_buf_grow(session, logrec, logrec->size + size));
	recsize = (uint32_t)size;
	WT_RET(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, size, fmt,
	    recsize, optype, op->uri, &op->key, &value));
	logrec->size += size;
	return (0);
}

static int
__txn_op_printlog(WT_SESSION_IMPL *session, WT_ITEM *logrec, FILE *out)
{
	WT_UNUSED(session);

	if (fprintf(out, "%.*s%s\n",
	    (int)WT_MIN(logrec->size, 40), (const char *)logrec->data,
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

static int
__txn_op_apply(WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM key, value;
	const char *fmt = "IISuu", *uri;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_open_cursor),
	    "raw,overwrite", NULL };
	uint32_t optype, recsize;

	/* Peek at the size. */
	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), "I",
	    &recsize));
	WT_RET(__wt_struct_unpack(session, *pp, recsize, fmt,
	    &recsize, &optype, &uri, &key, &value));
	*pp += recsize;

	WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
	cursor->set_key(cursor, &key);
	cursor->set_value(cursor, &value);
	WT_TRET(cursor->insert(cursor));
	WT_TRET(cursor->close(cursor));
	return (ret);
}

static int
__txn_commit_apply(WT_SESSION_IMPL *session,
    WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end)
{
	WT_UNUSED(lsnp);

	/* The logging subsystem zero-pads records. */
	while (*pp < end && **pp)
		WT_RET(__txn_op_apply(session, pp, end));

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
	uint8_t *p;
	size_t header_size;
	uint32_t rectype = TXN_LOG_COMMIT;
	u_int i;

	WT_UNUSED(cfg);
	txn = &session->txn;

	header_size = __wt_vsize_uint(rectype);
	WT_RET(__wt_scr_alloc(
	    session, sizeof(WT_LOG_RECORD) + header_size, &logrec));
	WT_CLEAR(*(WT_LOG_RECORD *)logrec->data);

	p = (uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	WT_RET(__wt_vpack_uint(&p, header_size, rectype));
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
	const uint8_t *end, *p;
	uint64_t rectype;

	out = cookie;

	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* Every log record must start with the type. */
	WT_RET(__wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &rectype));

	if (fprintf(out, "[%" PRIu32 "/%" PRId64 "] type %d\n",
	    lsnp->file, lsnp->offset, (int)rectype))
		return (errno);

	switch (rectype) {
	case TXN_LOG_COMMIT:
		WT_RET(__txn_commit_printlog(session, logrec, out));
		break;
	}

	return (0);
}

/*
 * __wt_txn_recover --
 *	Roll the log forward to recover committed changes.
 */
int
__wt_txn_recover(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	const uint8_t *end, *p;
	uint64_t rectype;

	WT_UNUSED(cookie);

	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* Every log record must start with the type. */
	WT_RET(__wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &rectype));

	switch (rectype) {
	case TXN_LOG_COMMIT:
		WT_RET(__txn_commit_apply(session, lsnp, &p, end));
		break;
	}

	return (0);
}
