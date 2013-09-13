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

	value.data = WT_UPDATE_DATA(op->u.op.upd);
	value.size = op->u.op.upd->size;

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
	if (op->u.op.key.data == NULL) {
		WT_ASSERT(session, op->u.op.ins != NULL);
		recno = op->u.op.ins->u.recno;

		if (WT_UPDATE_DELETED_ISSET(op->u.op.upd))
			WT_RET(__wt_logop_col_remove_pack(session, logrec,
			    op->fileid, recno));
		else
			WT_RET(__wt_logop_col_put_pack(session, logrec,
			    op->fileid, recno, &value));
	} else {
		if (WT_UPDATE_DELETED_ISSET(op->u.op.upd))
			WT_RET(__wt_logop_row_remove_pack(session, logrec,
			    op->fileid, &op->u.op.key));
		else
			WT_RET(__wt_logop_row_put_pack(session, logrec,
			    op->fileid, &op->u.op.key, &value));
	}

	return (0);
}

static int
__txn_commit_printlog(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	/* The logging subsystem zero-pads records. */
	while (*pp < end && **pp)
		WT_RET(__wt_txn_op_printlog(session, pp, end, out));
	return (0);
}

/*
 * __wt_txn_op_free --
 *	Free memory associated with a transactional operation.
 */
void
__wt_txn_op_free(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
	if (op->type == TXN_OP_TRUNCATE_ROW) {
		__wt_buf_free(session, &op->u.truncate_row.start);
		__wt_buf_free(session, &op->u.truncate_row.stop);
	}
}

/*
 * __wt_txn_log_commit --
 *	Write the operations of a transaction to the log at commit time.
 */
int
__wt_txn_log_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_DECL_RET;
	WT_DECL_ITEM(logrec);
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
	for (i = 0, op = txn->mod; i < txn->mod_count; i++, op++)
		switch (op->type) {
		case TXN_OP_BASIC:
			WT_ERR(__txn_op_log(session, logrec, op));
			break;
		case TXN_OP_INMEM:
		case TXN_OP_REF:
			/* Nothing to log, we're done. */
			break;
		case TXN_OP_TRUNCATE_COL:
			WT_ERR(__wt_logop_col_truncate_pack(session, logrec,
			    op->fileid,
			    op->u.truncate_col.start, op->u.truncate_col.stop));
			break;
		case TXN_OP_TRUNCATE_ROW:
			WT_ERR(__wt_logop_row_truncate_pack(session, logrec,
			    op->fileid,
			    &op->u.truncate_row.start, &op->u.truncate_row.stop,
			    op->u.truncate_row.mode));
			break;
		}

	WT_ERR(__wt_log_write(session,
	    logrec, NULL, S2C(session)->txn_logsync));

err:	__wt_logrec_free(session, &logrec);
	return (ret);
}

/*
 * __txn_log_file_sync --
 *	Write a log record for a file sync.
 */
static int
__txn_log_file_sync(WT_SESSION_IMPL *session, uint32_t flags, WT_LSN *lsnp)
{
	WT_DATA_HANDLE *dhandle;
	WT_DECL_RET;
	WT_DECL_ITEM(logrec);
	const char *fmt = WT_UNCHECKED_STRING(ISI);
	size_t header_size;
	uint32_t rectype = WT_LOGREC_FILE_SYNC;
	int start;

	dhandle = session->dhandle;
	start = LF_ISSET(WT_TXN_LOG_CKPT_START);

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
 * __wt_txn_log_checkpoint --
 *	Write a log record for a checkpoint operation.
 */
int
__wt_txn_log_checkpoint(
    WT_SESSION_IMPL *session, int full, uint32_t flags, WT_LSN *lsnp)
{
	WT_DECL_RET;
	WT_DECL_ITEM(logrec);
	WT_LSN *ckpt_lsn;
	WT_TXN *txn;
	const char *fmt = WT_UNCHECKED_STRING(IIQIu);
	uint8_t *end, *p;
	size_t recsize;
	uint32_t i, rectype = WT_LOGREC_CHECKPOINT;

	txn = &session->txn;
	ckpt_lsn = &txn->ckpt_lsn;

	/*
	 * If this is a file sync, log it unless there is a full checkpoint in
	 * progress.
	 */
	if (!full) {
		if (txn->full_ckpt) {
			if (lsnp != NULL)
				*lsnp = *ckpt_lsn;
			return (0);
		} else
			return (__txn_log_file_sync(session, flags, lsnp));
	}

	switch (flags) {
	case WT_TXN_LOG_CKPT_PREPARE:
		txn->full_ckpt = 1;
		*ckpt_lsn = S2C(session)->log->alloc_lsn;
		break;

	case WT_TXN_LOG_CKPT_START:
		/* Take a copy of the transaction snapshot. */
		txn->ckpt_nsnapshot = txn->snapshot_count;
		recsize = txn->ckpt_nsnapshot * WT_INTPACK64_MAXSIZE;
		WT_ERR(__wt_scr_alloc(session, recsize, &txn->ckpt_snapshot));
		p = txn->ckpt_snapshot->mem;
		end = p + recsize;
		for (i = 0; i < txn->snapshot_count; i++)
			WT_ERR(__wt_vpack_uint(
			    &p, WT_PTRDIFF(end, p), txn->snapshot[i]));
		break;

	case WT_TXN_LOG_CKPT_STOP:
		/*
		 * Write the checkpoint log record and tell the logging
		 * subsystem the checkpoint LSN.
		 */
		WT_ERR(__wt_struct_size(session, &recsize, fmt,
		    rectype, ckpt_lsn->file, ckpt_lsn->offset,
		    txn->ckpt_nsnapshot, txn->ckpt_snapshot));
		WT_ERR(__wt_logrec_alloc(session, recsize, &logrec));

		WT_ERR(__wt_struct_pack(session,
		    (uint8_t *)logrec->data + logrec->size, recsize, fmt,
		    rectype, ckpt_lsn->file, ckpt_lsn->offset,
		    txn->ckpt_nsnapshot, txn->ckpt_snapshot));
		logrec->size += (uint32_t)recsize;
		WT_ERR(__wt_log_write(session, logrec, lsnp, 0));

#if 0
		WT_ERR(__wt_log_ckpt(session, ckpt_lsn));
#endif

		/* FALLTHROUGH */
	case WT_TXN_LOG_CKPT_FAIL:
		/* Cleanup any allocated resources */
		INIT_LSN(ckpt_lsn);
		txn->ckpt_nsnapshot = 0;
		__wt_scr_free(&txn->ckpt_snapshot);
		txn->full_ckpt = 0;
		break;
	}

err:	__wt_logrec_free(session, &logrec);
	return (ret);
}

/*
 * __wt_txn_truncate_log --
 *	Begin truncating a range of a file.
 */
int
__wt_txn_truncate_log(
    WT_SESSION_IMPL *session, WT_CURSOR_BTREE *start, WT_CURSOR_BTREE *stop)
{
	WT_BTREE *btree;
	WT_ITEM *item;
	WT_TXN_OP *op;

	btree = S2BT(session);

	WT_RET(__txn_next_op(session, &op));
	op->fileid = btree->id;

	if (btree->type == BTREE_ROW) {
		op->type = TXN_OP_TRUNCATE_ROW;
		op->u.truncate_row.mode = 0;
		WT_CLEAR(op->u.truncate_row.start);
		WT_CLEAR(op->u.truncate_row.stop);
		if (start != NULL) {
			op->u.truncate_row.mode |= 0x2;
			item = &op->u.truncate_row.start;
			WT_RET(__wt_cursor_get_raw_key(&start->iface, item));
			WT_RET(__wt_buf_set(
			    session, item, item->data, item->size));
		}
		if (stop != NULL) {
			op->u.truncate_row.mode |= 0x1;
			item = &op->u.truncate_row.stop;
			WT_RET(__wt_cursor_get_raw_key(&stop->iface, item));
			WT_RET(__wt_buf_set(
			    session, item, item->data, item->size));
		}
	} else {
		op->type = TXN_OP_TRUNCATE_COL;
		op->u.truncate_col.start =
		    (start == NULL) ? 0 : start->recno;
		op->u.truncate_col.stop =
		    (stop == NULL) ? 0 : stop->recno;
	}

	WT_ASSERT(session, !F_ISSET(session, WT_SESSION_LOGGING_INMEM));
	F_SET(session, WT_SESSION_LOGGING_INMEM);
	return (0);
}

/*
 * __wt_txn_truncate_end --
 *	Finish truncating a range of a file.
 */
int
__wt_txn_truncate_end(WT_SESSION_IMPL *session)
{
	F_CLR(session, WT_SESSION_LOGGING_INMEM);
	return (0);
}

/*
 * __txn_printlog --
 *	Print a log record in a human-readable format.
 */
static int
__txn_printlog(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	FILE *out;
	const uint8_t *end, *p;
	uint32_t rectype;

	out = cookie;

	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* First, peek at the log record type. */
	WT_RET(__wt_logrec_read(session, &p, end, &rectype));

	if (fprintf(out, "  { \"lsn\" : [%" PRIu32 ",%" PRId64 "],\n",
	    lsnp->file, lsnp->offset) < 0 ||
	    fprintf(out, "    \"type\" : %d,\n", (int)rectype) < 0)
		return (errno);

	switch (rectype) {
	case WT_LOGREC_COMMIT:
		WT_RET(__txn_commit_printlog(session, &p, end, out));
		break;
	}

	if (fprintf(out, "  },\n") < 0)
		return (errno);

	return (0);
}

/*
 * __wt_txn_printlog --
 *	Print the log in a human-readable format.
 */
int
__wt_txn_printlog(WT_SESSION *wt_session, FILE *out)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)wt_session;

	if (fprintf(out, "[\n") < 0)
		return (errno);
	WT_RET(__wt_log_scan(
	    session, NULL, WT_LOGSCAN_FIRST, __txn_printlog, out));
	if (fprintf(out, "]\n") < 0)
		return (errno);

	return (0);
}

