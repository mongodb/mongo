/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__log_record_size(WT_SESSION_IMPL *session,
    WT_LOGREC_DESC *recdesc, va_list ap, size_t *sizep)
{
	WT_UNUSED(session);

	*sizep = wiredtiger_struct_size(recdesc->fmt, ap);
	return (0);
}

int
__wt_log_put(WT_SESSION_IMPL *session, WT_LOGREC_DESC *recdesc, ...)
{
	WT_BUF *buf;
	va_list ap;
	size_t size;
	int ret;

	buf = &session->logrec_buf;

	va_start(ap, recdesc);
	WT_ERR(__log_record_size(session, recdesc, ap, &size));
	va_end(ap);

	WT_RET(__wt_buf_initsize(session, buf, size));

	va_start(ap, recdesc);
	WT_ERR(__wt_struct_packv(session, buf->mem, size, recdesc->fmt, ap));
err:	va_end(ap);

	return (ret);
}

int
__wt_log_vprintf(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_CONNECTION_IMPL *conn;
	WT_BUF *buf;
	va_list ap_copy;
	size_t len;

	conn = S2C(session);

	if (conn->log_fh == NULL)
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
#if 1
	strcpy((char *)buf->mem + len - 2, "\n");
	return ((write(conn->log_fh->fd, buf->mem, len - 1) ==
	    (ssize_t)len - 1) ? 0 : WT_ERROR);
#else
	return (__wt_logput_debug(session, (char *)buf->mem));
#endif
}

int
__wt_log_printf(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_ATTRIBUTE ((format (printf, 2, 3)))
{
	va_list ap;
	int ret;

	va_start(ap, fmt);
	ret = __wt_log_vprintf(session, fmt, ap);
	va_end(ap);

	return (ret);
}
