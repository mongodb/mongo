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
	WT_ITEM value;
	const char *fmt = "IISuu";
	size_t size;
	uint64_t recno;
	uint32_t optype, recsize;

	value.data = WT_UPDATE_DATA(op->upd);
	value.size = op->upd->size;

	/*
	 * Cases:
	 * 1) column store remove;
	 * 2) column store insert/update;
	 * 3) row store remove;
	 * 4) row store insert/update;
	 *
	 * We first calculate the size being packed (assuming the size itself
	 * is zero), then fix the calculation once we know the size.
	 */
#define	WT_TXNOP_COL_INSERT	1
#define	WT_TXNOP_COL_REMOVE	2
#define	WT_TXNOP_ROW_INSERT	3
#define	WT_TXNOP_ROW_REMOVE	4
	if (op->key.data == NULL) {
		WT_ASSERT(session, op->ins != NULL);
		recno = op->ins->u.recno;

		if (WT_UPDATE_DELETED_ISSET(op->upd)) {
			fmt = "IISq";
			optype = WT_TXNOP_COL_REMOVE;

			WT_RET(__wt_struct_size(session, &size, fmt,
			    0, optype, op->uri, recno));

			size += __wt_vsize_uint(size) - 1;
			WT_RET(__wt_buf_grow(
			    session, logrec, logrec->size + size));
			recsize = (uint32_t)size;
			WT_RET(__wt_struct_pack(session,
			    (uint8_t *)logrec->data + logrec->size,
			    size, fmt,
			    recsize, optype, op->uri, recno));
		} else {
			fmt = "IISqu";
			optype = WT_TXNOP_COL_INSERT;

			WT_RET(__wt_struct_size(session, &size, fmt,
			    0, optype, op->uri, recno, &value));

			size += __wt_vsize_uint(size) - 1;
			WT_RET(__wt_buf_grow(
			    session, logrec, logrec->size + size));
			recsize = (uint32_t)size;
			WT_RET(__wt_struct_pack(session,
			    (uint8_t *)logrec->data + logrec->size,
			    size, fmt,
			    recsize, optype, op->uri, recno, &value));
		}
	} else {
		if (WT_UPDATE_DELETED_ISSET(op->upd)) {
			fmt = "IISu";
			optype = WT_TXNOP_ROW_REMOVE;

			WT_RET(__wt_struct_size(session, &size, fmt,
			    0, optype, op->uri, &op->key));

			size += __wt_vsize_uint(size) - 1;
			WT_RET(__wt_buf_grow(
			    session, logrec, logrec->size + size));
			recsize = (uint32_t)size;
			WT_RET(__wt_struct_pack(session,
			    (uint8_t *)logrec->data + logrec->size,
			    size, fmt,
			    recsize, optype, op->uri, &op->key));
		} else {
			fmt = "IISuu";
			optype = WT_TXNOP_ROW_INSERT;

			WT_RET(__wt_struct_size(session, &size, fmt,
			    0, optype, op->uri, &op->key, &value));

			size += __wt_vsize_uint(size) - 1;
			WT_RET(__wt_buf_grow(
			    session, logrec, logrec->size + size));
			recsize = (uint32_t)size;
			WT_RET(__wt_struct_pack(session,
			    (uint8_t *)logrec->data + logrec->size,
			    size, fmt,
			    recsize, optype, op->uri, &op->key, &value));
		}
	}

	logrec->size += (uint32_t)size;
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
	const char *fmt, *uri;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_open_cursor),
	    "overwrite", NULL };
	uint64_t recno;
	uint32_t optype, recsize;

	/* Peek at the size and the type. */
	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), "II",
	    &recsize, &optype));

	switch (optype) {
	case WT_TXNOP_COL_INSERT:
		fmt = "IISqu";
		WT_RET(__wt_struct_unpack(session, *pp, recsize, fmt,
		    &recsize, &optype, &uri, &recno, &value));
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		cursor->set_key(cursor, recno);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_TXNOP_COL_REMOVE:
		fmt = "IISq";
		WT_RET(__wt_struct_unpack(session, *pp, recsize, fmt,
		    &recsize, &optype, &uri, &recno));
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		cursor->set_key(cursor, recno);
		WT_TRET(cursor->remove(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_TXNOP_ROW_INSERT:
		fmt = "IISuu";
		WT_RET(__wt_struct_unpack(session, *pp, recsize, fmt,
		    &recsize, &optype, &uri, &key, &value));
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		__wt_cursor_set_raw_key(cursor, &key);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_TXNOP_ROW_REMOVE:
		fmt = "IISu";
		WT_RET(__wt_struct_unpack(session, *pp, recsize, fmt,
		    &recsize, &optype, &uri, &key));
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		__wt_cursor_set_raw_key(cursor, &key);
		WT_TRET(cursor->remove(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	*pp += recsize;
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
	logrec->size =
	    (uint32_t)(offsetof(WT_LOG_RECORD, record) + header_size);

	/* Write updates to the log. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		if (op->upd != NULL)
			WT_ERR(__txn_op_log(session, logrec, op));
		else if (op->ref != NULL)
			/* We can't handle physical truncate yet. */
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
