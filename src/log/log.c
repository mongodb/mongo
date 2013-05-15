/*-
 * Copyright (c) 2008-2013 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int
__log_record_size(WT_SESSION_IMPL *session,
    size_t *sizep, WT_LOGREC_DESC *recdesc, va_list ap)
{
	return (__wt_struct_sizev(session, sizep, recdesc->fmt, ap));
}

int
__wt_log_filename(WT_SESSION_IMPL *session, WT_LOG *log, WT_ITEM *buf)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;

	conn = S2C(session);
	WT_RET(__wt_buf_initsize(session, buf,
	    strlen(conn->log_path) + ENTRY_SIZE));
	WT_ERR(__wt_buf_fmt(session, buf, "%s/%s.%10" PRIu32,
	    conn->log_path, WT_LOG_FILENAME, log->fileid));
	return (0);

err:	__wt_buf_free(session, buf);
	return (ret);
}

#ifdef	NOTDEF
int
__wt_log_put(WT_SESSION_IMPL *session, WT_LOGREC_DESC *recdesc, ...)
{
	WT_DECL_RET;
	WT_ITEM *buf;
	va_list ap;
	size_t size;

	buf = &session->logrec_buf;
	size = 0;

	va_start(ap, recdesc);
	ret = __log_record_size(session, &size, recdesc, ap);
	va_end(ap);
	WT_RET(ret);

	WT_RET(__wt_buf_initsize(session, buf, size));

	va_start(ap, recdesc);
	ret = __wt_struct_packv(session, buf->mem, size, recdesc->fmt, ap);
	va_end(ap);
	return (ret);
}
#endif

int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_ITEM *buf;
	va_list ap_copy;
	size_t len;

	conn = S2C(session);

	if (!conn->logging)
		return (0);

	buf = &session->logprint_buf;

	va_copy(ap_copy, ap);
	len = (size_t)vsnprintf(NULL, 0, fmt, ap_copy) + 2;
	va_end(ap_copy);

	WT_RET(__wt_buf_initsize(session, buf, len));

	(void)vsnprintf(buf->mem, len, fmt, ap);

	/*
	 * For now, just dump the text into the file.  Later, we will use
	 * __wt_logput_debug to wrap this in a log header.
	 */
#if 0
	strcpy((char *)buf->mem + len - 2, "\n");
	return ((write(conn->log_fh->fd, buf->mem, len - 1) ==
	    (ssize_t)len - 1) ? 0 : WT_ERROR);
	return (__wt_logput_debug(session, (char *)buf->mem));
#endif
	WT_VERBOSE_RET(session, log, "log record: %s\n", (char *)buf->mem);
	return (0);
}

#if 0
int
__wt_log_printf(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = __wt_log_vprintf(session, fmt, ap);
	va_end(ap);

	return (ret);
}
#endif

/*
 * __wt_log_open --
 *	Open the log file.
 */
int
__wt_log_open(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_ITEM(path);
	WT_DECL_RET;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	WT_RET(__wt_scr_alloc(session, 0, &path));
	WT_ERR(__wt_log_filename(session, log, path));
	WT_VERBOSE_ERR(session, log, "opening log %s",
	    (const char *)path->data);
	WT_ERR(__wt_open(session, path->data, 1, 0, 0, &log->log_fh));
err:	__wt_scr_free(&path);
	return (ret);
}

/*
 * __wt_log_close --
 *	Close the log file.
 */
int
__wt_log_close(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;
	/*
	 * If we don't have a log open, there's nothing to do.
	 */
	if (log->log_fh == NULL)
		return (0);
	WT_VERBOSE_RET(session, log, "closing log %s", log->log_fh->name);
	WT_RET(__wt_close(session, log->log_fh));
	log->log_fh = NULL;
	return (0);
}

static int
__log_size_fit(WT_SESSION_IMPL *session, WT_LSN *lsn, uint32_t size)
{
	WT_CONNECTION_IMPL *conn;

	conn = S2C(session);
	return (lsn->offset + size < conn->log_file_max);

}

static int
__log_newfile(WT_SESSION_IMPL *session)
{
	WT_CONNECTION_IMPL *conn;
	WT_LOG *log;

	conn = S2C(session);
	log = conn->log;

	/*
	 * Set aside the log file handle to be closed later.  Other threads
	 * may still be using it to write to the log.
	 */
	log->log_close_fh = log->log_fh;
	log->fileid++;
	WT_RET(__wt_log_open(session));
	/*
	xxx - need to write log file header record then update lsns.
	*/

	return (0);
}

static int
__log_acquire(WT_SESSION_IMPL *session, uint32_t size, WT_LOGSLOT *slot)
{
	WT_CONNECTION_IMPL *conn;
	WT_DECL_RET;
	WT_LOG *log;
	WT_LSN *base_lsn, *tmp_lsn;

	conn = S2C(session);
	log = conn->log;
	/*
	 * Called locked.  Add size to alloc_lsn.  Update some lsns.
	 * Return base lsn.
	 */
	if (!__log_size_fit(session, log->alloc_lsn, size)) {
		FLD_SET(slot->slot_flags, SLOT_CLOSEFH);
		base_lsn = __log_newfile(session);
	}
	slot->slot_lsn = log->alloc_lsn;
	log->alloc_lsn->offset += size;
	slot->slot_fh = log->log_fh;
	return (0);

}

static int
__log_fill(WT_SESSION_IMPL *session, WT_ITEM *record)
{
	/*
	 * Call __wt_write.
	 */
	return (0);
}

static int
__log_release(WT_SESSION_IMPL *session, uint32_t size, WT_LOGSLOT *slot)
{
	/*
	 * Take flags.  While current write lsn != my end lsn wait my turn.
	 * Set my lsn.  If sync, call __wt_fsync.  Update lsns.
	if (FLD_ISSET(slot->slot_flags, SLOT_SYNC))
	 */

	return (0);
}

int
__wt_log_read(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	return (0);
}

int
__wt_log_scan(WT_SESSION_IMPL *session, WT_ITEM *record, uint32_t flags,
    int (*func)(WT_SESSION_IMPL *session, WT_ITEM *record, void *cookie),
    void *cookie)
{
	return (0);
}

int
__wt_log_write(WT_SESSION_IMPL *session, WT_ITEM *record, WT_LSN *lsnp,
    uint32_t flags)
{
	/*
	 * If lock_try succeeds: acquire, fill, release.
	 * else slot_join.
	 * if slot leader (offset==0) wait to get lock.
	 * else wait for slot leader
	 * fill
	 * if my_release is last one
	 * else if I'm sync, wait
	 * release buffer
	 * free slot
	 */
	return (0);
}
