/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curlog_logrec --
 *	Callback function from log_scan to get a log record.
 */
static int
__curlog_logrec(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	WT_CURSOR_LOG *cl;

	WT_UNUSED(session);
	cl = cookie;
	/*
	 * Set up the LSNs and take a copy of the log record for the cursor.
	 */
	*cl->cur_lsn = *lsnp;
	*cl->next_lsn = *lsnp;
	cl->next_lsn->offset += logrec->size;
	if (cl->logrec == NULL)
		__wt_scr_alloc(session, logrec->size, &cl->logrec);
	WT_RET(__wt_buf_set(session,
	    cl->logrec, logrec->data, logrec->size));
	/*
	 * Skip the log header.  We need to do the exact same calculation
	 * to get the record type out, so setup up the step cursor pointers.
	 */
	cl->stepp = (const uint8_t *)cl->logrec->data +
	    offsetof(WT_LOG_RECORD, record);
	cl->stepp_end = (const uint8_t *)cl->logrec->data + logrec->size;
	WT_RET(__wt_logrec_read(session, &cl->stepp, cl->stepp_end,
	    &cl->rectype));
	return (0);
}

/*
 * __curlog_compare --
 *	WT_CURSOR.compare method for the log cursor type.
 */
static int
__curlog_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_CURSOR_LOG *acl, *bcl;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(a, session, compare, NULL);

	acl = (WT_CURSOR_LOG *)a;
	bcl = (WT_CURSOR_LOG *)b;
	WT_ASSERT(session, cmpp != NULL);
	*cmpp = LOG_CMP(acl->cur_lsn, bcl->cur_lsn);
	/*
	 * If these are step cursors, compare the count
	 * within the record if they are both on the same LSN.
	 */
	if (*cmpp == 0 && F_ISSET(acl, WT_LOGC_STEP)) {
		WT_ASSERT(session, F_ISSET(bcl, WT_LOGC_STEP));
		*cmpp = (acl->step_count != bcl->step_count ?
		    (acl->step_count < bcl->step_count ? -1 : 1) : 0);
	}
err:	API_END_RET(session, ret);

}

/*
 * __curlog_steprec --
 *	Callback function from log_scan to get a log record for stepping.
 */
static int
__curlog_steprec(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	WT_CURSOR_LOG *cl;

	/*
	 * We need to do the common things a non-step cursor does first.
	 */
	WT_RET(__curlog_logrec(session, logrec, lsnp, cookie));
	cl = cookie;

	/*
	 * The record count within a record starts at 1 so that we can
	 * reserve 0 to mean the entire record.
	 */
	cl->step_count = 1;
	if (cl->opkey == NULL)
		__wt_scr_alloc(session, 0, &cl->opkey);
	if (cl->opvalue == NULL)
		__wt_scr_alloc(session, 0, &cl->opvalue);
	/*
	 * Unpack the txnid so that we can return each
	 * individual operation for this txnid.
	 */
	if (cl->rectype == WT_LOGREC_COMMIT)
		WT_RET(__wt_vunpack_uint(&cl->stepp,
		    WT_PTRDIFF(cl->stepp_end, cl->stepp), &cl->txnid));
	else
		cl->txnid = 0;
	return (0);
}

/*
 * __curlog_op_read --
 *	Read out any key/value from an individual operation record
 *	in the log.  We're only interested in put and remove operations
 *	since truncate is not a cursor operation.  All successful
 *	returns from this function will have set up the cursor copy of
 *	key and value to give the user.
 */
static int
__curlog_op_read(WT_SESSION_IMPL *session,
    WT_CURSOR_LOG *cl, uint32_t optype, uint32_t opsize, uint32_t *fileid)
{
	WT_ITEM key, value;
	uint64_t recno;
	const uint8_t *end, *pp;

	pp = cl->stepp;
	end = pp + opsize;
	switch (optype) {
	case WT_LOGOP_COL_PUT:
		WT_RET(__wt_logop_col_put_unpack(session, &pp, end,
		    fileid, &recno, &value));
		WT_RET(__wt_buf_set(session, cl->opkey, &recno, sizeof(recno)));
		WT_RET(__wt_buf_set(session,
		    cl->opvalue, value.data, value.size));
		break;
	case WT_LOGOP_COL_REMOVE:
		WT_RET(__wt_logop_col_remove_unpack(session, &pp, end,
		    fileid, &recno));
		WT_RET(__wt_buf_set(session, cl->opkey, &recno, sizeof(recno)));
		WT_RET(__wt_buf_set(session, cl->opvalue, NULL, 0));
		break;
	case WT_LOGOP_ROW_PUT:
		WT_RET(__wt_logop_row_put_unpack(session, &pp, end,
		    fileid, &key, &value));
		WT_RET(__wt_buf_set(session, cl->opkey, key.data, key.size));
		WT_RET(__wt_buf_set(session,
		    cl->opvalue, value.data, value.size));
		break;
	case WT_LOGOP_ROW_REMOVE:
		WT_RET(__wt_logop_row_remove_unpack(session, &pp, end,
		    fileid, &key));
		WT_RET(__wt_buf_set(session, cl->opkey, key.data, key.size));
		WT_RET(__wt_buf_set(session, cl->opvalue, NULL, 0));
		break;
	default:
		/*
		 * Any other operations return the record in the value
		 * and an empty key.
		 */
		*fileid = 0;
		WT_RET(__wt_buf_set(session, cl->opkey, NULL, 0));
		WT_RET(__wt_buf_set(session, cl->opvalue, cl->stepp, opsize));
	}
	return (0);
}

/*
 * __curlog_stepkv --
 *	Set the key and value to return for a step cursor.
 */
static int
__curlog_stepkv(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	uint32_t fileid, opsize, optype;

	cl = (WT_CURSOR_LOG *)cursor;
	/*
	 * If it is a commit, peek to get the size and optype and
	 * read out any key/value from this operation.
	 */
	if (cl->rectype == WT_LOGREC_COMMIT) {
		WT_RET(__wt_logop_read(session,
		    &cl->stepp, cl->stepp_end, &optype, &opsize));
		WT_RET(__curlog_op_read(session, cl, optype, opsize, &fileid));
	} else {
		optype = WT_LOGOP_INVALID;
		opsize = cl->logrec->size;
		fileid = 0;
		WT_RET(__wt_buf_set(session, cl->opkey, NULL, 0));
		WT_RET(__wt_buf_set(session,
		    cl->opvalue, cl->stepp, opsize));
	}
	__wt_cursor_set_key(cursor, cl->cur_lsn->file, cl->cur_lsn->offset,
	    cl->step_count++);
	__wt_cursor_set_value(cursor, cl->txnid, cl->rectype, optype,
	    fileid, cl->opkey, cl->opvalue);
	cl->stepp += opsize;
	return (0);
}

/*
 * __curlog_stepnext --
 *	WT_CURSOR.next method for the step log cursor type.
 */
static int
__curlog_stepnext(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);

	/*
	 * If we don't have a record, or went to the end of the record we
	 * have, or we are in the zero-fill portion of the record, get a
	 * new one.
	 */
	if (cl->logrec == NULL || cl->stepp >= cl->stepp_end ||
	    !(*cl->stepp)) {
		cl->txnid = 0;
		WT_ERR(__wt_log_scan(session, cl->next_lsn, WT_LOGSCAN_ONE,
		    cl->scan_cb, cl));
	}
	WT_ASSERT(session, cl->logrec->data != NULL);
	WT_STAT_FAST_CONN_INCR(session, cursor_next);
	WT_STAT_FAST_DATA_INCR(session, cursor_next);
	WT_ERR(__curlog_stepkv(session, cursor));

err:	API_END_RET(session, ret);

}

/*
 * __curlog_next --
 *	WT_CURSOR.next method for the log cursor type.
 */
static int
__curlog_next(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_ITEM opkey;
	WT_SESSION_IMPL *session;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);

	WT_ERR(__wt_log_scan(session, cl->next_lsn, WT_LOGSCAN_ONE,
	    __curlog_logrec, cl));
	__wt_cursor_set_key(cursor, cl->cur_lsn->file, cl->cur_lsn->offset);
	opkey.data = NULL;
	opkey.size = 0;
	__wt_cursor_set_value(cursor, cl->txnid, cl->rectype, WT_LOGOP_INVALID,
	    0, &opkey, cl->logrec);
	WT_STAT_FAST_CONN_INCR(session, cursor_next);
	WT_STAT_FAST_DATA_INCR(session, cursor_next);

err:	API_END_RET(session, ret);

}

/*
 * __curlog_search --
 *	WT_CURSOR.search method for the log cursor type.
 */
static int
__curlog_search(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_ITEM opkey;
	WT_LSN key;
	WT_SESSION_IMPL *session;
	uint32_t counter;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, search, NULL);

	/*
	 * !!! We are ignoring the counter on a step cursor
	 * and only searching based on the LSN.
	 */
	if (F_ISSET(cl, WT_LOGC_STEP))
		WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl,
		    &key.file, &key.offset, &counter));
	else
		WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl,
		    &key.file, &key.offset));
	WT_ERR(__wt_log_scan(session, &key, WT_LOGSCAN_ONE,
	    cl->scan_cb, cl));
	/*
	 * If it is a step cursor set the key and value.
	 */
	if (F_ISSET(cl, WT_LOGC_STEP))
		WT_ERR(__curlog_stepkv(session, cursor));
	else {
		opkey.data = NULL;
		opkey.size = 0;
		__wt_cursor_set_value(cursor, cl->txnid, cl->rectype,
		    WT_LOGOP_INVALID, 0, &opkey, cl->logrec);
	}
	WT_STAT_FAST_CONN_INCR(session, cursor_search);
	WT_STAT_FAST_DATA_INCR(session, cursor_search);

err:	API_END_RET(session, ret);
}

/*
 * __curlog_reset --
 *	WT_CURSOR.reset method for the log cursor type.
 */
static int
__curlog_reset(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;

	cl = (WT_CURSOR_LOG *)cursor;
	cl->stepp = cl->stepp_end = NULL;
	if (F_ISSET(cl, WT_LOGC_STEP)) {
		cl->step_count = 0;
		if (cl->opkey != NULL)
			__wt_scr_free(&cl->opkey);
		if (cl->opvalue != NULL)
			__wt_scr_free(&cl->opvalue);
	}
	if (cl->logrec != NULL)
		__wt_scr_free(&cl->logrec);
	INIT_LSN(cl->cur_lsn);
	INIT_LSN(cl->next_lsn);
	return (0);
}

/*
 * __curlog_close --
 *	WT_CURSOR.close method for the log cursor type.
 */
static int
__curlog_close(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, close, NULL);
	cl = (WT_CURSOR_LOG *)cursor;
	WT_ERR(__curlog_reset(cursor));
	__wt_free(session, cl->cur_lsn);
	__wt_free(session, cl->next_lsn);
	WT_ERR(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __curlog_stepinit --
 *	Initialize the step portion of the log cursor.
 */
static int
__curlog_stepinit(WT_SESSION_IMPL *session, WT_CURSOR_LOG *cl)
{
	WT_CURSOR *cursor;

	WT_UNUSED(session);
	cursor = &cl->iface;
	cursor->next = __curlog_stepnext;
	cursor->key_format = LOGCSTEP_KEY_FORMAT;
	cl->scan_cb = __curlog_steprec;
	F_SET(cl, WT_LOGC_STEP);
	return (0);
}

/*
 * __wt_curlog_open --
 *	Initialize a log cursor.
 */
int
__wt_curlog_open(WT_SESSION_IMPL *session,
    const char *uri, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR_STATIC_INIT(iface,
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    NULL,			/* set-value */
	    __curlog_compare,		/* compare */
	    __curlog_next,		/* next */
	    __wt_cursor_notsup,		/* prev */
	    __curlog_reset,		/* reset */
	    __curlog_search,		/* search */
	    __wt_cursor_notsup,		/* search-near */
	    __wt_cursor_notsup,		/* insert */
	    __wt_cursor_notsup,		/* update */
	    __wt_cursor_notsup,		/* remove */
	    __curlog_close);		/* close */
	WT_CONFIG_ITEM cval;
	WT_CURSOR *cursor;
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;

	STATIC_ASSERT(offsetof(WT_CURSOR_LOG, iface) == 0);
	conn = S2C(session);
	if (!conn->logging)
		WT_RET_MSG(session, EINVAL,
		    "Cannot open a log cursor without logging enabled");

	cl = NULL;
	WT_RET(__wt_calloc_def(session, 1, &cl));
	cursor = &cl->iface;
	*cursor = iface;
	WT_ERR(__wt_calloc_def(session, 1, &cl->cur_lsn));
	WT_ERR(__wt_calloc_def(session, 1, &cl->next_lsn));
	cursor->session = &session->iface;

	/*
	 * We return a record's LSN as the key, and the raw WT_ITEM
	 * that is the record as the value.
	 */
	WT_ERR(__wt_config_gets_def(session, cfg, "step", 0, &cval));
	if (cval.val != 0) {
		__curlog_stepinit(session, cl);
	} else {
		cursor->key_format = LOGC_KEY_FORMAT;
		cl->scan_cb = __curlog_logrec;
	}
	cursor->value_format = LOGC_VALUE_FORMAT;

	INIT_LSN(cl->cur_lsn);
	INIT_LSN(cl->next_lsn);
	WT_CLEAR(cl->logrec);

	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	/* Log cursors default to read only. */
	WT_ERR(__wt_cursor_config_readonly(cursor, cfg, 1));

	if (0) {
err:		if (F_ISSET(cursor, WT_CURSTD_OPEN))
			WT_TRET(cursor->close(cursor));
		else {
			if (cl->next_lsn)
				__wt_free(session, cl->next_lsn);
			__wt_free(session, cl);
		}
		*cursorp = NULL;
	}

	return (ret);
}
