/* DO NOT EDIT: automatically built by dist/api_err.py. */

#include "wt_internal.h"

/*
 * Historically, there was only the wiredtiger_strerror call because the POSIX port didn't need
 * anything more complex; Windows requires memory allocation of error strings, so we added the
 * WT_SESSION.strerror method. Because we want wiredtiger_strerror to continue to be as thread-safe
 * as possible, errors are split into two categories: WiredTiger's or the system's constant strings
 * and Everything Else, and we check constant strings before Everything Else.
 */

/*
 * __wt_wiredtiger_error --
 *     Return a constant string for POSIX-standard and WiredTiger errors.
 */
const char *
__wt_wiredtiger_error(int error)
{
    /*
     * Check for WiredTiger specific errors.
     */
    switch (error) {
    case WT_ROLLBACK:
        return ("WT_ROLLBACK: conflict between concurrent operations");
    case WT_DUPLICATE_KEY:
        return ("WT_DUPLICATE_KEY: attempt to insert an existing key");
    case WT_ERROR:
        return ("WT_ERROR: non-specific WiredTiger error");
    case WT_NOTFOUND:
        return ("WT_NOTFOUND: item not found");
    case WT_PANIC:
        return ("WT_PANIC: WiredTiger library panic");
    case WT_RESTART:
        return ("WT_RESTART: restart the operation (internal)");
    case WT_RUN_RECOVERY:
        return ("WT_RUN_RECOVERY: recovery must be run to continue");
    case WT_CACHE_FULL:
        return ("WT_CACHE_FULL: operation would overflow cache");
    case WT_PREPARE_CONFLICT:
        return ("WT_PREPARE_CONFLICT: conflict with a prepared update");
    case WT_TRY_SALVAGE:
        return ("WT_TRY_SALVAGE: database corruption detected");
    }

    /* Windows strerror doesn't support ENOTSUP. */
    if (error == ENOTSUP)
        return ("Operation not supported");

    /*
     * Check for 0 in case the underlying strerror doesn't handle it, some historically didn't.
     */
    if (error == 0)
        return ("Successful return: 0");

    /* POSIX errors are non-negative integers. */
    if (error > 0)
        return (strerror(error));

    return (NULL);
}

/*
 * wiredtiger_strerror --
 *     Return a string for any error value, non-thread-safe version.
 */
const char *
wiredtiger_strerror(int error)
{
    static char buf[128];

    return (__wt_strerror(NULL, error, buf, sizeof(buf)));
}
