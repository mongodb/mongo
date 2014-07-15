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
	WT_CURSOR_LOG *al, *bl;
	WT_DECL_RET;
	WT_LSN alsn, blsn;
	WT_SESSION_IMPL *session;

	al = (WT_CURSOR_LOG *)a;
	bl = (WT_CURSOR_LOG *)b;
	CURSOR_API_CALL(a, session, compare, NULL);

	WT_ASSERT(session, cmpp != NULL);
	WT_ERR(__wt_cursor_get_key(a, &alsn.file, &alsn.offset));
	WT_ERR(__wt_cursor_get_key(b, &blsn.file, &blsn.offset));
	*cmpp = LOG_CMP(&alsn, &blsn);
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
	*cl->next_lsn = *lsnp;
	cl->next_lsn->offset += logrec->size;
	__wt_cursor_set_key((WT_CURSOR *)cl, lsnp->file, lsnp->offset);
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

	cl = (WT_CURSOR_LOG *)cursor;

	CURSOR_API_CALL(cursor, session, search, NULL);

	WT_ERR(__wt_cursor_get_key((WT_CURSOR *)cl, &key.file, &key.offset));
	WT_ERR(__wt_log_scan(session, &key, WT_LOGSCAN_ONE,
	    __curlog_logrec, &cl));
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
	/*
	 * Resetting a log cursor just means clearing its LSN.
	 */
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
	__wt_free(session, cl->next_lsn);
	WT_ERR(__wt_cursor_close(cursor));

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
	    NULL,			/* get-key */
	    NULL,			/* get-value */
	    NULL,			/* set-key */
	    __wt_cursor_notsup,		/* set-value */
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
	WT_ERR(__wt_calloc_def(session, 1, &cl->next_lsn));
	cursor = &cl->iface;
	*cursor = iface;
	cursor->session = &session->iface;

	/*
	 * We return a record's LSN as the key, and the raw WT_ITEM
	 * that is the record as the value.
	 */
	cursor->key_format = LSN_KEY_FORMAT;
	cursor->value_format = "u";

	INIT_LSN(cl->next_lsn);

	/* __wt_cursor_init is last so we don't have to clean up on error. */
	WT_ERR(__wt_cursor_init(cursor, uri, NULL, cfg, cursorp));

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
