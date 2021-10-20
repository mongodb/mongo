/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_op_timer_start --
 *     Start the operations timer.
 */
static inline void
__wt_op_timer_start(WT_SESSION_IMPL *session)
{
    uint64_t timeout_us;

    /* Timer can be configured per-transaction, and defaults to per-connection. */
    if ((timeout_us = session->txn.operation_timeout_us) == 0)
        timeout_us = S2C(session)->operation_timeout_us;
    if (timeout_us == 0)
        session->operation_start_us = session->operation_timeout_us = 0;
    else {
        session->operation_start_us = __wt_clock(session);
        session->operation_timeout_us = timeout_us;
    }
}

/*
 * __wt_op_timer_stop --
 *     Stop the operations timer.
 */
static inline void
__wt_op_timer_stop(WT_SESSION_IMPL *session)
{
    session->operation_start_us = session->operation_timeout_us = 0;
}

/*
 * __wt_op_timer_fired --
 *     Check the operations timers.
 */
static inline bool
__wt_op_timer_fired(WT_SESSION_IMPL *session)
{
    uint64_t diff, now;

    if (session->operation_start_us == 0 || session->operation_timeout_us == 0)
        return (false);

    now = __wt_clock(session);
    diff = WT_CLOCKDIFF_US(now, session->operation_start_us);
    return (diff > session->operation_timeout_us);
}
