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
			    session, logrec, op->uri, recno));
		else
			WT_RET(__wt_logop_col_put_pack(
			    session, logrec, op->uri, recno, &value));
	} else {
		if (WT_UPDATE_DELETED_ISSET(op->upd))
			WT_RET(__wt_logop_row_remove_pack(
			    session, logrec, op->uri, &op->key));
		else
			WT_RET(__wt_logop_row_put_pack(
			    session, logrec, op->uri, &op->key, &value));
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

static int
__txn_op_apply(WT_SESSION_IMPL *session,
    const uint8_t **pp, const uint8_t *end, int metadata)
{
	WT_CURSOR *cursor;
	WT_DECL_RET;
	WT_ITEM key, value;
	const char *uri;
	const char *cfg[] = { WT_CONFIG_BASE(session, session_open_cursor),
	    "overwrite", NULL };
	uint64_t recno;
	uint32_t optype, opsize;
	int is_metadata;

	/* Peek at the size and the type. */
	WT_RET(__wt_logop_read(session, pp, end, &optype, &opsize));
	end = *pp + opsize;

	switch (optype) {
	case WT_LOGOP_COL_PUT:
		WT_RET(__wt_logop_col_put_unpack(session, pp, end,
		    &uri, &recno, &value));
		is_metadata = (strcmp(uri, WT_METADATA_URI) == 0);
		if (metadata != is_metadata)
			break;
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		cursor->set_key(cursor, recno);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_LOGOP_COL_REMOVE:
		WT_RET(__wt_logop_col_remove_unpack(session, pp, end,
		    &uri, &recno));
		is_metadata = (strcmp(uri, WT_METADATA_URI) == 0);
		if (metadata != is_metadata)
			break;
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		cursor->set_key(cursor, recno);
		WT_TRET(cursor->remove(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_LOGOP_ROW_PUT:
		WT_RET(__wt_logop_row_put_unpack(session, pp, end,
		    &uri, &key, &value));
		is_metadata = (strcmp(uri, WT_METADATA_URI) == 0);
		if (metadata != is_metadata)
			break;
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		__wt_cursor_set_raw_key(cursor, &key);
		__wt_cursor_set_raw_value(cursor, &value);
		WT_TRET(cursor->insert(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	case WT_LOGOP_ROW_REMOVE:
		WT_RET(__wt_logop_row_remove_unpack(session, pp, end,
		    &uri, &key));
		is_metadata = (strcmp(uri, WT_METADATA_URI) == 0);
		if (metadata != is_metadata)
			break;
		WT_RET(__wt_open_cursor(session, uri, NULL, cfg, &cursor));
		__wt_cursor_set_raw_key(cursor, &key);
		WT_TRET(cursor->remove(cursor));
		WT_TRET(cursor->close(cursor));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	WT_ASSERT(session, ret == 0);
	return (ret);
}

static int
__txn_commit_apply(WT_SESSION_IMPL *session,
    WT_LSN *lsnp, const uint8_t **pp, const uint8_t *end, int metadata)
{
	WT_UNUSED(lsnp);

	/* The logging subsystem zero-pads records. */
	while (*pp < end && **pp)
		WT_RET(__txn_op_apply(session, pp, end, metadata));

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
	uint32_t rectype = WT_LOGREC_COMMIT;
	u_int i;

	WT_UNUSED(cfg);
	txn = &session->txn;

	header_size = __wt_vsize_uint(rectype);
	WT_RET(__wt_logrec_alloc(session, &logrec));

	p = (uint8_t *)logrec->data + logrec->size;
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

/*
 * __txn_log_recover --
 *	Roll the log forward to recover committed changes.
 */
static int
__txn_log_recover(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	const uint8_t *end, *p;
	uint32_t rectype;
	int metadata;

	metadata = *(int *)cookie;
	p = (const uint8_t *)logrec->data + offsetof(WT_LOG_RECORD, record);
	end = (const uint8_t *)logrec->data + logrec->size;

	/* First, peek at the log record type. */
	WT_RET(__wt_logrec_read(session, &p, end, &rectype));

	switch (rectype) {
	case WT_LOGREC_COMMIT:
		WT_RET(__txn_commit_apply(session, lsnp, &p, end, metadata));
		break;
	}

	return (0);
}

/*
 * __wt_txn_recover --
 *	Run recovery.
 */
int
__wt_txn_recover(WT_SESSION_IMPL *default_session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_SESSION_IMPL *rec_session;
	int metadata;

	conn = S2C(default_session);
	/* We need a real session for recovery. */
	rec_session = NULL;
	WT_ERR(__wt_open_session(conn, 0, NULL, NULL, &rec_session));
	F_SET(rec_session, WT_SESSION_NO_LOGGING);

	metadata = 1;
	WT_ERR(__wt_log_scan(rec_session,
	    NULL, WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER,
	    __txn_log_recover, &metadata));

	metadata = 0;
	WT_ERR(__wt_log_scan(rec_session,
	    NULL, WT_LOGSCAN_FIRST | WT_LOGSCAN_RECOVER,
	    __txn_log_recover, &metadata));


err:	if (rec_session != NULL)
		WT_TRET(rec_session->iface.close(&rec_session->iface, NULL));

	return (ret);
}
