/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_session_prefetch_check --
 *     Check if pre-fetching should be triggered for a given ref. Pre-fetching is skipped for
 *     internal sessions, internal pages, tiered tables, special btree handles, an overwhelmed
 *     prefetch queue, and sessions that have not yet read enough pages from disk to justify it.
 *     Internal pages are excluded because identifying which leaf pages to preload from an internal
 *     page traversal is non-trivial.
 */
bool
__wt_session_prefetch_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    if (!F_ISSET(session, WT_SESSION_PREFETCH_ENABLED))
        return (false);

    WT_STAT_CONN_INCR(session, prefetch_attempts);

    if (__wt_atomic_load_enum_relaxed(&session->dhandle->type) == WT_DHANDLE_TYPE_TIERED ||
      __wt_atomic_load_enum_relaxed(&session->dhandle->type) == WT_DHANDLE_TYPE_TIERED_TREE)
        return (false);

    if (__wt_tsan_suppress_load_uint64(&S2C(session)->prefetch_queue_count) >
      WT_MAX_PREFETCH_QUEUE) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_queue_full);
        return (false);
    }

    if (F_ISSET(session, WT_SESSION_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_session);
        return (false);
    }

    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_page);
        return (false);
    }

    if (F_ISSET(S2BT(session), WT_BTREE_SPECIAL_FLAGS) &&
      !F_ISSET(S2BT(session), WT_BTREE_VERIFY)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_special_handle);
        return (false);
    }

    if (session->pf.prefetch_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, prefetch_disk_one);

    if (session->pf.prefetch_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_disk_read_count);
        return (false);
    }

    WT_STAT_CONN_INCR(session, prefetch_attempts_succeeded);

    return (true);
}
