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
    /* Check for WiredTiger specific errors. */
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

    /* Check for WiredTiger specific sub-level errors. */
    switch (error) {
    case WT_NONE:
        return ("WT_NONE: No additional context");
    case WT_BACKGROUND_COMPACT_ALREADY_RUNNING:
        return ("WT_BACKGROUND_COMPACT_ALREADY_RUNNING: Background compaction is already running");
    case WT_CACHE_OVERFLOW:
        return ("WT_CACHE_OVERFLOW: Cache capacity has overflown");
    case WT_WRITE_CONFLICT:
        return ("WT_WRITE_CONFLICT: Write conflict between concurrent operations");
    case WT_OLDEST_FOR_EVICTION:
        return ("WT_OLDEST_FOR_EVICTION: Transaction has the oldest pinned transaction ID");
    case WT_CONFLICT_BACKUP:
        return ("WT_CONFLICT_BACKUP: Conflict performing operation due to running backup");
    case WT_CONFLICT_DHANDLE:
        return ("WT_CONFLICT_DHANDLE: Another thread currently holds the data handle of the table");
    case WT_CONFLICT_SCHEMA_LOCK:
        return ("WT_CONFLICT_SCHEMA_LOCK: Conflict performing schema operation");
    case WT_UNCOMMITTED_DATA:
        return ("WT_UNCOMMITTED_DATA: Table has uncommitted data");
    case WT_DIRTY_DATA:
        return ("WT_DIRTY_DATA: Table has dirty data");
    case WT_CONFLICT_TABLE_LOCK:
        return ("WT_CONFLICT_TABLE_LOCK: Another thread currently holds the table lock");
    case WT_CONFLICT_CHECKPOINT_LOCK:
        return ("WT_CONFLICT_CHECKPOINT_LOCK: Another thread currently holds the checkpoint lock");
    case WT_MODIFY_READ_UNCOMMITTED:
        return (
          "WT_MODIFY_READ_UNCOMMITTED: Read-uncommitted readers do not support reconstructing a "
          "record with modifies");
    case WT_CONFLICT_LIVE_RESTORE:
        return (
          "WT_CONFLICT_LIVE_RESTORE: Conflict performing operation due to an in-progress live "
          "restore");
    case WT_CONFLICT_DISAGG:
        return ("WT_CONFLICT_DISAGG: Conflict with disaggregated storage");
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

/*
 * __wt_is_valid_sub_level_error --
 *     Return true if the provided error falls within the valid range for sub level error codes,
 *     return false otherwise.
 */
bool
__wt_is_valid_sub_level_error(int sub_level_err)
{
    return (sub_level_err <= -32000 && sub_level_err > -32200);
}
