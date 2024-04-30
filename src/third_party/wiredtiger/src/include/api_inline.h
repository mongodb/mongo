/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *  All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#pragma once

/*
 * TODO: WT-12795 Ideally calls to these functions would be managed in the same way as other API
 * tracking works, but they are special and different at the moment due to complexity in ordering
 * related to the different work done during API entry.
 */

/* Some macros to assist with a clean flow when using cursor API tracking */
#define WT_DECL_CUR_TRACK bool _tracked = false
#define WT_CUR_TRACK_BEGIN(session)       \
    __wt_api_track_cursor_begin(session); \
    _tracked = true
#define WT_CUR_TRACK_END(session) \
    if (_tracked)                 \
    __wt_api_track_cursor_end(session)

/*
 * __wt_api_track_cursor_begin --
 *     Start tracking a cursor API entry point for statistics.
 */
static WT_INLINE void
__wt_api_track_cursor_begin(WT_SESSION_IMPL *session)
{
    /*
     * Track cursor API calls, so we can know how many are in the library at a point in time. These
     * need to be balanced. If the api call counter is zero, it means these have been used in the
     * wrong order compared to the other enter/end macros.
     */
    WT_ASSERT(session, WT_SESSION_IS_DEFAULT(session) || session->api_call_counter != 0);
    if (session->api_call_counter == 1) {
        if (F_ISSET(session, WT_SESSION_INTERNAL))
            (void)__wt_atomic_add64(&S2C(session)->api_count_cursor_int_in, 1);
        else
            (void)__wt_atomic_add64(&S2C(session)->api_count_cursor_in, 1);
    }
}

/*
 * __wt_api_track_cursor_end --
 *     Finish tracking a cursor API entry point for statistics.
 */
static WT_INLINE void
__wt_api_track_cursor_end(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_ASSERT(session, WT_SESSION_IS_DEFAULT(session) || session->api_call_counter != 0);
    if ((session)->api_call_counter == 1) {
        if (F_ISSET(session, WT_SESSION_INTERNAL)) {
            (void)__wt_atomic_add64(&conn->api_count_cursor_int_out, 1);
            WT_API_COUNTER_CHECK(session, api_count_cursor_int);
        } else {
            (void)__wt_atomic_add64(&conn->api_count_cursor_out, 1);
            WT_API_COUNTER_CHECK(session, api_count_cursor);
        }
    }
}
