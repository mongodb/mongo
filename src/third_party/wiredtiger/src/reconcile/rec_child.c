/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __rec_child_deleted --
 *     Handle pages with leaf pages in the WT_REF_DELETED state.
 */
static int
__rec_child_deleted(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref, WT_CHILD_MODIFY_STATE *cmsp)
{
    WT_PAGE_DELETED *page_del;
    WT_TXN *txn;
    uint8_t prepare_state;

    cmsp->state = WT_CHILD_IGNORE;

    txn = session->txn;

    /*
     * The complicated case is a fast-delete which may not be visible or stable. Otherwise, discard
     * any underlying disk blocks and don't write anything.
     */
    page_del = ref->ft_info.del;
    if (page_del == NULL)
        return (ref->addr == NULL ? 0 : __wt_ref_block_free(session, ref));

    /*
     * The fast-delete may not yet be visible to us. In that case, we proceed as with any change not
     * visible during reconciliation by ignoring the change for the purposes of writing the internal
     * page.
     *
     * We expect the page to be clean after reconciliation. If there are invisible updates, abort
     * eviction.
     */
    if (__wt_page_del_active(session, ref, !F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))) {
        if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
            WT_RET_PANIC(session, EINVAL, "reconciliation illegally skipped an update");
        if (F_ISSET(r, WT_REC_CLEAN_AFTER_REC))
            return (__wt_set_return(session, EBUSY));
        cmsp->state = WT_CHILD_ORIGINAL;
        return (0);
    }

    /*
     * A visible entry can be in a prepared state and checkpoints skip in-progress prepared changes.
     * We can't race here, the entry won't be visible to the checkpoint, or will be in a prepared
     * state, one or the other.
     *
     * We should never see an in-progress prepare in eviction: when we check to see if an internal
     * page can be evicted, we check for an unresolved fast-truncate, which includes a fast-truncate
     * in a prepared state, so it's an error to see that during eviction.
     */
    WT_ORDERED_READ(prepare_state, page_del->prepare_state);
    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        WT_ASSERT(session, !F_ISSET(r, WT_REC_EVICT));

        cmsp->state = WT_CHILD_ORIGINAL;
        return (0);
    }

    /*
     * Deal with underlying disk blocks. If there are readers that might want to see the page's
     * state before it's deleted, or the fast-delete can be undone by RTS, we can't discard the
     * pages. Write a cell to the internal page with information describing the fast-delete.
     *
     * We have the WT_REF locked, but that lock is released before returning to the function writing
     * cells to the page. Copy out the current fast-truncate information for that function.
     */
    if (__wt_page_del_active(session, ref, true)) {
        cmsp->del = *ref->ft_info.del;
        cmsp->state = WT_CHILD_PROXY;
        return (0);
    }

    /*
     * Otherwise, we can discard the leaf page to the block manager and no cell needs to be written.
     * Done outside of the underlying tracking routines because this action is permanent and
     * irrevocable. (Clearing the address means we've lost track of the disk address in a permanent
     * way. This is safe because there's no path to reading the leaf page again: if there's ever a
     * read into this part of the name space again, the cache read function instantiates an entirely
     * new page.)
     */
    WT_RET(__wt_ref_block_free(session, ref));
    __wt_overwrite_and_free(session, ref->ft_info.del);
    return (0);
}

/*
 * __wt_rec_child_modify --
 *     Return if the internal page's child references any modifications.
 */
int
__wt_rec_child_modify(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref, WT_CHILD_MODIFY_STATE *cmsp)
{
    WT_DECL_RET;
    WT_PAGE_MODIFY *mod;

    /* We may acquire a hazard pointer our caller must release. */
    cmsp->hazard = false;

    /* Default to using the original child address. */
    cmsp->state = WT_CHILD_ORIGINAL;

    /*
     * This function is called when walking an internal page to decide how to handle child pages
     * referenced by the internal page.
     *
     * Internal pages are reconciled for two reasons: first, when evicting an internal page, second
     * by the checkpoint code when writing internal pages. During eviction, all pages should be in
     * the WT_REF_DISK or WT_REF_DELETED state. During checkpoint, eviction that might affect review
     * of an internal page is prohibited, however, as the subtree is not reserved for our exclusive
     * use, there are other page states that must be considered.
     */
    for (;; __wt_yield()) {
        switch (r->tested_ref_state = ref->state) {
        case WT_REF_DISK:
            /* On disk, not modified by definition. */
            WT_ASSERT(session, ref->addr != NULL);
            goto done;

        case WT_REF_DELETED:
            /*
             * The child is in a deleted state.
             *
             * It's possible the state could change underneath us as the page is read in, and we can
             * race between checking for a deleted state and looking at the transaction ID to see if
             * the delete is visible to us. Lock down the structure.
             */
            if (!WT_REF_CAS_STATE(session, ref, WT_REF_DELETED, WT_REF_LOCKED))
                break;
            ret = __rec_child_deleted(session, r, ref, cmsp);
            WT_REF_SET_STATE(ref, WT_REF_DELETED);
            goto done;

        case WT_REF_LOCKED:
            /*
             * Locked.
             *
             * We should never be here during eviction, active child pages in an evicted page's
             * subtree fails the eviction attempt.
             */
            WT_RET_ASSERT(session, !F_ISSET(r, WT_REC_EVICT), EBUSY,
              "unexpected WT_REF_LOCKED child state during eviction reconciliation");

            /* If the page is being read from disk, it's not modified by definition. */
            if (F_ISSET(ref, WT_REF_FLAG_READING))
                goto done;

            /*
             * Otherwise, the child is being considered by the eviction server or the child is a
             * deleted page being read. The eviction may have started before the checkpoint and so
             * we must wait for the eviction to be resolved. I suspect we could handle reads of
             * deleted pages, but we can't distinguish between the two and reads of deleted pages
             * aren't expected to be common.
             */
            break;

        case WT_REF_MEM:
            /*
             * In memory.
             *
             * We should never be here during eviction, active child pages in an evicted page's
             * subtree fails the eviction attempt.
             */
            WT_RET_ASSERT(session, !F_ISSET(r, WT_REC_EVICT), EBUSY,
              "unexpected WT_REF_MEM child state during eviction reconciliation");

            /*
             * If called during checkpoint, acquire a hazard pointer so the child isn't evicted,
             * it's an in-memory case.
             *
             * This call cannot return split/restart, we have a lock on the parent which prevents a
             * child page split.
             *
             * Set WT_READ_NO_WAIT because we're only interested in the WT_REF's final state. Pages
             * in transition might change WT_REF state during our read, and then return WT_NOTFOUND
             * to us. In that case, loop and look again.
             */
            ret = __wt_page_in(
              session, ref, WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_NO_GEN | WT_READ_NO_WAIT);
            if (ret == WT_NOTFOUND) {
                ret = 0;
                break;
            }
            WT_RET(ret);
            cmsp->hazard = true;
            goto in_memory;

        case WT_REF_SPLIT:
            /*
             * The page was split out from under us.
             *
             * We should never be here during eviction, active child pages in an evicted page's
             * subtree fails the eviction attempt.
             *
             * We should never be here during checkpoint, dirty page eviction is shutout during
             * checkpoint, all splits in process will have completed before we walk any pages for
             * checkpoint.
             */
            WT_RET_ASSERT(
              session, false, EBUSY, "unexpected WT_REF_SPLIT child state during reconciliation");
            /* NOTREACHED */
            return (EBUSY);

        default:
            return (__wt_illegal_value(session, r->tested_ref_state));
        }
        WT_STAT_CONN_INCR(session, child_modify_blocked_page);
    }

in_memory:
    /*
     * In-memory states: the child is potentially modified if the page's modify structure has been
     * instantiated. If the modify structure exists and the page has actually been modified, set
     * that state. If that's not the case, we would normally use the original cell's disk address as
     * our reference, however there are two special cases, both flagged by a missing block address.
     *
     * First, if forced to instantiate a deleted child page and it's never modified, we end up here
     * with a page that has a modify structure, no modifications, and no disk address. Ignore those
     * pages, they're not modified and there is no reason to write the cell.
     *
     * Second, insert splits are permitted during checkpoint. When doing the final checkpoint pass,
     * we first walk the internal page's page-index and write out any dirty pages we find, then we
     * write out the internal page in post-order traversal. If we found the split page in the first
     * step, it will have an address; if we didn't find the split page in the first step, it won't
     * have an address and we ignore it, it's not part of the checkpoint.
     */
    mod = ref->page->modify;
    if (mod != NULL && mod->rec_result != 0)
        cmsp->state = WT_CHILD_MODIFIED;
    else if (ref->addr == NULL) {
        cmsp->state = WT_CHILD_IGNORE;
        WT_CHILD_RELEASE(session, cmsp->hazard, ref);
    }

done:
    WT_DIAGNOSTIC_YIELD;
    return (ret);
}
