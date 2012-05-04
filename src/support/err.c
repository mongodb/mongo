/*-
 * Copyright (c) 2008-2012 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __handle_error_default --
 *	Default WT_EVENT_HANDLER->handle_error implementation: send to stderr.
 */
static void
__handle_error_default(int error, const char *errmsg)
{
	size_t len_err, len_errmsg;
	const char *err;

	if (error != 0) {
		err = wiredtiger_strerror(error);
		len_err = strlen(err);
		len_errmsg = strlen(errmsg);
		if (len_err >= len_errmsg &&
		    strcmp(errmsg + (len_errmsg - len_err), err) != 0) {
			(void)fprintf(stderr,
			    "%s: %s\n", errmsg, wiredtiger_strerror(error));
		}
	}
	(void)fprintf(stderr, "%s\n", errmsg);
}

/*
 * __handle_message_default --
 *	Default WT_EVENT_HANDLER->handle_message implementation: send to stdout.
 */
static void
__handle_message_default(const char *message)
{
	(void)printf("%s\n", message);
}

/*
 * __wt_eventv --
 * 	Report a message to an event handler.
 */
void
__wt_eventv(WT_SESSION_IMPL *session, int msg_event,
    int error,
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

	handler = &session->event_handler;
	if (msg_event) {
		if (handler->handle_message == NULL ||
		    handler->handle_message(handler, s))
			__handle_message_default(s);
	} else
		if (handler->handle_error == NULL ||
		    handler->handle_error(handler, error, s))
			__handle_error_default(error, s);
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
	__wt_eventv(session, 0, error, NULL, 0, fmt, ap);
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
	__wt_eventv(session, 0, 0, NULL, 0, fmt, ap);
	va_end(ap);
}

/*
 * __wt_msgv --
 * 	Informational message.
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

	handler = &session->event_handler;
	if (handler->handle_message == NULL ||
	    handler->handle_message(handler, s))
		__handle_message_default(s);
}

/*
 * __wt_msg --
 * 	Informational message.
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
 * __wt_progress --
 *	Progress message.
 */
void
__wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v)
{
	WT_EVENT_HANDLER *handler;

	handler = &session->event_handler;
	if (handler->handle_progress != NULL)
		handler->handle_progress(
		    handler, s == NULL ? session->name : s, v);
}

/*
 * __wt_verbose --
 * 	Verbose message.
 */
void
__wt_verbose(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_eventv(session, 1, 0, NULL, 0, fmt, ap);
	va_end(ap);
}

/*
 * __wt_assert --
 *	Assert and other unexpected failures, includes file/line information
 * for debugging.
 */
int
__wt_assert(WT_SESSION_IMPL *session,
    int error, const char *file_name, int line_number, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 5, 6)))
{
	va_list ap;

	va_start(ap, fmt);
	__wt_eventv(session, 0, error, file_name, line_number, fmt, ap);
	va_end(ap);

#ifdef HAVE_DIAGNOSTIC
	__wt_abort(session);
	/* NOTREACHED */
#endif
	return (error);
}

/*
 * __wt_illegal_value --
 *	Print a standard error message when we detect an illegal value.
 */
int
__wt_illegal_value(WT_SESSION_IMPL *session, const char *name)
{
	WT_RET_MSG(session, WT_ERROR,
	    "%s%s"
	    "encountered an illegal file format or internal value; restart "
	    "the system and verify the underlying files, if corruption is "
	    "detected use the WT_SESSION salvage method or the wt utility's "
	    "salvage command to repair the file",
	    name == NULL ? "" : name, name == NULL ? "" : " ");
}

/*
 * __wt_unknown_object_type --
 *	Print a standard error message when given an unknown object type.
 */
int
__wt_unknown_object_type(WT_SESSION_IMPL *session, const char *uri)
{
	WT_RET_MSG(session, EINVAL,
	    "unknown or unsupported object type: %s", uri);
}
