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
 *     Check if pre-fetching work should be performed for a given ref.
 */
bool
__wt_session_prefetch_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    /* Disable pre-fetch work on tiered tables. */
    if (session->dhandle->type == WT_DHANDLE_TYPE_TIERED ||
      session->dhandle->type == WT_DHANDLE_TYPE_TIERED_TREE)
        return (false);

    if (S2C(session)->prefetch_queue_count > WT_MAX_PREFETCH_QUEUE)
        return (false);

    /*
     * Check if pre-fetching is enabled for this particular session. We don't perform pre-fetching
     * on internal threads or internal pages (finding the right content to preload based on internal
     * pages is hard), so check for that too. We also want to pre-fetch sessions that have read at
     * least one page from disk. The result of this function will subsequently be checked by cursor
     * logic to determine if pre-fetching will be performed.
     */
    if (!F_ISSET(session, WT_SESSION_PREFETCH_ENABLED)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (F_ISSET(session, WT_SESSION_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_session);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_internal_page);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (F_ISSET(S2BT(session), WT_BTREE_SPECIAL_FLAGS) &&
      !F_ISSET(S2BT(session), WT_BTREE_VERIFY)) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_special_handle);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    if (session->pf.prefetch_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, prefetch_disk_one);

    if (session->pf.prefetch_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, prefetch_skipped_disk_read_count);
        WT_STAT_CONN_INCR(session, prefetch_skipped);
        return (false);
    }

    WT_STAT_CONN_INCR(session, prefetch_attempts);

    return (true);
}
