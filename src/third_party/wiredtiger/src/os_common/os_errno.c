/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_errno --
 *     Return errno, or WT_ERROR if errno not set.
 */
int
__wt_errno(void)
{
    /*
     * Called when we know an error occurred, and we want the system error code, but there's some
     * chance it's not set.
     */
    return (errno == 0 ? WT_ERROR : errno);
}

/*
 * __wt_strerror --
 *     WT_SESSION.strerror and wiredtiger_strerror.
 */
const char *
__wt_strerror(WT_SESSION_IMPL *session, int error, char *errbuf, size_t errlen)
{
    const char *p;

    /*
     * Check for a WiredTiger or POSIX constant string, no buffer needed.
     */
    if ((p = __wt_wiredtiger_error(error)) != NULL)
        return (p);

    /*
     * !!!
     * This function MUST handle a NULL WT_SESSION_IMPL handle.
     *
     * When called with a passed-in buffer, write the buffer.
     * When called with a valid session handle, write the session's buffer.
     * There's no way the session's buffer should be NULL if buffer format
     * succeeded, but Coverity is unconvinced; regardless, a test for NULL
     * isn't a bad idea given future code changes in the underlying code.
     *
     * Fallback to a generic message.
     */
    if (errbuf != NULL && __wt_snprintf(errbuf, errlen, "error return: %d", error) == 0)
        return (errbuf);
    if (session != NULL && __wt_buf_fmt(session, &session->err, "error return: %d", error) == 0 &&
      session->err.data != NULL)
        return (session->err.data);

    /* Defeated. */
    return ("Unable to return error string");
}

/*
 * __wt_ext_map_windows_error --
 *     Extension API call to map a Windows system error to a POSIX/ANSI error.
 */
int
__wt_ext_map_windows_error(WT_EXTENSION_API *wt_api, WT_SESSION *wt_session, uint32_t windows_error)
{
    WT_UNUSED(wt_api);
    WT_UNUSED(wt_session);

/*
 * This extension API only makes sense in Windows builds, but it's hard to exclude it otherwise
 * (there's no way to return an error, anyway). Call an underlying function on Windows, else panic
 * so callers figure out what they're doing wrong.
 */
#ifdef _WIN32
    return (__wt_map_windows_error(windows_error));
#else
    WT_UNUSED(windows_error);
    WT_RET_PANIC(
      (WT_SESSION_IMPL *)wt_session, WT_PANIC, "unexpected attempt to map Windows error");
#endif
}
