/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */
#ifndef __LOAD_CONTROL_INLINE_H
#define __LOAD_CONTROL_INLINE_H

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * __wt_conn_load_control_read_loadshed --
 *     Check whether reads should be shed because the read load has crossed the configured load
 *     control threshold.
 */
static WT_INLINE bool
__wt_conn_load_control_read_loadshed(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control = &S2C(session)->load_control;

    /* Shed reads when load control is enabled and the read load is at or above the threshold. */
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL) && load_control->control_threshold > 0)
        return (load_control->control_threshold <=
          __wt_atomic_load_uint16_relaxed(&load_control->read_load));

    return (false);
}

/*
 * __wt_conn_load_control_write_loadshed --
 *     Check whether writes should be shed because the write load has crossed the configured load
 *     control threshold.
 */
static WT_INLINE bool
__wt_conn_load_control_write_loadshed(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control = &S2C(session)->load_control;

    /* Shed writes when load control is enabled and the write load is at or above the threshold. */
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL) && load_control->control_threshold > 0)
        return (load_control->control_threshold <=
          __wt_atomic_load_uint16_relaxed(&load_control->write_load));

    return (false);
}

#if defined(__cplusplus)
}
#endif
#endif /* !__LOAD_CONTROL_INLINE_H */
