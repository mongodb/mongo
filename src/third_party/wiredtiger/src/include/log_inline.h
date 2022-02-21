/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

/*
 * __wt_log_cmp --
 *     Compare 2 LSNs, return -1 if lsn1 < lsn2, 0if lsn1 == lsn2 and 1 if lsn1 > lsn2.
 */
static inline int
__wt_log_cmp(WT_LSN *lsn1, WT_LSN *lsn2)
{
    uint64_t l1, l2;

    /*
     * Read LSNs into local variables so that we only read each field once and all comparisons are
     * on the same values.
     */
    l1 = ((volatile WT_LSN *)lsn1)->file_offset;
    l2 = ((volatile WT_LSN *)lsn2)->file_offset;

    return (l1 < l2 ? -1 : (l1 > l2 ? 1 : 0));
}

/*
 * __wt_log_op --
 *     Return if an operation should be logged.
 */
static inline bool
__wt_log_op(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    /*
     * Objects with checkpoint durability don't need logging unless we're in debug mode. That rules
     * out almost all log records, check it first.
     */
    if (F_ISSET(S2BT(session), WT_BTREE_NO_LOGGING) &&
      !FLD_ISSET(conn->log_flags, WT_CONN_LOG_DEBUG_MODE))
        return (false);

    /* Logging must be enabled, and outside of recovery. */
    if (!FLD_ISSET(conn->log_flags, WT_CONN_LOG_ENABLED))
        return (false);
    if (F_ISSET(conn, WT_CONN_RECOVERING))
        return (false);

    return (true);
}
