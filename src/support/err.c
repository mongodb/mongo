/*-
 * Copyright (c) 2014-2016 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __handle_error_default --
 *	Default WT_EVENT_HANDLER->handle_error implementation: send to stderr.
 */
static int
__handle_error_default(WT_EVENT_HANDLER *handler,
    WT_SESSION *wt_session, int error, const char *errmsg)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(handler);
	WT_UNUSED(error);

	session = (WT_SESSION_IMPL *)wt_session;

	WT_RET(__wt_fprintf(session, WT_STDERR(session), "%s\n", errmsg));
	WT_RET(__wt_fflush(session, WT_STDERR(session)));
	return (0);
}

/*
 * __handle_message_default --
 *	Default WT_EVENT_HANDLER->handle_message implementation: send to stdout.
 */
static int
__handle_message_default(WT_EVENT_HANDLER *handler,
    WT_SESSION *wt_session, const char *message)
{
	WT_SESSION_IMPL *session;

	WT_UNUSED(handler);

	session = (WT_SESSION_IMPL *)wt_session;
	WT_RET(__wt_fprintf(session, WT_STDOUT(session), "%s\n", message));
	WT_RET(__wt_fflush(session, WT_STDOUT(session)));
	return (0);
}

/*
 * __handle_progress_default --
 *	Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
__handle_progress_default(WT_EVENT_HANDLER *handler,
    WT_SESSION *wt_session, const char *operation, uint64_t progress)
{
	WT_UNUSED(handler);
	WT_UNUSED(wt_session);
	WT_UNUSED(operation);
	WT_UNUSED(progress);

	return (0);
}

/*
 * __handle_close_default --
 *	Default WT_EVENT_HANDLER->handle_close implementation: ignore.
 */
static int
__handle_close_default(WT_EVENT_HANDLER *handler,
    WT_SESSION *wt_session, WT_CURSOR *cursor)
{
	WT_UNUSED(handler);
	WT_UNUSED(wt_session);
	WT_UNUSED(cursor);

	return (0);
}

static WT_EVENT_HANDLER __event_handler_default = {
	__handle_error_default,
	__handle_message_default,
	__handle_progress_default,
	__handle_close_default
};

/*
 * __handler_failure --
 *	Report the failure of an application-configured event handler.
 */
static void
__handler_failure(WT_SESSION_IMPL *session,
    int error, const char *which, bool error_handler_failed)
{
	WT_EVENT_HANDLER *handler;
	WT_SESSION *wt_session;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[256];

	(void)snprintf(s, sizeof(s),
	    "application %s event handler failed: %s",
	    which, __wt_strerror(session, error, NULL, 0));

	/*
	 * Use the error handler to report the failure, unless it was the error
	 * handler that failed.  If it was the error handler that failed, or a
	 * call to the error handler fails, use the default error handler.
	 */
	wt_session = (WT_SESSION *)session;
	handler = session->event_handler;
	if (!error_handler_failed &&
	    handler->handle_error != __handle_error_default &&
	    handler->handle_error(handler, wt_session, error, s) == 0)
		return;

	(void)__handle_error_default(NULL, wt_session, error, s);
}

/*
 * __wt_event_handler_set --
 *	Set an event handler, fill in any NULL methods with the defaults.
 */
void
__wt_event_handler_set(WT_SESSION_IMPL *session, WT_EVENT_HANDLER *handler)
{
	if (handler == NULL)
		handler = &__event_handler_default;
	else {
		if (handler->handle_error == NULL)
			handler->handle_error = __handle_error_default;
		if (handler->handle_message == NULL)
			handler->handle_message = __handle_message_default;
		if (handler->handle_progress == NULL)
			handler->handle_progress = __handle_progress_default;
	}

	session->event_handler = handler;
}

/*
 * __wt_eventv --
 * 	Report a message to an event handler.
 */
int
__wt_eventv(WT_SESSION_IMPL *session, bool msg_event, int error,
    const char *file_name, int line_number, const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;
	WT_DECL_RET;
	WT_SESSION *wt_session;
	struct timespec ts;
	size_t len, remain, wlen;
	int prefix_cnt;
	const char *err, *prefix;
	char *end, *p, tid[128];

	/*
	 * We're using a stack buffer because we want error messages no matter
	 * what, and allocating a WT_ITEM, or the memory it needs, might fail.
	 *
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	/*
	 * !!!
	 * This function MUST handle a NULL WT_SESSION_IMPL handle.
	 *
	 * Without a session, we don't have event handlers or prefixes for the
	 * error message.  Write the error to stderr and call it a day.  (It's
	 * almost impossible for that to happen given how early we allocate the
	 * first session, but if the allocation of the first session fails, for
	 * example, we can end up here without a session.)
	 */
	if (session == NULL) {
		if (fprintf(stderr,
		    "WiredTiger Error%s%s: ",
		    error == 0 ? "" : ": ",
		    error == 0 ? "" :
		    __wt_strerror(session, error, NULL, 0)) < 0)
			ret = EIO;
		if (vfprintf(stderr, fmt, ap) < 0)
			ret = EIO;
		if (fprintf(stderr, "\n") < 0)
			ret = EIO;
		if (fflush(stderr) != 0)
			ret = EIO;
		return (ret);
	}

	p = s;
	end = s + sizeof(s);

	/*
	 * We have several prefixes for the error message: a timestamp and the
	 * process and thread ids, the database error prefix, the data-source's
	 * name, and the session's name.  Write them as a comma-separate list,
	 * followed by a colon.
	 */
	prefix_cnt = 0;
	if (__wt_epoch(session, &ts) == 0) {
		__wt_thread_id(tid, sizeof(tid));
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)snprintf(p, remain,
		    "[%" PRIuMAX ":%" PRIuMAX "][%s]",
		    (uintmax_t)ts.tv_sec,
		    (uintmax_t)ts.tv_nsec / WT_THOUSAND, tid);
		p = wlen >= remain ? end : p + wlen;
		prefix_cnt = 1;
	}
	if ((prefix = S2C(session)->error_prefix) != NULL) {
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)snprintf(p, remain,
		    "%s%s", prefix_cnt == 0 ? "" : ", ", prefix);
		p = wlen >= remain ? end : p + wlen;
		prefix_cnt = 1;
	}
	prefix = session->dhandle == NULL ? NULL : session->dhandle->name;
	if (prefix != NULL) {
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)snprintf(p, remain,
		    "%s%s", prefix_cnt == 0 ? "" : ", ", prefix);
		p = wlen >= remain ? end : p + wlen;
		prefix_cnt = 1;
	}
	if ((prefix = session->name) != NULL) {
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)snprintf(p, remain,
		    "%s%s", prefix_cnt == 0 ? "" : ", ", prefix);
		p = wlen >= remain ? end : p + wlen;
		prefix_cnt = 1;
	}
	if (prefix_cnt != 0) {
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)snprintf(p, remain, ": ");
		p = wlen >= remain ? end : p + wlen;
	}

	if (file_name != NULL) {
		remain = WT_PTRDIFF(end, p);
		wlen = (size_t)
		    snprintf(p, remain, "%s, %d: ", file_name, line_number);
		p = wlen >= remain ? end : p + wlen;
	}

	remain = WT_PTRDIFF(end, p);
	wlen = (size_t)vsnprintf(p, remain, fmt, ap);
	p = wlen >= remain ? end : p + wlen;

	if (error != 0) {
		/*
		 * When the engine calls __wt_err on error, it often outputs an
		 * error message including the string associated with the error
		 * it's returning.  We could change the calls to call __wt_errx,
		 * but it's simpler to not append an error string if all we are
		 * doing is duplicating an existing error string.
		 *
		 * Use strcmp to compare: both strings are nul-terminated, and
		 * we don't want to run past the end of the buffer.
		 */
		err = __wt_strerror(session, error, NULL, 0);
		len = strlen(err);
		if (WT_PTRDIFF(p, s) < len || strcmp(p - len, err) != 0) {
			remain = WT_PTRDIFF(end, p);
			(void)snprintf(p, remain, ": %s", err);
		}
	}

	/*
	 * If a handler fails, return the error status: if we're in the process
	 * of handling an error, any return value we provide will be ignored by
	 * our caller, our caller presumably already has an error value it will
	 * be returning.
	 *
	 * If an application-specified or default informational message handler
	 * fails, complain using the application-specified or default error
	 * handler.
	 *
	 * If an application-specified error message handler fails, complain
	 * using the default error handler.  If the default error handler fails,
	 * there's nothing to do.
	 */
	wt_session = (WT_SESSION *)session;
	handler = session->event_handler;
	if (msg_event) {
		ret = handler->handle_message(handler, wt_session, s);
		if (ret != 0)
			__handler_failure(session, ret, "message", false);
	} else {
		ret = handler->handle_error(handler, wt_session, error, s);
		if (ret != 0 && handler->handle_error != __handle_error_default)
			__handler_failure(session, ret, "error", true);
	}

	return (ret);
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

	/*
	 * Ignore error returns from underlying event handlers, we already have
	 * an error value to return.
	 */
	va_start(ap, fmt);
	(void)__wt_eventv(session, false, error, NULL, 0, fmt, ap);
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

	/*
	 * Ignore error returns from underlying event handlers, we already have
	 * an error value to return.
	 */
	va_start(ap, fmt);
	(void)__wt_eventv(session, false, 0, NULL, 0, fmt, ap);
	va_end(ap);
}

/*
 * __wt_ext_err_printf --
 *	Extension API call to print to the error stream.
 */
int
__wt_ext_err_printf(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = __wt_eventv(session, false, 0, NULL, 0, fmt, ap);
	va_end(ap);
	return (ret);
}

/*
 * info_msg --
 * 	Informational message.
 */
static int
info_msg(WT_SESSION_IMPL *session, const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;
	WT_SESSION *wt_session;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	(void)vsnprintf(s, sizeof(s), fmt, ap);

	wt_session = (WT_SESSION *)session;
	handler = session->event_handler;
	return (handler->handle_message(handler, wt_session, s));
}

/*
 * __wt_msg --
 * 	Informational message.
 */
int
__wt_msg(WT_SESSION_IMPL *session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 2, 3)))
{
	WT_DECL_RET;
	va_list ap;

	va_start(ap, fmt);
	ret = info_msg(session, fmt, ap);
	va_end(ap);

	return (ret);
}

/*
 * __wt_ext_msg_printf --
 *	Extension API call to print to the message stream.
 */
int
__wt_ext_msg_printf(
    WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 3, 4)))
{
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	va_list ap;

	if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
		session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	va_start(ap, fmt);
	ret = info_msg(session, fmt, ap);
	va_end(ap);
	return (ret);
}

/*
 * __wt_ext_strerror --
 *	Extension API call to return an error as a string.
 */
const char *
__wt_ext_strerror(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, int error)
{
	if (wt_session == NULL)
		wt_session = (WT_SESSION *)
		    ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

	return (wt_session->strerror(wt_session, error));
}

/*
 * __wt_progress --
 *	Progress message.
 */
int
__wt_progress(WT_SESSION_IMPL *session, const char *s, uint64_t v)
{
	WT_DECL_RET;
	WT_EVENT_HANDLER *handler;
	WT_SESSION *wt_session;

	wt_session = (WT_SESSION *)session;
	handler = session->event_handler;
	if (handler != NULL && handler->handle_progress != NULL)
		if ((ret = handler->handle_progress(handler,
		    wt_session, s == NULL ? session->name : s, v)) != 0)
			__handler_failure(session, ret, "progress", false);
	return (0);
}

/*
 * __wt_assert --
 *	Assert and other unexpected failures, includes file/line information
 * for debugging.
 */
void
__wt_assert(WT_SESSION_IMPL *session,
    int error, const char *file_name, int line_number, const char *fmt, ...)
    WT_GCC_FUNC_ATTRIBUTE((format (printf, 5, 6)))
#ifdef HAVE_DIAGNOSTIC
    WT_GCC_FUNC_ATTRIBUTE((noreturn))
#endif
{
	va_list ap;

	va_start(ap, fmt);
	(void)__wt_eventv(
	    session, false, error, file_name, line_number, fmt, ap);
	va_end(ap);

#ifdef HAVE_DIAGNOSTIC
	__wt_abort(session);			/* Drop core if testing. */
	/* NOTREACHED */
#endif
}

/*
 * __wt_panic --
 *	A standard error message when we panic.
 */
int
__wt_panic(WT_SESSION_IMPL *session)
{
	F_SET(S2C(session), WT_CONN_PANIC);
	__wt_err(session, WT_PANIC, "the process must exit and restart");

#if defined(HAVE_DIAGNOSTIC)
	__wt_abort(session);			/* Drop core if testing. */
	/* NOTREACHED */
#else
	/*
	 * Chaos reigns within.
	 * Reflect, repent, and reboot.
	 * Order shall return.
	 */
	return (WT_PANIC);
#endif
}

/*
 * __wt_illegal_value --
 *	A standard error message when we detect an illegal value.
 */
int
__wt_illegal_value(WT_SESSION_IMPL *session, const char *name)
{
	__wt_errx(session, "%s%s%s",
	    name == NULL ? "" : name, name == NULL ? "" : ": ",
	    "encountered an illegal file format or internal value");

#if defined(HAVE_DIAGNOSTIC)
	__wt_abort(session);			/* Drop core if testing. */
	/* NOTREACHED */
#else
	return (__wt_panic(session));
#endif
}

/*
 * __wt_object_unsupported --
 *	Print a standard error message for an object that doesn't support a
 * particular operation.
 */
int
__wt_object_unsupported(WT_SESSION_IMPL *session, const char *uri)
{
	WT_RET_MSG(session, ENOTSUP, "unsupported object operation: %s", uri);
}

/*
 * __wt_bad_object_type --
 *	Print a standard error message when given an unknown or unsupported
 * object type.
 */
int
__wt_bad_object_type(WT_SESSION_IMPL *session, const char *uri)
{
	if (WT_PREFIX_MATCH(uri, "backup:") ||
	    WT_PREFIX_MATCH(uri, "colgroup:") ||
	    WT_PREFIX_MATCH(uri, "config:") ||
	    WT_PREFIX_MATCH(uri, "file:") ||
	    WT_PREFIX_MATCH(uri, "index:") ||
	    WT_PREFIX_MATCH(uri, "log:") ||
	    WT_PREFIX_MATCH(uri, "lsm:") ||
	    WT_PREFIX_MATCH(uri, "statistics:") ||
	    WT_PREFIX_MATCH(uri, "table:"))
		return (__wt_object_unsupported(session, uri));

	WT_RET_MSG(session, ENOTSUP, "unknown object type: %s", uri);
}
