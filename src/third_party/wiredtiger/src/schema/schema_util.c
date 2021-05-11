/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __schema_backup_check_int --
 *     Helper for __wt_schema_backup_check. Intended to be called while holding the hot backup read
 *     lock.
 */
static int
__schema_backup_check_int(WT_SESSION_IMPL *session, const char *name)
{
    WT_CONNECTION_IMPL *conn;
    int i;
    char **backup_list;

    conn = S2C(session);

    /*
     * There is a window at the end of a backup where the list has been cleared from the connection
     * but the flag is still set. It is safe to drop at that point.
     */
    if (conn->hot_backup_start == 0 || (backup_list = conn->hot_backup_list) == NULL) {
        return (0);
    }
    for (i = 0; backup_list[i] != NULL; ++i) {
        if (strcmp(backup_list[i], name) == 0)
            return (__wt_set_return(session, EBUSY));
    }

    return (0);
}

/*
 * __wt_schema_backup_check --
 *     Check if a backup cursor is open and give an error if the schema operation will conflict.
 *     This is called after the schema operations have taken the schema lock so no hot backup cursor
 *     can be created until this is done.
 */
int
__wt_schema_backup_check(WT_SESSION_IMPL *session, const char *name)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);
    if (conn->hot_backup_start == 0)
        return (0);
    WT_WITH_HOTBACKUP_READ_LOCK_UNCOND(session, ret = __schema_backup_check_int(session, name));
    return (ret);
}

/*
 * __wt_schema_get_source --
 *     Find a matching data source or report an error.
 */
WT_DATA_SOURCE *
__wt_schema_get_source(WT_SESSION_IMPL *session, const char *name)
{
    WT_NAMED_DATA_SOURCE *ndsrc;

    TAILQ_FOREACH (ndsrc, &S2C(session)->dsrcqh, q)
        if (WT_PREFIX_MATCH(name, ndsrc->prefix))
            return (ndsrc->dsrc);
    return (NULL);
}

/*
 * __wt_schema_internal_session --
 *     Create and return an internal schema session if necessary.
 */
int
__wt_schema_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
{
    /*
     * Open an internal session if a transaction is running so that the schema operations are not
     * logged and buffered with any log records in the transaction. The new session inherits its
     * flags from the original.
     */
    *int_sessionp = session;
    if (F_ISSET(session->txn, WT_TXN_RUNNING)) {
        /* We should not have a schema txn running now. */
        WT_ASSERT(session, !F_ISSET(session, WT_SESSION_SCHEMA_TXN));
        WT_RET(__wt_open_internal_session(
          S2C(session), "schema", true, session->flags, session->lock_flags, int_sessionp));
    }
    return (0);
}

/*
 * __wt_schema_session_release --
 *     Release an internal schema session if needed.
 */
int
__wt_schema_session_release(WT_SESSION_IMPL *session, WT_SESSION_IMPL *int_session)
{
    if (session != int_session)
        WT_RET(__wt_session_close_internal(int_session));

    return (0);
}

/*
 * __str_name_check --
 *     Internal function to disallow any use of the WiredTiger name space. Can be called directly or
 *     after skipping the URI prefix.
 */
static int
__str_name_check(WT_SESSION_IMPL *session, const char *name, bool skip_wt)
{

    if (!skip_wt && WT_PREFIX_MATCH(name, "WiredTiger"))
        WT_RET_MSG(session, EINVAL,
          "%s: the \"WiredTiger\" name space may not be used by applications", name);

    /*
     * Disallow JSON quoting characters -- the config string parsing code supports quoted strings,
     * but there's no good reason to use them in names and we're not going to do the testing.
     */
    if (strpbrk(name, "{},:[]\\\"'") != NULL)
        WT_RET_MSG(session, EINVAL,
          "%s: WiredTiger objects should not include grouping characters in their names", name);
    return (0);
}

/*
 * __wt_str_name_check --
 *     Disallow any use of the WiredTiger name space.
 */
int
__wt_str_name_check(WT_SESSION_IMPL *session, const char *str)
{
    int skipped;
    const char *name, *sep;
    bool skip;

    /*
     * Check if name is somewhere in the WiredTiger name space: it would be "bad" if the application
     * truncated the metadata file. Skip any leading URI prefix if needed, check and then skip over
     * a table name.
     */
    name = str;
    skip = false;
    for (skipped = 0; skipped < 2; skipped++) {
        if ((sep = strchr(name, ':')) == NULL) {
            skip = true;
            break;
        }

        name = sep + 1;
    }
    return (__str_name_check(session, name, skip));
}

/*
 * __wt_name_check --
 *     Disallow any use of the WiredTiger name space.
 */
int
__wt_name_check(WT_SESSION_IMPL *session, const char *str, size_t len, bool check_uri)
{
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, len, &tmp));

    WT_ERR(__wt_buf_fmt(session, tmp, "%.*s", (int)len, str));

    /* If we want to skip the URI check call the internal function directly. */
    ret = check_uri ? __wt_str_name_check(session, tmp->data) :
                      __str_name_check(session, tmp->data, false);

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}
