/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wt_evict_file --
 *     Discard pages for a specific file.
 */
int
__wt_evict_file(WT_SESSION_IMPL *session, WT_CACHE_OP syncop)
{
    WT_BTREE *btree;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_REF *next_ref, *ref;
    uint32_t rec_flags, walk_flags;

    dhandle = session->dhandle;
    btree = dhandle->handle;

    /*
     * We need exclusive access to the file, we're about to discard the root page. Assert eviction
     * has been locked out.
     */
    WT_ASSERT(session, btree->evict_disabled > 0 || !F_ISSET(dhandle, WT_DHANDLE_OPEN));

    /*
     * We do discard objects without pages in memory. If that's the case, we're done.
     */
    if (btree->root.page == NULL)
        return (0);

    /* Make sure the oldest transaction ID is up-to-date. */
    WT_RET(__wt_txn_update_oldest(session, WT_TXN_OLDEST_STRICT | WT_TXN_OLDEST_WAIT));

    /* Walk the tree, discarding pages. */
    walk_flags = WT_READ_CACHE | WT_READ_NO_EVICT;
    if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))
        walk_flags |= WT_READ_VISIBLE_ALL;

    next_ref = NULL;
    WT_ERR(__wt_tree_walk(session, &next_ref, walk_flags));
    while ((ref = next_ref) != NULL) {
        page = ref->page;

        /*
         * Eviction can fail when a page in the evicted page's subtree switches state. For example,
         * if we don't evict a page marked empty, because we expect it to be merged into its parent,
         * it might no longer be empty after it's reconciled, in which case eviction of its parent
         * would fail. We can either walk the tree multiple times (until it's finally empty), or
         * reconcile each page to get it to its final state before considering if it's an eviction
         * target or will be merged into its parent.
         *
         * Don't limit this test to any particular page type, that tends to introduce bugs when the
         * reconciliation of other page types changes, and there's no advantage to doing so.
         *
         * Eviction can also fail because an update cannot be written. If sessions have disjoint
         * sets of files open, updates in a no-longer-referenced file may not yet be visible, and
         * the write will fail with EBUSY. Our caller handles that error, retrying later.
         */
        if (syncop == WT_SYNC_CLOSE && __wt_page_is_modified(page)) {
            /*
             * When setting the reconciliation flags, remember to not enable history store eviction
             * for the history store file itself. Also metadata file doesn't have any associated
             * history.
             */
            rec_flags = WT_REC_EVICT | WT_REC_CLEAN_AFTER_REC | WT_REC_VISIBLE_ALL;
            if (!WT_IS_HS(btree->dhandle) && !WT_IS_METADATA(dhandle))
                rec_flags |= WT_REC_HS;
            WT_ERR(__wt_reconcile(session, ref, NULL, rec_flags));
        }

        /*
         * We can't evict the page just returned to us (it marks our place in the tree), so move the
         * walk to one page ahead of the page being evicted. Note, we reconciled the returned page
         * first: if reconciliation of that page were to change the shape of the tree, and we did
         * the next walk call before the reconciliation, the next walk call could miss a page in the
         * tree.
         */
        WT_ERR(__wt_tree_walk(session, &next_ref, walk_flags));

        switch (syncop) {
        case WT_SYNC_CLOSE:
            /* Evict the page. */
            WT_ERR(__wt_evict(session, ref, ref->state, WT_EVICT_CALL_CLOSING));
            break;
        case WT_SYNC_DISCARD:
            /*
             * Discard the page whether it is dirty or not. The check if the page can be evicted is
             * not exhaustive, but provides basic checking on the page's status.
             */
            WT_ASSERT(session,
              F_ISSET(dhandle, WT_DHANDLE_DEAD) || F_ISSET(S2C(session), WT_CONN_CLOSING) ||
                __wt_page_can_evict(session, ref, NULL));
            __wt_ref_out(session, ref);
            break;
        case WT_SYNC_CHECKPOINT:
        case WT_SYNC_WRITE_LEAVES:
            WT_ERR(__wt_illegal_value(session, syncop));
            break;
        }
    }

    if (0) {
err:
        /* On error, clear any left-over tree walk. */
        if (next_ref != NULL)
            WT_TRET(__wt_page_release(session, next_ref, walk_flags));
    }

    return (ret);
}
