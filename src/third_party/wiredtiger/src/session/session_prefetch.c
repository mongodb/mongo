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
 *     Check to see whether cursors owned by this session might benefit from doing pre-fetch
 *
 * FIXME-WT-11758 Change this function so that when pre-fetching is enabled on the session level,
 *     all cursors associated with that session with automatically perform pre-fetching. Use the
 *     session-level configuration once it is introduced.
 */
bool
__wt_session_prefetch_check(WT_SESSION_IMPL *session, WT_REF *ref)
{
    /* Internal threads should not be configured to do pre-fetching. */
    if (!S2C(session)->prefetch_auto_on || F_ISSET(session, WT_SESSION_INTERNAL))
        return (false);

    if (S2C(session)->prefetch_queue_count > WT_MAX_PREFETCH_QUEUE)
        return (false);

    /*
     * Don't deal with internal pages at the moment - finding the right content to preload based on
     * internal pages is hard.
     */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL))
        return (false);

    if (session->pf.prefetch_disk_read_count == 1)
        WT_STAT_CONN_INCR(session, block_prefetch_disk_one);

    /* A single read from disk is common - don't use it to guide pre-fetch behavior. */
    if (session->pf.prefetch_disk_read_count < 2) {
        WT_STAT_CONN_INCR(session, block_prefetch_skipped);
        return (false);
    }

    if (session->pf.prefetch_prev_ref == NULL) {
        WT_STAT_CONN_INCR(session, block_prefetch_attempts);
        return (true);
    }

    /*
     * If the previous pre-fetch was using the same home ref, pre-fetch for approximately the number
     * of pages that were added to the queue.
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
