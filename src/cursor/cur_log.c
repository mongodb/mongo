/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
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
__curlog_logrec(WT_SESSION_IMPL *session,
    WT_ITEM *logrec, WT_LSN *lsnp, WT_LSN *next_lsnp,
    void *cookie, int firstrecord)
{
	WT_CURSOR_LOG *cl;

	cl = cookie;
	WT_UNUSED(firstrecord);

	/* Set up the LSNs and take a copy of the log record for the cursor. */
	*cl->cur_lsn = *lsnp;
	*cl->next_lsn = *next_lsnp;
	WT_RET(__wt_buf_set(session, cl->logrec, logrec->data, logrec->size));

	/*
	 * Read the log header.  Set up the step pointers to walk the
	 * operations inside the record.  Get the record type.
	 */
	cl->stepp = WT_LOG_SKIP_HEADER(cl->logrec->data);
	cl->stepp_end = (uint8_t *)cl->logrec->data + logrec->size;
	WT_RET(__wt_logrec_read(session, &cl->stepp, cl->stepp_end,
	    &cl->rectype));

	/* A step count of 0 means the entire record. */
	cl->step_count = 0;

	/*
	 * Unpack the txnid so that we can return each
	 * individual operation for this txnid.
	 */
	if (cl->rectype == WT_LOGREC_COMMIT)
		WT_RET(__wt_vunpack_uint(&cl->stepp,
		    WT_PTRDIFF(cl->stepp_end, cl->stepp), &cl->txnid));
	else {
		/*
		 * Step over anything else.
		 * Setting stepp to NULL causes the next()
		 * method to read a new record on the next call.
		 */
		cl->stepp = NULL;
		cl->txnid = 0;
	}
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
	*cmpp = __wt_log_cmp(acl->cur_lsn, bcl->cur_lsn);
	/*
	 * If both are on the same LSN, compare step counter.
	 */
	if (*cmpp == 0)
		*cmpp = (acl->step_count != bcl->step_count ?
		    (acl->step_count < bcl->step_count ? -1 : 1) : 0);
err:	API_END_RET(session, ret);

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
 * __curlog_kv --
 *	Set the key and value of the log cursor to return to the user.
 */
static int
__curlog_kv(WT_SESSION_IMPL *session, WT_CURSOR *cursor)
{
	WT_CURSOR_LOG *cl;
	WT_ITEM item;
	uint32_t fileid, key_count, opsize, optype;

	cl = (WT_CURSOR_LOG *)cursor;
	/*
	 * If it is a commit and we have stepped over the header, peek to get
	 * the size and optype and read out any key/value from this operation.
	 */
	if ((key_count = cl->step_count++) > 0) {
		WT_RET(__wt_logop_read(session,
		    &cl->stepp, cl->stepp_end, &optype, &opsize));
		WT_RET(__curlog_op_read(session, cl, optype, opsize, &fileid));
		/* Position on the beginning of the next record part. */
		cl->stepp += opsize;
	} else {
		optype = WT_LOGOP_INVALID;
		fileid = 0;
		cl->opkey->data = NULL;
		cl->opkey->size = 0;
		/*
		 * Non-commit records we want to return the record without the
		 * header and the adjusted size.  Add one to skip over the type
		 * which is normally consumed by __wt_logrec_read.
		 */
		cl->opvalue->data = WT_LOG_SKIP_HEADER(cl->logrec->data) + 1;
		cl->opvalue->size = WT_LOG_REC_SIZE(cl->logrec->size) - 1;
	}
	/*
	 * The log cursor sets the LSN and step count as the cursor key and
	 * and log record related data in the value.  The data in the value
	 * contains any operation key/value that was in the log record.
	 * For the special case that the caller needs the result in raw form,
	 * we create packed versions of the key/value.
	 */
	if (FLD_ISSET(cursor->flags, WT_CURSTD_RAW)) {
		memset(&item, 0, sizeof(item));
		WT_RET(wiredtiger_struct_size((WT_SESSION *)session,
		    &item.size, WT_LOGC_KEY_FORMAT, cl->cur_lsn->l.file,
		    cl->cur_lsn->l.offset, key_count));
		WT_RET(__wt_realloc(session, NULL, item.size, &cl->packed_key));
		item.data = cl->packed_key;
		WT_RET(wiredtiger_struct_pack((WT_SESSION *)session,
		    cl->packed_key, item.size, WT_LOGC_KEY_FORMAT,
		    cl->cur_lsn->l.file, cl->cur_lsn->l.offset, key_count));
		__wt_cursor_set_key(cursor, &item);

		WT_RET(wiredtiger_struct_size((WT_SESSION *)session,
		    &item.size, WT_LOGC_VALUE_FORMAT, cl->txnid, cl->rectype,
		    optype, fileid, cl->opkey, cl->opvalue));
		WT_RET(__wt_realloc(session, NULL, item.size,
		    &cl->packed_value));
		item.data = cl->packed_value;
		WT_RET(wiredtiger_struct_pack((WT_SESSION *)session,
		    cl->packed_value, item.size, WT_LOGC_VALUE_FORMAT,
		    cl->txnid, cl->rectype, optype, fileid, cl->opkey,
		    cl->opvalue));
		__wt_cursor_set_value(cursor, &item);
	} else {
		__wt_cursor_set_key(cursor, cl->cur_lsn->l.file,
		    cl->cur_lsn->l.offset, key_count);
		__wt_cursor_set_value(cursor, cl->txnid, cl->rectype, optype,
		    fileid, cl->opkey, cl->opvalue);
	}
	return (0);
}

/*
 * __curlog_next --
 *	WT_CURSOR.next method for the step log cursor type.
 */
static int
__curlog_next(WT_CURSOR *cursor)
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
	if (cl->stepp == NULL || cl->stepp >= cl->stepp_end || !*cl->stepp) {
		cl->txnid = 0;
		ret = __wt_log_scan(session, cl->next_lsn, WT_LOGSCAN_ONE,
		    __curlog_logrec, cl);
		if (ret == ENOENT)
			ret = WT_NOTFOUND;
		WT_ERR(ret);
	}
	WT_ASSERT(session, cl->logrec->data != NULL);
	WT_ERR(__curlog_kv(session, cursor));
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
	uint32_t counter, key_file, key_offset;

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, search, NULL);

	/*
	 * !!! We are ignoring the counter and only searching based on the LSN.
	 */
	WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl,
	    &key_file, &key_offset, &counter));
	WT_SET_LSN(&key, key_file, key_offset);
	ret = __wt_log_scan(session, &key, WT_LOGSCAN_ONE,
	    __curlog_logrec, cl);
	if (ret == ENOENT)
		ret = WT_NOTFOUND;
	WT_ERR(ret);
	WT_ERR(__curlog_kv(session, cursor));
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
	cl->step_count = 0;
	WT_INIT_LSN(cl->cur_lsn);
	WT_INIT_LSN(cl->next_lsn);
	return (0);
}

/*
 * __curlog_close --
 *	WT_CURSOR.close method for the log cursor type.
 */
static int
__curlog_close(WT_CURSOR *cursor)
{
	WT_CONNECTION_IMPL *conn;
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_LOG *log;
	WT_SESSION_IMPL *session;

	CURSOR_API_CALL(cursor, session, close, NULL);
	cl = (WT_CURSOR_LOG *)cursor;
	conn = S2C(session);
	WT_ASSERT(session, FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED));
	log = conn->log;
	WT_TRET(__wt_readunlock(session, log->log_archive_lock));
	WT_TRET(__curlog_reset(cursor));
	__wt_free(session, cl->cur_lsn);
	__wt_free(session, cl->next_lsn);
	__wt_scr_free(session, &cl->logrec);
	__wt_scr_free(session, &cl->opkey);
	__wt_scr_free(session, &cl->opvalue);
	__wt_free(session, cl->packed_key);
	__wt_free(session, cl->packed_value);
	WT_TRET(__wt_cursor_close(cursor));

err:	API_END_RET(session, ret);
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
	    __wt_cursor_get_key,		/* get-key */
	    __wt_cursor_get_value,		/* get-value */
	    __wt_cursor_set_key,		/* set-key */
	    __wt_cursor_set_value,		/* set-value */
	    __curlog_compare,			/* compare */
	    __wt_cursor_equals,			/* equals */
	    __curlog_next,			/* next */
	    __wt_cursor_notsup,			/* prev */
	    __curlog_reset,			/* reset */
	    __curlog_search,			/* search */
	    __wt_cursor_search_near_notsup,	/* search-near */
	    __wt_cursor_notsup,			/* insert */
	    __wt_cursor_notsup,			/* update */
	    __wt_cursor_notsup,			/* remove */
	    __wt_cursor_reconfigure_notsup,	/* reconfigure */
	    __curlog_close);			/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_LOG *cl;
	WT_DECL_RET;
	WT_LOG *log;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_LOG, iface) == 0);
	conn = S2C(session);
	if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
		WT_RET_MSG(session, EINVAL,
		    "Cannot open a log cursor without logging enabled");

	log = conn->log;
	cl = NULL;
	WT_RET(__wt_calloc_one(session, &cl));
	cursor = &cl->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	WT_ERR(__wt_calloc_one(session, &cl->cur_lsn));
	WT_ERR(__wt_calloc_one(session, &cl->next_lsn));
	WT_ERR(__wt_scr_alloc(session, 0, &cl->logrec));
	WT_ERR(__wt_scr_alloc(session, 0, &cl->opkey));
	WT_ERR(__wt_scr_alloc(session, 0, &cl->opvalue));
	cursor->key_format = WT_LOGC_KEY_FORMAT;
	cursor->value_format = WT_LOGC_VALUE_FORMAT;

	WT_INIT_LSN(cl->cur_lsn);
	WT_INIT_LSN(cl->next_lsn);

	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

	/*
	 * The user may be trying to read a log record they just wrote.
	 * Log records may be buffered, so force out any now.
	 */
	WT_ERR(__wt_log_force_write(session, 1, NULL));

	/* Log cursors block archiving. */
	WT_ERR(__wt_readlock(session, log->log_archive_lock));

	if (0) {
err:		if (F_ISSET(cursor, WT_CURSTD_OPEN))
			WT_TRET(cursor->close(cursor));
		else {
			__wt_free(session, cl->cur_lsn);
			__wt_free(session, cl->next_lsn);
			__wt_scr_free(session, &cl->logrec);
			__wt_scr_free(session, &cl->opkey);
			__wt_scr_free(session, &cl->opvalue);
			/*
			 * NOTE:  We cannot get on the error path with the
			 * readlock held.  No need to unlock it unless that
			 * changes above.
			 */
			__wt_free(session, cl);
		}
		*cursorp = NULL;
	}

	return (ret);
}
