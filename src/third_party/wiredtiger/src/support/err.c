/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __handle_error_default --
 *     Default WT_EVENT_HANDLER->handle_error implementation: send to stderr.
 */
static int
__handle_error_default(
  WT_EVENT_HANDLER *handler, WT_SESSION *wt_session, int error, const char *errmsg)
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
 *     Default WT_EVENT_HANDLER->handle_message implementation: send to stdout.
 */
static int
__handle_message_default(WT_EVENT_HANDLER *handler, WT_SESSION *wt_session, const char *message)
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
 *     Default WT_EVENT_HANDLER->handle_progress implementation: ignore.
 */
static int
__handle_progress_default(
  WT_EVENT_HANDLER *handler, WT_SESSION *wt_session, const char *operation, uint64_t progress)
{
    WT_UNUSED(handler);
    WT_UNUSED(wt_session);
    WT_UNUSED(operation);
    WT_UNUSED(progress);

    return (0);
}

/*
 * __handle_close_default --
 *     Default WT_EVENT_HANDLER->handle_close implementation: ignore.
 */
static int
__handle_close_default(WT_EVENT_HANDLER *handler, WT_SESSION *wt_session, WT_CURSOR *cursor)
{
    WT_UNUSED(handler);
    WT_UNUSED(wt_session);
    WT_UNUSED(cursor);

    return (0);
}

static WT_EVENT_HANDLER __event_handler_default = {__handle_error_default, __handle_message_default,
  __handle_progress_default, __handle_close_default};

/*
 * __handler_failure --
 *     Report the failure of an application-configured event handler.
 */
static void
__handler_failure(WT_SESSION_IMPL *session, int error, const char *which, bool error_handler_failed)
{
    WT_EVENT_HANDLER *handler;
    WT_SESSION *wt_session;

    /*
     * !!!
     * SECURITY:
     * Buffer placed at the end of the stack in case snprintf overflows.
     */
    char s[256];

    if (__wt_snprintf(s, sizeof(s), "application %s event handler failed: %s", which,
          __wt_strerror(session, error, NULL, 0)) != 0)
        return;

    /*
     * Use the error handler to report the failure, unless it was the error handler that failed. If
     * it was the error handler that failed, or a call to the error handler fails, use the default
     * error handler.
     */
    wt_session = (WT_SESSION *)session;
    handler = session->event_handler;
    if (!error_handler_failed && handler->handle_error != __handle_error_default &&
      handler->handle_error(handler, wt_session, error, s) == 0)
        return;

    /*
     * In case there is a failure in the default error handler, make sure we don't recursively try
     * to report *that* error.
     */
    session->event_handler = &__event_handler_default;
    (void)__handle_error_default(NULL, wt_session, error, s);
    session->event_handler = handler;
}

/*
 * __wt_event_handler_set --
 *     Set an event handler, fill in any NULL methods with the defaults.
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
        if (handler->handle_close == NULL)
            handler->handle_close = __handle_close_default;
    }

    session->event_handler = handler;
}

#define WT_ERROR_APPEND(p, remain, ...)                                \
    do {                                                               \
        size_t __len;                                                  \
        WT_ERR(__wt_snprintf_len_set(p, remain, &__len, __VA_ARGS__)); \
        if (__len > remain)                                            \
            __len = remain;                                            \
        p += __len;                                                    \
        remain -= __len;                                               \
    } while (0)
#define WT_ERROR_APPEND_AP(p, remain, ...)                              \
    do {                                                                \
        size_t __len;                                                   \
        WT_ERR(__wt_vsnprintf_len_set(p, remain, &__len, __VA_ARGS__)); \
        if (__len > remain)                                             \
            __len = remain;                                             \
        p += __len;                                                     \
        remain -= __len;                                                \
    } while (0)

/*
 * __eventv_unpack_json_str --
 *     Unpack a string into JSON escaped format. Can be called with null destination for sizing.
 */
static size_t
__eventv_unpack_json_str(u_char *dest, size_t dest_len, char *src, size_t src_len)
  WT_GCC_FUNC_ATTRIBUTE((cold))
{
    size_t len, n, nchars;
    u_char *q;
    const char *p;

    nchars = 0;
    q = dest;

    for (p = src, len = src_len; len > 0; p++, --len) {
        n = __wt_json_unpack_char((u_char)*p, q, dest_len, false);
        nchars += n;
        if (q != NULL) {
            dest_len -= n;
            q += n;
        }
    }

    if (q != NULL)
        *q = '\0';

    return (nchars);
}

/*
 * __eventv_gen_msg --
 *     Generate a formatted message.
 */
static int
__eventv_gen_msg(WT_SESSION_IMPL *session, char *buffer, size_t *remain, bool is_json, int error,
  const char *func, int line, WT_VERBOSE_CATEGORY category, WT_VERBOSE_LEVEL level, const char *msg)
  WT_GCC_FUNC_ATTRIBUTE((cold))
{
    struct timespec ts;
    WT_DECL_RET;
    size_t len, msg_len, remain_msg, unpacked_msg_len;
    u_char *unpacked_json_str;
    char msg_str[2 * 1024], *p, *p_msg, tid[128];
    const char *err, *prefix, *verbosity_level_tag;

    p = buffer;
    p_msg = msg_str;
    unpacked_json_str = NULL;

    remain_msg = sizeof(msg_str);

    if (is_json)
        WT_ERROR_APPEND(p, *remain, "{");

    /* Timestamp and thread id. */
    __wt_epoch(session, &ts);
    WT_ERR(__wt_thread_str(tid, sizeof(tid)));
    if (is_json) {
        WT_ERROR_APPEND(p, *remain, "\"ts_sec\":%" PRIuMAX ",", (uintmax_t)ts.tv_sec);
        WT_ERROR_APPEND(
          p, *remain, "\"ts_usec\":%" PRIuMAX ",", (uintmax_t)ts.tv_nsec / WT_THOUSAND);
        WT_ERROR_APPEND(p, *remain, "\"thread\":\"%s\",", tid);
    } else
        WT_ERROR_APPEND(p, *remain, "[%" PRIuMAX ":%" PRIuMAX "][%s]", (uintmax_t)ts.tv_sec,
          (uintmax_t)ts.tv_nsec / WT_THOUSAND, tid);

    /* Error prefix. */
    if ((prefix = S2C(session)->error_prefix) != NULL) {
        if (is_json)
            WT_ERROR_APPEND(p, *remain, "\"session_err_prefix\":\"%s\",", prefix);
        else
            WT_ERROR_APPEND(p, *remain, ", %s", prefix);
    }

    /* Session dhandle name. */
    prefix = session->dhandle == NULL ? NULL : session->dhandle->name;
    if (prefix != NULL) {
        if (is_json)
            WT_ERROR_APPEND(p, *remain, "\"session_dhandle_name\":\"%s\",", prefix);
        else
            WT_ERROR_APPEND(p, *remain, ", %s", prefix);
    }

    /* Session name. */
    if ((prefix = session->name) != NULL) {
        if (is_json)
            WT_ERROR_APPEND(p, *remain, "\"session_name\":\"%s\",", prefix);
        else
            WT_ERROR_APPEND(p, *remain, ", %s", prefix);
    }

    if (is_json) {
        /* Category and verbosity level. */
        WT_ERROR_APPEND(p, *remain, "\"category\":\"%s\",", WT_VERBOSE_CATEGORY_STR(category));
        WT_ERROR_APPEND(p, *remain, "\"category_id\":%" PRIu32 ",", category);
        WT_VERBOSE_LEVEL_STR(level, verbosity_level_tag);
        WT_ERROR_APPEND(p, *remain, "\"verbose_level\":\"%s\",", verbosity_level_tag);
        WT_ERROR_APPEND(p, *remain, "\"verbose_level_id\":%d,", level);

        /* Format the content of the message into an intermediate buffer. */
        WT_ERROR_APPEND(p_msg, remain_msg, "%s", msg);

        /* Escape any characters that are special for JSON. */
        msg_len = sizeof(msg_str) - remain_msg;
        unpacked_msg_len = __eventv_unpack_json_str(NULL, 0, msg_str, msg_len);
        WT_ERR(__wt_malloc(session, unpacked_msg_len + 1, &unpacked_json_str));
        WT_UNUSED(__eventv_unpack_json_str(unpacked_json_str, unpacked_msg_len, msg_str, msg_len));

        /* Message. */
        WT_ERROR_APPEND(p, *remain, "\"msg\":\"");
        if (func != NULL)
            WT_ERROR_APPEND(p, *remain, "%s:%d:", func, line);
        WT_ERROR_APPEND(p, *remain, "%s", unpacked_json_str);
        WT_ERROR_APPEND(p, *remain, "\"");
    } else {
        WT_ERROR_APPEND(p, *remain, ": ");
        if (func != NULL)
            WT_ERROR_APPEND(p, *remain, "%s, %d: ", func, line);

        /* Category and verbosity level. */
        WT_VERBOSE_LEVEL_STR(level, verbosity_level_tag);
        WT_ERROR_APPEND(
          p, *remain, "[%s][%s]", WT_VERBOSE_CATEGORY_STR(category), verbosity_level_tag);

        /* Message. */
        WT_ERROR_APPEND(p, *remain, ": %s", msg);
    }

    /* Error message. */
    if (error != 0) {
        /*
         * When the engine calls __wt_err on error, it often outputs an error message including the
         * string associated with the error it's returning. We could change the calls to call
         * __wt_errx, but it's simpler to not append an error string if all we are doing is
         * duplicating an existing error string.
         *
         * Use strcmp to compare: both strings are nul-terminated, and we don't want to run past the
         * end of the buffer.
         */
        err = __wt_strerror(session, error, NULL, 0);
        if (is_json) {
            WT_ERROR_APPEND(p, *remain, ",");
            WT_ERROR_APPEND(p, *remain, "\"error_str\":\"%s\",", err);
            WT_ERROR_APPEND(p, *remain, "\"error_code\":%d", error);
        } else {
            len = strlen(err);
            if (WT_PTRDIFF(p, buffer) < len || strcmp(p - len, err) != 0)
                WT_ERROR_APPEND(p, *remain, ": %s", err);
        }
    }

    if (is_json)
        WT_ERROR_APPEND(p, *remain, "}");

err:
    __wt_free(session, unpacked_json_str);
    return (ret);
}

/*
 * __eventv --
 *     Report a message to an event handler.
 */
static int
__eventv(WT_SESSION_IMPL *session, bool is_json, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, WT_VERBOSE_LEVEL level, const char *fmt, va_list ap)
  WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_DECL_RET;
    WT_EVENT_HANDLER *handler;
    WT_SESSION *wt_session;
    size_t remain, remain_msg;
    char *p;

    /*
     * We're using a stack buffer because we want error messages no matter
     * what, and allocating a WT_ITEM, or the memory it needs, might fail.
     *
     * !!!
     * SECURITY:
     * Buffer placed at the end of the stack in case snprintf overflows.
     */
    char msg[4 * 1024], s[4 * 1024];
    p = msg;
    remain = sizeof(s);
    remain_msg = sizeof(msg);

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
    if (session == NULL)
        goto err;

    /* Format the message. */
    WT_ERROR_APPEND_AP(p, remain_msg, fmt, ap);
    WT_ERR(__eventv_gen_msg(session, s, &remain, is_json, error, func, line, category, level, msg));

    /*
     * If a handler fails, return the error status: if we're in the process of handling an error,
     * any return value we provide will be ignored by our caller, our caller presumably already has
     * an error value it will be returning.
     *
     * If an application-specified or default informational message handler fails, complain using
     * the application-specified or default error handler.
     *
     * If an application-specified error message handler fails, complain using the default error
     * handler. If the default error handler fails, fallback to stderr.
     */
    wt_session = (WT_SESSION *)session;
    handler = session->event_handler;
    if (level != WT_VERBOSE_ERROR) {
        ret = handler->handle_message(handler, wt_session, s);
        if (ret != 0)
            __handler_failure(session, ret, "message", false);
    } else {
        ret = handler->handle_error(handler, wt_session, error, s);
        if (ret != 0 && handler->handle_error != __handle_error_default)
            __handler_failure(session, ret, "error", true);
    }

    /*
     * The buffer is fixed sized, complain if we overflow. (The test is for no more bytes remaining
     * in the buffer, so technically we might have filled it exactly.) Be cautious changing this
     * code, it's a recursive call.
     */
    if (ret == 0 && remain == 0)
        __wt_err(
          session, ENOMEM, "error or message truncated: internal WiredTiger buffer too small");

    if (ret != 0) {
err:
        if (fprintf(stderr, "WiredTiger Error%s%s: ", error == 0 ? "" : ": ",
              error == 0 ? "" : __wt_strerror(session, error, NULL, 0)) < 0)
            WT_TRET(EIO);
        if (vfprintf(stderr, fmt, ap) < 0)
            WT_TRET(EIO);
        if (fprintf(stderr, "\n") < 0)
            WT_TRET(EIO);
        if (fflush(stderr) != 0)
            WT_TRET(EIO);
    }

    return (ret);
}

/*
 * __wt_err_func --
 *     Report an error.
 */
void
__wt_err_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((cold))
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 6, 7))) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    va_list ap;

    /*
     * Ignore error returns from underlying event handlers, we already have an error value to
     * return.
     */
    va_start(ap, fmt);
    WT_IGNORE_RET(__eventv(session,
      session ? FLD_ISSET(S2C(session)->json_output, WT_JSON_OUTPUT_ERROR) : false, error, func,
      line, category, WT_VERBOSE_ERROR, fmt, ap));
    va_end(ap);
}

/*
 * __wt_errx_func --
 *     Report an error with no error code.
 */
void
__wt_errx_func(WT_SESSION_IMPL *session, const char *func, int line, WT_VERBOSE_CATEGORY category,
  const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((cold)) WT_GCC_FUNC_ATTRIBUTE((format(printf, 5, 6)))
  WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    va_list ap;

    /*
     * Ignore error returns from underlying event handlers, we already have an error value to
     * return.
     */
    va_start(ap, fmt);
    WT_IGNORE_RET(__eventv(session,
      session ? FLD_ISSET(S2C(session)->json_output, WT_JSON_OUTPUT_ERROR) : false, 0, func, line,
      category, WT_VERBOSE_ERROR, fmt, ap));
    va_end(ap);
}

/*
 * __wt_panic_func --
 *     A standard error message when we panic.
 */
int
__wt_panic_func(WT_SESSION_IMPL *session, int error, const char *func, int line,
  WT_VERBOSE_CATEGORY category, const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((cold))
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 6, 7))) WT_GCC_FUNC_ATTRIBUTE((visibility("default")))
{
    WT_CONNECTION_IMPL *conn;
    va_list ap;

    conn = S2C(session);

    /*
     * Ignore error returns from underlying event handlers, we already have an error value to
     * return.
     */
    va_start(ap, fmt);
    WT_IGNORE_RET(
      __eventv(session, session ? FLD_ISSET(conn->json_output, WT_JSON_OUTPUT_ERROR) : false, error,
        func, line, category, WT_VERBOSE_ERROR, fmt, ap));
    va_end(ap);

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     *
     * If the connection has already panicked, just return the error.
     */
    if (session != NULL && F_ISSET(conn, WT_CONN_PANIC))
        return (WT_PANIC);

    /*
     * Call the error callback function before setting the connection's panic flag, so applications
     * can trace the failing thread before being flooded with panic returns from API calls. Using
     * the variable-arguments list from the current call even thought the format doesn't need it as
     * I'm not confident of underlying support for a NULL.
     */
    va_start(ap, fmt);
    WT_IGNORE_RET(__eventv(session, FLD_ISSET(conn->json_output, WT_JSON_OUTPUT_ERROR), WT_PANIC,
      func, line, category, WT_VERBOSE_ERROR, "the process must exit and restart", ap));
    va_end(ap);

#if defined(HAVE_DIAGNOSTIC)
    /*
     * In the diagnostic builds, we want to drop core in case of panics that are not due to data
     * corruption. A core could be useful in debugging.
     *
     * In the case of corruption, we want to be able to test the application's capability to salvage
     * by returning an error code. But we do not want to lose the ability to drop core if required.
     * Hence in the diagnostic mode, the application can set the debug flag to choose between
     * dropping a core and returning an error.
     */
    if (!F_ISSET(conn, WT_CONN_DATA_CORRUPTION) ||
      FLD_ISSET(conn->debug_flags, WT_CONN_DEBUG_CORRUPTION_ABORT))
        __wt_abort(session);
#endif
    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     *
     * Panic the connection;
     */
    if (session != NULL)
        F_SET(conn, WT_CONN_PANIC);

    /*
     * !!!
     * Chaos reigns within.
     * Reflect, repent, and reboot.
     * Order shall return.
     */
    return (WT_PANIC);
}

/*
 * __wt_set_return_func --
 *     Conditionally log the source of an error code and return the error.
 */
int
__wt_set_return_func(WT_SESSION_IMPL *session, const char *func, int line, int err)
{
    __wt_verbose(session, WT_VERB_ERROR_RETURNS, "%s: %d Error: %d", func, line, err);
    return (err);
}

/*
 * __wt_ext_err_printf --
 *     Extension API call to print to the error stream.
 */
int
__wt_ext_err_printf(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 3, 4)))
{
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    va_list ap;

    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    va_start(ap, fmt);
    ret = __eventv(session,
      session ? FLD_ISSET(S2C(session)->json_output, WT_JSON_OUTPUT_ERROR) : false, 0, NULL, 0,
      WT_VERB_EXTENSION, WT_VERBOSE_ERROR, fmt, ap);
    va_end(ap);
    return (ret);
}

/*
 * __wt_verbose_worker --
 *     Verbose message.
 */
void
__wt_verbose_worker(WT_SESSION_IMPL *session, WT_VERBOSE_CATEGORY category, WT_VERBOSE_LEVEL level,
  const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((format(printf, 4, 5))) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    va_list ap;

    va_start(ap, fmt);
    WT_IGNORE_RET(__eventv(session,
      session ? FLD_ISSET(S2C(session)->json_output, WT_JSON_OUTPUT_MESSAGE) : false, 0, NULL, 0,
      category, level, fmt, ap));
    va_end(ap);
}

/*
 * __wt_msg --
 *     Informational message.
 */
int
__wt_msg(WT_SESSION_IMPL *session, const char *fmt, ...) WT_GCC_FUNC_ATTRIBUTE((cold))
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 2, 3)))
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_EVENT_HANDLER *handler;
    WT_SESSION *wt_session;

    WT_RET(__wt_scr_alloc(session, 0, &buf));

    WT_VA_ARGS_BUF_FORMAT(session, buf, fmt, false);

    wt_session = (WT_SESSION *)session;
    handler = session->event_handler;
    ret = handler->handle_message(handler, wt_session, buf->data);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_ext_msg_printf --
 *     Extension API call to print to the message stream.
 */
int
__wt_ext_msg_printf(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, const char *fmt, ...)
  WT_GCC_FUNC_ATTRIBUTE((format(printf, 3, 4)))
{
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_EVENT_HANDLER *handler;
    WT_SESSION_IMPL *session;

    if ((session = (WT_SESSION_IMPL *)wt_session) == NULL)
        session = ((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    WT_RET(__wt_scr_alloc(session, 0, &buf));

    WT_VA_ARGS_BUF_FORMAT(session, buf, fmt, false);

    wt_session = (WT_SESSION *)session;
    handler = session->event_handler;
    ret = handler->handle_message(handler, wt_session, buf->data);

err:
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __wt_ext_strerror --
 *     Extension API call to return an error as a string.
 */
const char *
__wt_ext_strerror(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, int error)
{
    if (wt_session == NULL)
        wt_session = (WT_SESSION *)((WT_CONNECTION_IMPL *)wt_api->conn)->default_session;

    return (wt_session->strerror(wt_session, error));
}

/*
 * __wt_progress --
 *     Progress message.
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
        if ((ret = handler->handle_progress(
               handler, wt_session, s == NULL ? session->name : s, v)) != 0)
            __handler_failure(session, ret, "progress", false);
    return (0);
}

/*
 * __wt_inmem_unsupported_op --
 *     Print a standard error message for an operation that's not supported for in-memory
 *     configurations.
 */
int
__wt_inmem_unsupported_op(WT_SESSION_IMPL *session, const char *tag) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    if (F_ISSET(S2C(session), WT_CONN_IN_MEMORY))
        WT_RET_MSG(session, ENOTSUP, "%s%snot supported for in-memory configurations",
          tag == NULL ? "" : tag, tag == NULL ? "" : ": ");
    return (0);
}

/*
 * __wt_object_unsupported --
 *     Print a standard error message for an object that doesn't support a particular operation.
 */
int
__wt_object_unsupported(WT_SESSION_IMPL *session, const char *uri) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_RET_MSG(session, ENOTSUP, "unsupported object operation: %s", uri);
}

/*
 * __wt_bad_object_type --
 *     Print a standard error message when given an unknown or unsupported object type.
 */
int
__wt_bad_object_type(WT_SESSION_IMPL *session, const char *uri) WT_GCC_FUNC_ATTRIBUTE((cold))
{
    if (WT_PREFIX_MATCH(uri, "backup:") || WT_PREFIX_MATCH(uri, "colgroup:") ||
      WT_PREFIX_MATCH(uri, "config:") || WT_PREFIX_MATCH(uri, "file:") ||
      WT_PREFIX_MATCH(uri, "index:") || WT_PREFIX_MATCH(uri, "log:") ||
      WT_PREFIX_MATCH(uri, "lsm:") || WT_PREFIX_MATCH(uri, "object:") ||
      WT_PREFIX_MATCH(uri, "statistics:") || WT_PREFIX_MATCH(uri, "table:") ||
      WT_PREFIX_MATCH(uri, "tiered:"))
        return (__wt_object_unsupported(session, uri));

    WT_RET_MSG(session, ENOTSUP, "unknown object type: %s", uri);
}

/*
 * __wt_unexpected_object_type --
 *     Print a standard error message when given an unexpected object type.
 */
int
__wt_unexpected_object_type(WT_SESSION_IMPL *session, const char *uri, const char *expect)
  WT_GCC_FUNC_ATTRIBUTE((cold))
{
    WT_RET_MSG(session, EINVAL, "uri %s doesn't match expected \"%s\"", uri, expect);
}
