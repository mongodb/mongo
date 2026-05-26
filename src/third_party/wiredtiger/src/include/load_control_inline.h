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
 * __wt_conn_load_control_read_overload --
 *     check if the system is read overloaded.
 */
static WT_INLINE bool
__wt_conn_load_control_read_overload(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control = &S2C(session)->load_control;

    /* If load control is enabled, check if the read load crossed the control threshold. */
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL))
        return (load_control->control_threshold <=
          __wt_atomic_load_uint8_relaxed(&load_control->read_load));

    return (false);
}

/*
 * __wt_conn_load_control_write_overload --
 *     check if the system is write overloaded.
 */
static WT_INLINE bool
__wt_conn_load_control_write_overload(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_LOAD_CONTROL *load_control = &S2C(session)->load_control;

    /* If load control is enabled, check if the write load crossed the control threshold. */
    if (F_ISSET(load_control, WT_CONN_LOAD_CONTROL))
        return (load_control->control_threshold <=
          __wt_atomic_load_uint8_relaxed(&load_control->write_load));

    return (false);
}

#if defined(__cplusplus)
}
#endif
#endif /* !__LOAD_CONTROL_INLINE_H */
