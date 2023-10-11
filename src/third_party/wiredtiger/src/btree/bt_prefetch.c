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
     * FIXME-WT-11759 Consider whether we should have these asserts here or swallow up the errors
     * instead.
     */
    WT_ASSERT_ALWAYS(session, F_ISSET(ref, WT_REF_FLAG_LEAF),
      "Pre-fetch starts with a leaf page and reviews the parent");

    WT_ASSERT_ALWAYS(session, __wt_session_gen(session, WT_GEN_SPLIT) != 0,
      "Pre-fetch requires a split generation to traverse internal page(s)");

    session->pf.prefetch_prev_ref = ref;
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
         * in cache.
         */
        if (next_ref->state == WT_REF_DISK && F_ISSET(next_ref, WT_REF_FLAG_LEAF)) {
            ret = __wt_conn_prefetch_queue_push(session, next_ref);
            if (ret == 0)
                ++block_preload;
            else if (ret != EBUSY)
                WT_RET(ret);
        }
    }
    WT_INTL_FOREACH_END;

    WT_STAT_CONN_INCRV(session, block_prefetch_pages_queued, block_preload);
    return (0);
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

    if (pe->ref->home != pe->first_home)
        __wt_verbose(
          session, WT_VERB_PREFETCH, "The home changed while queued for pre-fetch %s", "");

    /*
     * FIXME-WT-11759 Consider whether we should have these asserts here or swallow up the errors
     * instead.
     */
    WT_ASSERT_ALWAYS(session, pe->dhandle != NULL, "Pre-fetch needs to save a valid dhandle");
    WT_ASSERT_ALWAYS(
      session, !F_ISSET(pe->ref, WT_REF_FLAG_INTERNAL), "Pre-fetch should only see leaf pages");

    if (pe->ref->state != WT_REF_DISK) {
        WT_STAT_CONN_INCR(session, block_prefetch_pages_fail);
        return (0);
    }

    WT_STAT_CONN_INCR(session, block_prefetch_pages_read);

    if (__wt_ref_addr_copy(session, pe->ref, &addr)) {
        WT_RET(__wt_page_in(session, pe->ref, WT_READ_PREFETCH));
        WT_RET(__wt_page_release(session, pe->ref, 0));
    } else
        return (WT_ERROR);

    return (0);
}
