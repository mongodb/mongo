/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __txn_op_log --
 *	Log an operation for the current transaction.
 */
static int
__txn_op_log(WT_SESSION_IMPL *session,
    WT_ITEM *logrec, WT_TXN_OP *op, WT_CURSOR_BTREE *cbt)
{
	WT_DECL_RET;
	WT_ITEM key, value;
	WT_UPDATE *upd;
	uint64_t recno;

	WT_CLEAR(key);
	upd = op->u.upd;
	value.data = WT_UPDATE_DATA(upd);
	value.size = upd->size;

	/*
	 * Log the operation.  It must be one of the following:
	 * 1) column store remove;
	 * 2) column store insert/update;
	 * 3) row store remove; or
	 * 4) row store insert/update.
	 */
	if (cbt->btree->type != BTREE_ROW) {
		WT_ASSERT(session, cbt->ins != NULL);
		recno = WT_INSERT_RECNO(cbt->ins);
		WT_ASSERT(session, recno != 0);

		if (WT_UPDATE_DELETED_ISSET(upd))
			WT_ERR(__wt_logop_col_remove_pack(session, logrec,
			    op->fileid, recno));
		else
			WT_ERR(__wt_logop_col_put_pack(session, logrec,
			    op->fileid, recno, &value));
	} else {
		WT_ERR(__wt_cursor_row_leaf_key(cbt, &key));

		if (WT_UPDATE_DELETED_ISSET(upd))
			WT_ERR(__wt_logop_row_remove_pack(session, logrec,
			    op->fileid, &key));
		else
			WT_ERR(__wt_logop_row_put_pack(session, logrec,
			    op->fileid, &key, &value));
	}

err:	__wt_buf_free(session, &key);
	return (ret);
}

/*
 * __txn_commit_printlog --
 *	Print a commit log record.
 */
static int
__txn_commit_printlog(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end, FILE *out)
{
	int firstrecord;

	firstrecord = 1;

	/* The logging subsystem zero-pads records. */
	while (*pp < end && **pp) {
		if (!firstrecord)
			fprintf(out, ",\n");

		firstrecord = 0;

		WT_RET(__wt_txn_op_printlog(session, pp, end, out));
	}

	fprintf(out, "\n");

	return (0);
}

/*
 * __wt_txn_op_free --
 *	Free memory associated with a transactional operation.
 */
void
__wt_txn_op_free(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
	switch (op->type) {
	case TXN_OP_BASIC:
	case TXN_OP_INMEM:
	case TXN_OP_REF:
	case TXN_OP_TRUNCATE_COL:
		break;

	case TXN_OP_TRUNCATE_ROW:
		__wt_buf_free(session, &op->u.truncate_row.start);
		__wt_buf_free(session, &op->u.truncate_row.stop);
		break;
	}
}

/*
 * __txn_logrec_init --
 *	Allocate and initialize a buffer for a transaction's log records.
 */
static int
__txn_logrec_init(WT_SESSION_IMPL *session)
{
	WT_DECL_ITEM(logrec);
	WT_DECL_RET;
	WT_TXN *txn;
	const char *fmt = WT_UNCHECKED_STRING(Iq);
	uint32_t rectype = WT_LOGREC_COMMIT;
	size_t header_size;

	txn = &session->txn;
	if (txn->logrec != NULL)
		return (0);

	WT_ASSERT(session, txn->id != WT_TXN_NONE);
	WT_RET(__wt_struct_size(session, &header_size, fmt, rectype, txn->id));
	WT_RET(__wt_logrec_alloc(session, header_size, &logrec));

	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, header_size,
	    fmt, rectype, txn->id));
	logrec->size += (uint32_t)header_size;
	txn->logrec = logrec;

	if (0) {
err:		__wt_logrec_free(session, &logrec);
	}
	return (ret);
}

/*
 * __wt_txn_log_op --
 *	Write the last logged operation into the in-memory buffer.
 */
int
__wt_txn_log_op(WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt)
{
	WT_ITEM *logrec;
	WT_TXN *txn;
	WT_TXN_OP *op;

	if (!FLD_ISSET(S2C(session)->log_flags, WT_CONN_LOG_ENABLED) ||
	    F_ISSET(session, WT_SESSION_NO_LOGGING))
		return (0);

	txn = &session->txn;

	/* We'd better have a transaction. */
	WT_ASSERT(session,
	    F_ISSET(txn, TXN_RUNNING) && F_ISSET(txn, TXN_HAS_ID));

	WT_ASSERT(session, txn->mod_count > 0);
	op = txn->mod + txn->mod_count - 1;

	WT_RET(__txn_logrec_init(session));
	logrec = txn->logrec;

	switch (op->type) {
	case TXN_OP_BASIC:
		return (__txn_op_log(session, logrec, op, cbt));
	case TXN_OP_INMEM:
	case TXN_OP_REF:
		/* Nothing to log, we're done. */
		return (0);
	case TXN_OP_TRUNCATE_COL:
		return (__wt_logop_col_truncate_pack(session, logrec,
		    op->fileid,
		    op->u.truncate_col.start, op->u.truncate_col.stop));
	case TXN_OP_TRUNCATE_ROW:
		return (__wt_logop_row_truncate_pack(session, txn->logrec,
		    op->fileid,
		    &op->u.truncate_row.start, &op->u.truncate_row.stop,
		    (uint32_t)op->u.truncate_row.mode));
	WT_ILLEGAL_VALUE(session);
	}

	/* NOTREACHED */
}

/*
 * __wt_txn_log_commit --
 *	Write the operations of a transaction to the log at commit time.
 */
int
__wt_txn_log_commit(WT_SESSION_IMPL *session, const char *cfg[])
{
	WT_TXN *txn;

	WT_UNUSED(cfg);
	txn = &session->txn;

	/* Write updates to the log. */
	return (__wt_log_write(session, txn->logrec, NULL, txn->txn_logsync));
}

/*
 * __txn_log_file_sync --
 *	Write a log record for a file sync.
 */
static int
__txn_log_file_sync(WT_SESSION_IMPL *session, uint32_t flags, WT_LSN *lsnp)
{
	WT_BTREE *btree;
	WT_DECL_ITEM(logrec);
	WT_DECL_RET;
	size_t header_size;
	uint32_t rectype = WT_LOGREC_FILE_SYNC;
	int start;
	const char *fmt = WT_UNCHECKED_STRING(III);

	btree = S2BT(session);
	start = LF_ISSET(WT_TXN_LOG_CKPT_START);

	WT_RET(__wt_struct_size(
	    session, &header_size, fmt, rectype, btree->id, start));
	WT_RET(__wt_logrec_alloc(session, header_size, &logrec));

	WT_ERR(__wt_struct_pack(session,
	    (uint8_t *)logrec->data + logrec->size, header_size,
	    fmt, rectype, btree->id, start));
	logrec->size += (uint32_t)header_size;

	WT_ERR(__wt_log_write(session, logrec, lsnp, 0));
err:	__wt_logrec_free(session, &logrec);
	return (ret);
}

/*
 * __wt_txn_checkpoint_logread --
 *	Read a log record for a checkpoint operation.
 */
int
__wt_txn_checkpoint_logread(
    WT_SESSION_IMPL *session, const uint8_t **pp, const uint8_t *end,
    WT_LSN *ckpt_lsn)
{
	WT_ITEM ckpt_snapshot;
	u_int ckpt_nsnapshot;
	const char *fmt = WT_UNCHECKED_STRING(IQIU);

	WT_RET(__wt_struct_unpack(session, *pp, WT_PTRDIFF(end, *pp), fmt,
	    &ckpt_lsn->file, &ckpt_lsn->offset,
	    &ckpt_nsnapshot, &ckpt_snapshot));
	WT_UNUSED(ckpt_nsnapshot);
	WT_UNUSED(ckpt_snapshot);
	*pp = end;
	return (0);
}

/*
 * __wt_txn_checkpoint_log --
 *	Write a log record for a checkpoint operation.
 */
int
__wt_txn_checkpoint_log(
    WT_SESSION_IMPL *session, int full, uint32_t flags, WT_LSN *lsnp)
{
	WT_DECL_ITEM(logrec);
	WT_DECL_RET;
	WT_LSN *ckpt_lsn;
	WT_TXN *txn;
	uint8_t *end, *p;
	size_t recsize;
	uint32_t i, rectype = WT_LOGREC_CHECKPOINT;
	const char *fmt = WT_UNCHECKED_STRING(IIQIU);

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
		 * During a clean connection close, we get here without the
		 * prepare or start steps.  In that case, log the current LSN
		 * as the checkpoint LSN.
		 */
		if (!txn->full_ckpt) {
			txn->ckpt_nsnapshot = 0;
			*ckpt_lsn = S2C(session)->log->alloc_lsn;
		}

		/* Write the checkpoint log record. */
		WT_ERR(__wt_struct_size(session, &recsize, fmt,
		    rectype, ckpt_lsn->file, ckpt_lsn->offset,
		    txn->ckpt_nsnapshot, &txn->ckpt_snapshot));
		WT_ERR(__wt_logrec_alloc(session, recsize, &logrec));

		WT_ERR(__wt_struct_pack(session,
		    (uint8_t *)logrec->data + logrec->size, recsize, fmt,
		    rectype, ckpt_lsn->file, ckpt_lsn->offset,
		    txn->ckpt_nsnapshot, &txn->ckpt_snapshot));
		logrec->size += (uint32_t)recsize;
		WT_ERR(__wt_log_write(session, logrec, lsnp, 0));

		/*
		 * If this full checkpoint completed successfully and there is
		 * no hot backup in progress, tell the logging subsystem the
		 * checkpoint LSN so that it can archive.
		 */
		if (!S2C(session)->hot_backup)
			WT_ERR(__wt_log_ckpt(session, ckpt_lsn));

		/* FALLTHROUGH */
	case WT_TXN_LOG_CKPT_FAIL:
		/* Cleanup any allocated resources */
		WT_INIT_LSN(ckpt_lsn);
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

	if (btree->type == BTREE_ROW) {
		op->type = TXN_OP_TRUNCATE_ROW;
		op->u.truncate_row.mode = TXN_TRUNC_ALL;
		WT_CLEAR(op->u.truncate_row.start);
		WT_CLEAR(op->u.truncate_row.stop);
		if (start != NULL) {
			op->u.truncate_row.mode = TXN_TRUNC_START;
			item = &op->u.truncate_row.start;
			WT_RET(__wt_cursor_get_raw_key(&start->iface, item));
			WT_RET(__wt_buf_set(
			    session, item, item->data, item->size));
		}
		if (stop != NULL) {
			op->u.truncate_row.mode =
			    (op->u.truncate_row.mode == TXN_TRUNC_ALL) ?
			    TXN_TRUNC_STOP : TXN_TRUNC_BOTH;
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

	/* Write that operation into the in-memory log. */
	WT_RET(__wt_txn_log_op(session, NULL));

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
__txn_printlog(WT_SESSION_IMPL *session,
    WT_ITEM *rawrec, WT_LSN *lsnp, void *cookie, int firstrecord)
{
	FILE *out;
	WT_LOG_RECORD *logrec;
	WT_LSN ckpt_lsn;
	int compressed;
	uint64_t txnid;
	uint32_t fileid, rectype;
	int32_t start;
	const uint8_t *end, *p;
	const char *msg;

	out = cookie;

	p = LOG_SKIP_HEADER(rawrec->data);
	end = (const uint8_t *)rawrec->data + rawrec->size;
	logrec = (WT_LOG_RECORD *)rawrec->data;
	compressed = F_ISSET(logrec, WT_LOG_RECORD_COMPRESSED);

	/* First, peek at the log record type. */
	WT_RET(__wt_logrec_read(session, &p, end, &rectype));

	if (!firstrecord)
		fprintf(out, ",\n");

	if (fprintf(out, "  { \"lsn\" : [%" PRIu32 ",%" PRId64 "],\n",
	    lsnp->file, lsnp->offset) < 0 ||
	    fprintf(out, "    \"hdr_flags\" : \"%s\",\n",
	    compressed ? "compressed" : "") < 0 ||
	    fprintf(out, "    \"rec_len\" : %" PRIu32 ",\n", logrec->len) < 0 ||
	    fprintf(out, "    \"mem_len\" : %" PRIu32 ",\n",
	    compressed ? logrec->mem_len : logrec->len) < 0)
		return (errno);

	switch (rectype) {
	case WT_LOGREC_CHECKPOINT:
		WT_RET(__wt_struct_unpack(session, p, WT_PTRDIFF(end, p),
		    WT_UNCHECKED_STRING(IQ), &ckpt_lsn.file, &ckpt_lsn.offset));
		if (fprintf(out, "    \"type\" : \"checkpoint\",\n") < 0 ||
		    fprintf(
		    out, "    \"ckpt_lsn\" : [%" PRIu32 ",%" PRId64 "]\n",
		    ckpt_lsn.file, ckpt_lsn.offset) < 0)
			return (errno);
		break;

	case WT_LOGREC_COMMIT:
		WT_RET(__wt_vunpack_uint(&p, WT_PTRDIFF(end, p), &txnid));
		if (fprintf(out, "    \"type\" : \"commit\",\n") < 0 ||
		    fprintf(out, "    \"txnid\" : %" PRIu64 ",\n", txnid) < 0)
			return (errno);
		WT_RET(__txn_commit_printlog(session, &p, end, out));
		break;

	case WT_LOGREC_FILE_SYNC:
		WT_RET(__wt_struct_unpack(session, p, WT_PTRDIFF(end, p),
		    WT_UNCHECKED_STRING(Ii), &fileid, &start));
		if (fprintf(out, "    \"type\" : \"file_sync\",\n") < 0 ||
		    fprintf(out, "    \"fileid\" : %" PRIu32 ",\n",
		    fileid) < 0 ||
		    fprintf(out, "    \"start\" : %" PRId32 "\n", start) < 0)
			return (errno);
		break;

	case WT_LOGREC_MESSAGE:
		WT_RET(__wt_struct_unpack(session, p, WT_PTRDIFF(end, p),
		    WT_UNCHECKED_STRING(S), &msg));
		if (fprintf(out, "    \"type\" : \"message\",\n") < 0 ||
		    fprintf(out, "    \"message\" : \"%s\"\n", msg) < 0)
			return (errno);
		break;
	}

	if (fprintf(out, "  }") < 0)
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
	if (fprintf(out, "\n]\n") < 0)
		return (errno);

	return (0);
}
