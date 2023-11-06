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
    if (S2C(session)->prefetch_queue_count > WT_MAX_PREFETCH_QUEUE)
        return (false);

    /*
     * Check if pre-fetching is enabled for this particular session. We don't perform pre-fetching
     * on internal threads or internal pages (finding the right content to preload based on internal
     * pages is hard), so check for that too. We also want to pre-fetch sessions that have read at
     * least one page from disk. The result of this function will subsequently be checked by cursor
     * logic to determine if pre-fetching will be performed.
     */
    if (!F_ISSET(session, WT_SESSION_PREFETCH) || F_ISSET(session, WT_SESSION_INTERNAL) ||
      F_ISSET(ref, WT_REF_FLAG_INTERNAL) || session->pf.prefetch_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, block_prefetch_skipped);
        return (false);
    }

    if (session->pf.prefetch_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, block_prefetch_disk_one);

    if (session->pf.prefetch_prev_ref == NULL) {
        WT_STAT_CONN_INCR(session, block_prefetch_attempts);
        return (true);
    }

    /*
     * We want to avoid the scenario of requesting pre-fetch on one particular ref many times (e.g
     * when reading along a single page). We can identify this by checking if the previous pre-fetch
     * was performed using the same home ref.
     *
     * In the event that we find this to be true, we perform pre-fetch for approximately the number
     * of pages that were added to the queue (WT_PREFETCH_QUEUE_PER_TRIGGER). We then want to ensure
     * that we will not pre-fetch from this ref for a while, and this is done by checking a counter.
     *
     * The counter variable prefetch_skipped_with_parent tracks the number of skips we have
     * performed on a particular ref. If the number of skips surpasses the number of pages that have
     * been queued for pre-fetch, we are okay to pre-fetch from this ref again. This condition will
     * evaluate to false and the counter will be reset, effectively marking the ref as available to
     * pre-fetch from.
     */
    if (session->pf.prefetch_prev_ref->page == ref->home &&
      session->pf.prefetch_skipped_with_parent < WT_PREFETCH_QUEUE_PER_TRIGGER) {
        ++session->pf.prefetch_skipped_with_parent;
        WT_STAT_CONN_INCR(session, block_prefetch_skipped);
        return (false);
    }

    session->pf.prefetch_skipped_with_parent = 0;

    WT_STAT_CONN_INCR(session, block_prefetch_attempts);
    return (true);
}
