/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * __wt_msg_call --
 *	Pass a message to a callback function.
 */
void
__wt_msgv(SESSION *session, const char *prefix1, const char *prefix2,
    const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;
	char *end, *p;
	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	p = s;
	end = s + sizeof(s);

	if (prefix1 != NULL && prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p),
		    "%s [%s]: ", prefix1, prefix2);
	else if (prefix1 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix1);
	else if (prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix2);
	if (p < end)
		p += vsnprintf(p, (size_t)(end - p), fmt, ap);

	handler = session->event_handler;
	handler->handle_message(handler, s);
}

/*
 * __wt_msg --
 * 	Report a message.
 */
void
__wt_msg(SESSION *session, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__wt_msgv(session,
	    (session->btree != NULL) ? session->btree->name : NULL,
	    session->name,
	    fmt, ap);
	va_end(ap);
}

#ifdef HAVE_DIAGNOSTIC
/*
 * __wt_assert --
 *	Internal version of assert function.
 */
void
__wt_assert(
    SESSION *session, const char *check, const char *file_name, int line_number)
{
	__wt_errx(session,
	    "assertion failure: %s/%d: \"%s\"", file_name, line_number, check);

	__wt_abort(session);
	/* NOTREACHED */
}
#endif

/*
 * __wt_api_args --
 *	Print a standard error message when an API function is passed illegal
 *	arguments.
 */
int
__wt_api_args(SESSION *session, const char *name)
{
	__wt_errx(session,
	    "%s: illegal API arguments or flag values specified", name);
	return (WT_ERROR);
}

/*
 * __wt_api_arg_min --
 *	Print a standard error message when an API function is passed a
 *	too-small argument.
 */
int
__wt_api_arg_min(SESSION *session,
    const char *name, const char *arg_name, uint32_t v, uint32_t min)
{
	if (v >= min)
		return (0);

	__wt_errx(session,
	    "%s: %s argument less than minimum value of %lu",
	    name, arg_name, (u_long)min);
	return (WT_ERROR);
}

/*
 * __wt_api_arg_max --
 *	Print a standard error message when an API function is passed a
 *	too-large argument.
 */
int
__wt_api_arg_max(SESSION *session,
    const char *name, const char *arg_name, uint32_t v, uint32_t max)
{
	if (v <= max)
		return (0);

	__wt_errx(session,
	    "%s: %s argument larger than maximum value of %lu",
	    name, arg_name, (u_long)max);
	return (WT_ERROR);
}

/*
 * __wt_file_method_type --
 *	Print a standard error message on attempts to call methods inappropriate
 *	for a file type.
 */
int
__wt_file_method_type(SESSION *session, const char *name, int column_err)
{
	__wt_errx(session,
	    "%s: this method is not supported for a %s file",
	    name, column_err ? "column-store" : "row-store");
	return (WT_ERROR);
}

/*
 * __wt_file_wrong_fixed_size --
 *	Print a standard error message on attempts to put the wrong size element
 *	into a fixed-size file.
 */
int
__wt_file_wrong_fixed_size(SESSION *session, uint32_t len, uint32_t config_len)
{
	__wt_errx(session,
	    "%s: length of %lu does not match fixed-length file configuration "
	    "of %lu",
	    session->name, (u_long)len, (u_long)config_len);
	return (WT_ERROR);
}

/*
 * __wt_file_readonly --
 *	Print a standard error message on attempts to modify a read-only file.
 */
int
__wt_file_readonly(SESSION *session, const char *name)
{
	__wt_errx(session,
	    "%s: the file was opened read-only and may not be modified", name);
	return (WT_READONLY);
}

/*
 * __wt_file_format --
 *	Print a standard error message when a file format error is suddenly
 *	discovered.
 */
int
__wt_file_format(SESSION *session)
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
__wt_file_item_too_big(SESSION *session)
{
	__wt_errx(session, "the item is too large for the file to store");
	return (WT_ERROR);
}

/*
 * __wt_session_lockout --
 *	Standard SESSION handle lockout error message.
 */
int
__wt_session_lockout(SESSION *session)
{
	__wt_errx(session,
	    "An unavailable handle method was called; the handle method is "
	    "not available for some reason, for example, handle methods are "
	    "restricted after an error, or configuration methods may be "
	    "restricted after the file or environment have been opened, "
	    "or operational methods may be restricted until the file or "
	    "environment has been opened.");
	return (WT_ERROR);
}

/*
 * __wt_btree_lockout --
 *	Standard BTREE handle lockout error message.
 */
int
__wt_btree_lockout(BTREE *btree)
{
	return (__wt_connection_lockout(btree->conn));
}

/*
 * __wt_connection_lockout --
 *	Standard CONNECTION handle lockout error message.
 */
int
__wt_connection_lockout(CONNECTION *conn)
{
	return (__wt_session_lockout(&conn->default_session));
}

/*
 * __wt_errv --
 * 	Report an error (va_list version).
 */
void
__wt_errv(SESSION *session, int error,
    const char *prefix1, const char *prefix2, const char *fmt, va_list ap)
{
	WT_EVENT_HANDLER *handler;
	char *end, *p;

	/*
	 * !!!
	 * SECURITY:
	 * Buffer placed at the end of the stack in case snprintf overflows.
	 */
	char s[2048];

	p = s;
	end = s + sizeof(s);

	if (prefix1 != NULL && prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p),
		    "%s [%s]: ", prefix1, prefix2);
	else if (prefix1 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix1);
	else if (prefix2 != NULL && p < end)
		p += snprintf(p, (size_t)(end - p), "%s: ", prefix2);
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
__wt_err(SESSION *session, int error, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv(session, error,
	    (session->btree != NULL) ? session->btree->name : NULL,
	    session->name,
	    fmt, ap);
	va_end(ap);
}

/*
 * __wt_errx --
 * 	Report an error with no error code.
 */
void
__wt_errx(SESSION *session, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	__wt_errv(session, 0,
	    (session->btree != NULL) ? session->btree->name : NULL,
	    session->name,
	    fmt, ap);
	va_end(ap);
}
