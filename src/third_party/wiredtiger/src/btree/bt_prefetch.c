/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_btree_prefetch --
 *     Pre-load a set of pages into the cache. This session holds a hazard pointer on the ref passed
 *     in, so there must be a valid page and a valid parent page (though that parent could change if
 *     a split happens).
 */
int
__wt_btree_prefetch(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_REF *next_ref;
    uint64_t block_preload;

    conn = S2C(session);
    block_preload = 0;

    /*
     * Pre-fetch traverses the internal page, to do that safely it requires a split gen.
     */
    if (!(F_ISSET(ref, WT_REF_FLAG_LEAF) || __wt_session_gen(session, WT_GEN_SPLIT) == 0)) {
        WT_STAT_CONN_INCR(session, block_prefetch_failed_start);
        return (0);
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
    if (session->pf.prefetch_prev_ref_home == ref->home &&
      session->pf.prefetch_skipped_with_parent < WT_PREFETCH_QUEUE_PER_TRIGGER) {
        ++session->pf.prefetch_skipped_with_parent;
        WT_STAT_CONN_INCR(session, block_prefetch_skipped_same_ref);
        WT_STAT_CONN_INCR(session, block_prefetch_skipped);
        return (0);
    }

    session->pf.prefetch_skipped_with_parent = 0;

    /* Load and decompress a set of pages into the block cache. */
    WT_INTL_FOREACH_BEGIN (session, ref->home, next_ref) {
        /* Don't let the pre-fetch queue get overwhelmed. */
        if (conn->prefetch_queue_count > WT_MAX_PREFETCH_QUEUE ||
          block_preload > WT_PREFETCH_QUEUE_PER_TRIGGER)
            break;

        /*
         * Skip queuing pages that are already in cache or are internal. They aren't the pages we
         * are looking for. This pretty much assumes that all children of an internal page remain in
         * cache during the scan. If a previous pre-fetch of this internal page read a page in, then
         * that page was evicted and now a future page wants to be pre-fetched, this algorithm needs
         * a tweak. It would need to remember which child was last queued and start again from
         * there, rather than this approximation which assumes recently pre-fetched pages are still
         * in cache. Don't prefetch fast deleted pages to avoid wasted effort. We can skip reading
         * these deleted pages into the cache if the fast truncate information is visible in the
         * session transaction snapshot.
         */
        if (next_ref->state == WT_REF_DISK && F_ISSET(next_ref, WT_REF_FLAG_LEAF) &&
          next_ref->page_del == NULL && !F_ISSET(next_ref, WT_REF_FLAG_PREFETCH)) {
            ret = __wt_conn_prefetch_queue_push(session, next_ref);
            if (ret == EBUSY) {
                ret = 0;
                break;
            }
            if (ret != 0)
                break;
            ++block_preload;
        }
    }
    WT_INTL_FOREACH_END;
    session->pf.prefetch_prev_ref_home = ref->home;

    WT_STAT_CONN_INCRV(session, block_prefetch_pages_queued, block_preload);
    return (ret);
}

/*
 * __wt_prefetch_page_in --
 *     Does the heavy lifting of reading a page into the cache. Immediately releases the page since
 *     reading it in is the useful side effect here. Must be called while holding a dhandle.
 */
int
__wt_prefetch_page_in(WT_SESSION_IMPL *session, WT_PREFETCH_QUEUE_ENTRY *pe)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;

    if (pe->ref->home != pe->first_home)
        __wt_verbose(
          session, WT_VERB_PREFETCH, "The home changed while queued for pre-fetch %s", "");

    WT_PREFETCH_ASSERT(session, pe->dhandle != NULL, block_prefetch_skipped_no_valid_dhandle);
    WT_PREFETCH_ASSERT(
      session, !F_ISSET(pe->ref, WT_REF_FLAG_INTERNAL), block_prefetch_skipped_internal_page);

    if (pe->ref->state != WT_REF_DISK) {
        WT_STAT_CONN_INCR(session, block_prefetch_pages_fail);
        return (0);
    }

    WT_STAT_CONN_INCR(session, block_prefetch_pages_read);

    WT_ENTER_GENERATION(session, WT_GEN_SPLIT);
    if (__wt_ref_addr_copy(session, pe->ref, &addr)) {
        WT_ERR(__wt_page_in(session, pe->ref, WT_READ_PREFETCH));
        WT_ERR(__wt_page_release(session, pe->ref, 0));
    } else
        ret = (WT_ERROR);

err:
    WT_LEAVE_GENERATION(session, WT_GEN_SPLIT);
    return (ret);
}
