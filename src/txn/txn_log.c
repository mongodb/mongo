/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__txn_op_log(WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_TXN_OP *op)
{
	WT_ITEM value;
	uint64_t recno;

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
	if (op->key.data == NULL) {
		WT_ASSERT(session, op->ins != NULL);
		recno = op->ins->u.recno;

		if (WT_UPDATE_DELETED_ISSET(op->upd))
			WT_RET(__wt_logop_col_remove_pack(
			    session, logrec, op->fileid, recno));
		else
			WT_RET(__wt_logop_col_put_pack(
			    session, logrec, op->fileid, recno, &value));
	} else {
		if (WT_UPDATE_DELETED_ISSET(op->upd))
			WT_RET(__wt_logop_row_remove_pack(
			    session, logrec, op->fileid, &op->key));
		else
			WT_RET(__wt_logop_row_put_pack(
			    session, logrec, op->fileid, &op->key, &value));
	}

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

/*
 * __wt_txn_log_commit --
 *	Write the operations of a transaction to the log at commit time.
 */
int
__wt_txn_log_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_ITEM *logrec;
	WT_TXN *txn;
	WT_TXN_OP *op;
	const char *fmt = "I";
	size_t header_size;
	uint32_t rectype = WT_LOGREC_COMMIT;
	u_int i;

	WT_UNUSED(cfg);
	txn = &session->txn;

	WT_RET(__wt_struct_size(session, &header_size, fmt, rectype));
	WT_RET(__wt_logrec_alloc(session, header_size, &logrec));

	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, header_size, fmt, rectype));
	logrec->size += (uint32_t)header_size;

	/* Write updates to the log. */
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++) {
		if (op->upd != NULL)
			WT_ERR(__txn_op_log(session, logrec, op));
		/* XXX We can't handle physical truncate yet. */
	}

	WT_ERR(__wt_log_write(session,
	    logrec, NULL, S2C(session)->txn_logsync));

err:	__wt_logrec_free(session, &logrec);
	return (ret);
}

/*
 * __wt_txn_log_checkpoint --
 *	Write a log record for a checkpoint operation.
 */
int
__wt_txn_log_checkpoint(WT_SESSION_IMPL *session, int start, WT_LSN *lsnp)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_ITEM *logrec;
	const char *fmt = WT_UNCHECKED_STRING(ISI);
	size_t header_size;
	uint32_t rectype = WT_LOGREC_CHECKPOINT;

	dhandle = session->dhandle;

	WT_RET(__wt_struct_size(
	    session, &header_size, fmt, rectype, dhandle->name, start));
	WT_RET(__wt_logrec_alloc(session, header_size, &logrec));

	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, header_size,
	    fmt, rectype, dhandle->name, start));
	logrec->size += (uint32_t)header_size;

	WT_ERR(__wt_log_write(session, logrec, lsnp, 0));
err:	__wt_logrec_free(session, &logrec);
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
	case WT_LOGREC_COMMIT:
		WT_RET(__txn_commit_printlog(session, logrec, out));
		break;
	}

	return (0);
}

