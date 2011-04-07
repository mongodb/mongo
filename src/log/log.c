/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

static int
__log_record_size(SESSION *session,
    WT_LOGREC_DESC *recdesc, va_list ap, size_t *sizep)
{
	WT_UNUSED(session);

	*sizep = wiredtiger_struct_size(recdesc->fmt, ap);
	return (0);
}

int
__wt_log_put(SESSION *session, WT_LOGREC_DESC *recdesc, ...)
{
	WT_BUF *buf;
	va_list ap;
	size_t size;
	int ret;

	buf = &session->logrec_buf;

	va_start(ap, recdesc);
	WT_ERR(__log_record_size(session, recdesc, ap, &size));
	va_end(ap);

	WT_RET(__wt_buf_setsize(session, buf, size));

	va_start(ap, recdesc);
	WT_ERR(wiredtiger_struct_packv(buf->mem, size, recdesc->fmt, ap));
err:	va_end(ap);

	return (ret);
}

int
__wt_log_printf(SESSION *session, const char *fmt, ...)
    WT_GCC_ATTRIBUTE ((format (printf, 2, 3)))
{
	CONNECTION *conn;
	WT_BUF *buf;
	va_list ap;
	size_t len;

	conn = S2C(session);

	if (conn->log_fh == NULL)
		return (0);

	buf = &session->logprint_buf;

	va_start(ap, fmt);
	len = vsnprintf(NULL, 0, fmt, ap) + 2;
	va_end(ap);

	WT_RET(__wt_buf_setsize(session, buf, len));

	va_start(ap, fmt);
	(void)vsnprintf(buf->mem, len, fmt, ap);
	va_end(ap);

	/*
	 * For now, just dump the text into the file.  Later, we will use
	 * __wt_logput_debug to wrap this in a log header.
	 */
#if 1
	strcpy((char *)buf->mem + len - 2, "\n");
	return ((write(conn->log_fh->fd, buf->mem, len - 1) ==
	    (ssize_t)len - 1) ?  0 : WT_ERROR);
#else
	return (__wt_logput_debug(session, (char *)buf->mem));
#endif
}
