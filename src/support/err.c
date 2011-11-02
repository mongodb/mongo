/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_errv --
 * 	Report an error to an event handler.
 */
void
__wt_errv(WT_SESSION_IMPL *session, int error,
    const char *file_name, int line_number, const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;
	const char *prefix1, *prefix2;
	char *end, *p;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	p = s;
	end = s + sizeof(s);

	prefix1 = (session->btree != NULL) ? session->btree->name : NULL;
	prefix2 = session->name;

	if (prefix1 != NULL && prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p),
		    "%s [%s]: ", prefix1, prefix2);
	else if (prefix1 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix1);
	else if (prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix2);
	if (file_name != NULL && p < end)
		p += snprintf(p, (size_t)(end - p),
		    "%s, %d: ", file_name, line_number);
	if (p < end)
		p += vsnprintf(p, (size_t)(end - p), fmt, ap);
	if (error != 0 && p < end)
		p += snprintf(p,
		    (size_t)(end - p), ": %s", wiredtiger_strerror(error));

	handler = session->event_handler;
	handler->handle_error(handler, error, s);
}

/*
 * __wt_err --
 * 	Report an error.
 */
void
__wt_err(WT_SESSION_IMPL *session, int error, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv(session, error, NULL, 0, fmt, ap);
	va_end(ap);
}

/*
 * __wt_errx --
 * 	Report an error with no error code.
 */
void
__wt_errx(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv(session, 0, NULL, 0, fmt, ap);
	va_end(ap);
}

/*
 * __wt_msg_call --
 *	Pass a message to an event handler.
 */
void
__wt_msgv(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	(void)vsnprintf(s, sizeof(s), fmt, ap);

	handler = session->event_handler;
	(void)handler->handle_message(handler, s);
}

/*
 * __wt_msg --
 * 	Report a message.
 */
void
__wt_msg(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_msgv(session, fmt, ap);
	va_end(ap);
}

/*
 * __wt_failure --
 *	Assert and other unexpected failures, includes file/line information
 * for debugging.
 */
int
__wt_failure(WT_SESSION_IMPL *session,
    int error, const char *file_name, int line_number, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 5, 6)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv(session, error, file_name, line_number, fmt, ap);
	va_end(ap);

#ifdef HAVE_DIAGNOSTIC
	__wt_abort(session);
	/* NOTREACHED */
#endif
	return (error);
}

/*
 * __wt_file_format --
 *	Print a standard error message when a file format error is suddenly
 *	discovered.
 */
int
__wt_file_format(WT_SESSION_IMPL *session)
{
	__wt_errx(session, "the file is corrupted; use the Db.salvage"
	    " method or the db_salvage utility to repair the file");
	return (WT_ERROR);
}

/*
 * __wt_file_item_too_big --
 *	Print a standard error message when an element is too large to store.
 */
int
__wt_file_item_too_big(WT_SESSION_IMPL *session)
{
	__wt_errx(session, "the item is too large for the file to store");
	return (WT_ERROR);
}

/*
 * __wt_unknown_object_type --
 *	Print a standard error message when given an unknown object type.
 */
int
__wt_unknown_object_type(WT_SESSION_IMPL *session, const char *uri)
{
	__wt_errx(session, "unknown object type: %s", uri);
	return (EINVAL);
}
