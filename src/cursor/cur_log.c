/*-
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __curlog_compare --
 *	WT_CURSOR.compare method for the log cursor type.
 */
static int
__curlog_compare(WT_CURSOR *a, WT_CURSOR *b, int *cmpp)
{
	WT_DECL_RET;
	WT_LSN alsn, blsn;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(a, session, compare, NULL);

	WT_ASSERT(session, cmpp != NULL);
	WT_ERR(__wt_cursor_get_key(a, &alsn.file, &alsn.offset));
	WT_ERR(__wt_cursor_get_key(b, &blsn.file, &blsn.offset));
	*cmpp = LOG_CMP(&alsn, &blsn);
err:	API_END_RET(session, ret);

}

/*
 * __curlog_iterrec --
 *	Callback function from log_scan to get a log record.
 */
static int
__curlog_iterrec(
    WT_SESSION_IMPL *session, WT_ITEM *logrec, WT_LSN *lsnp, void *cookie)
{
	WT_CURSOR_LOG *cl;

	WT_UNUSED(session);
	cl = cookie;
	/*
	 * Update the cursor's LSN to this record so that we know the next
	 * full record to read.
	 */
	*cl->cur_lsn = *lsnp;
	*cl->next_lsn = *lsnp;
	cl->next_lsn->offset += logrec->size;
	/*
	 * We're starting with a new full record.  We need to copy it into
	 * the cursor, and then set up our pointers within it.
	 */
	if (cl->iter_logrec == NULL)
		__wt_scr_alloc(session, logrec->size, &cl->iter_logrec);
	WT_RET(__wt_buf_set(session,
	    cl->iter_logrec, logrec->data, logrec->size));
	/*
	 * Skip the log header.
	 */
	cl->iterp = (const uint8_t *)cl->iter_logrec->data +
	    offsetof(WT_LOG_RECORD, record);
	cl->iterp_end = (const uint8_t *)cl->iter_logrec->data + logrec->size;
	/*
	 * The record count within a record starts at 1 so that we can
	 * reserve 0 to mean the entire record.
	 */
	cl->iter_count = 1;
	return (0);
}

/*
 * __curlog_iternext --
 *	WT_CURSOR.next method for the log cursor type.
 */
static int
__curlog_iternext(WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_ITEM value;
	WT_SESSION_IMPL *session;
	uint32_t opsize, optype;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);

	opsize = optype = 0;
	/*
	 * If we don't have a record, or went to the end of the record we
	 * have, or we are in the zero-fill portion of the record, get a
	 * new one.
	 */
	if (cl->iter_logrec == NULL || cl->iterp >= cl->iterp_end ||
	    !(*cl->iterp)) {
		cl->txnid = 0;
		WT_ERR(__wt_log_scan(session, cl->next_lsn, WT_LOGSCAN_ONE,
		    __curlog_iterrec, cl));
		WT_ERR(__wt_logrec_read(session, &cl->iterp, cl->iterp_end,
		    &cl->rectype));
		/*
		 * If it is not a commit record, return the whole record.
		 * Otherwise, unpack the txnid so that we can return each
		 * individual operation for this txnid.
		 */
		if (cl->rectype != WT_LOGREC_COMMIT)
			opsize = cl->iter_logrec->size;
		else
			WT_ERR(__wt_vunpack_uint(&cl->iterp,
			    WT_PTRDIFF(cl->iterp_end, cl->iterp), &cl->txnid));
	}
	WT_ASSERT(session, cl->iter_logrec->data != NULL);
	WT_STAT_FAST_CONN_INCR(session, cursor_next);
	WT_STAT_FAST_DATA_INCR(session, cursor_next);
	/*
	 * Peek to get the size and optype.  If it isn't a commit
	 * record, we set opsize to the record size above because we're
	 * returning the entire record.  If it is a commit, we're walking
	 * the parts of the record.
	 */
	if (cl->rectype == WT_LOGREC_COMMIT)
		WT_ERR(__wt_logop_read(session, &cl->iterp, cl->iterp_end,
		    &optype, &opsize));
	__wt_cursor_set_key(cursor, cl->cur_lsn->file, cl->cur_lsn->offset,
	    cl->txnid, cl->iter_count++);
	value.data = cl->iterp;
	value.size = opsize;
	cl->iterp += opsize;
	__wt_cursor_set_raw_value(cursor, &value);

err:	API_END_RET(session, ret);

}

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
	 * Update the cursor's LSN to this record and set its key to this
	 * LSN.  Set the log record as the value.
	 */
	*cl->cur_lsn = *lsnp;
	*cl->next_lsn = *lsnp;
	cl->next_lsn->offset += logrec->size;
	__wt_cursor_set_raw_value((WT_CURSOR *)cl, logrec);
	return (0);
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
	WT_SESSION_IMPL *session;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);

	WT_ERR(__wt_log_scan(session, cl->next_lsn, WT_LOGSCAN_ONE,
	    __curlog_logrec, cl));
	__wt_cursor_set_key(cursor, cl->cur_lsn->file, cl->cur_lsn->offset);
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
	WT_LSN key;
	WT_SESSION_IMPL *session;
	uint64_t txnid;
	uint32_t opno;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, search, NULL);

	if (F_ISSET(cl, WT_LOGC_ITERATOR))
		WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl,
		    &key.file, &key.offset, &txnid, &opno));
	else
		WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl,
		    &key.file, &key.offset));
	WT_ERR(__wt_log_scan(session, &key, WT_LOGSCAN_ONE,
	    cl->scan_cb, cl));
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
	if (F_ISSET(cl, WT_LOGC_ITERATOR) && cl->iter_logrec != NULL) {
		cl->iterp = cl->iterp_end = NULL;
		cl->iter_count = 0;
		__wt_scr_free(&cl->iter_logrec);
	}
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
	if (F_ISSET(cl, WT_LOGC_ITERATOR) && cl->iter_logrec != NULL) {
		cl->iterp = cl->iterp_end = NULL;
		cl->iter_count = 0;
		__wt_scr_free(&cl->iter_logrec);
	}
	__wt_free(session, cl->cur_lsn);
	__wt_free(session, cl->next_lsn);
	WT_ERR(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
}

/*
 * __curlog_iterinit --
 *	Initialize the iterator portion of the log cursor.
 */
static int
__curlog_iterinit(WT_SESSION_IMPL *session, WT_CURSOR_LOG *cl)
{
	WT_CURSOR *cursor;

	cursor = &cl->iface;
	cursor->next = __curlog_iternext;
	cursor->key_format = LSNITER_KEY_FORMAT;
	cl->scan_cb = __curlog_iterrec;
	F_SET(cl, WT_LOGC_ITERATOR);
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
	WT_RET(__wt_calloc_def(session, 1, &cl->cur_lsn));
	WT_ERR(__wt_calloc_def(session, 1, &cl->next_lsn));
	cursor->session = &session->iface;

	/*
	 * We return a record's LSN as the key, and the raw WT_ITEM
	 * that is the record as the value.
	 */
	WT_ERR(__wt_config_gets_def(session, cfg, "iterator", 0, &cval));
	if (cval.val != 0) {
		__curlog_iterinit(session, cl);
	} else {
		cursor->key_format = LSN_KEY_FORMAT;
		cl->scan_cb = __curlog_logrec;
	}
	cursor->value_format = "u";

	INIT_LSN(cl->cur_lsn);
	INIT_LSN(cl->next_lsn);
	WT_CLEAR(cl->iter_logrec);

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
