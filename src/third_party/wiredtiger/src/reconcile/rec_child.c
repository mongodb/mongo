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
__rec_child_deleted(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REF *ref,
  WT_PAGE_DELETED *page_del, WT_CHILD_MODIFY_STATE *cmsp)
{
    uint8_t prepare_state;

    cmsp->state = WT_CHILD_IGNORE;

    /*
     * If there's no page-delete structure, the truncate must be globally visible. Discard any
     * underlying disk blocks and don't write anything in the internal page.
     */
    if (page_del == NULL)
        return (__wt_ref_block_free(session, ref));

    /*
     * The truncate may not yet be visible to us. In that case, we proceed as with any change not
     * visible during reconciliation by ignoring the change for the purposes of writing the internal
     * page.
     *
     * We expect the page to be clean after reconciliation. If there are invisible updates, abort
     * eviction.
     *
     * We must have reconciliation leave the page dirty in this case, because the truncation hasn't
     * been written to disk yet; if the page gets marked clean it might be discarded and then the
     * truncation is lost.
     */
    if (!__wt_page_del_visible(session, page_del, !F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT))) {
        if (F_ISSET(r, WT_REC_VISIBILITY_ERR))
            WT_RET_PANIC(session, EINVAL, "reconciliation illegally skipped an update");
        /*
         * In addition to the WT_REC_CLEAN_AFTER_REC case, fail if we're trying to evict an internal
         * page and we can't see the update to it. There's not much point continuing; unlike with a
         * leaf page, rewriting the page image and keeping the modification doesn't accomplish a
         * great deal. Also currently code elsewhere assumes that evicting (vs. checkpointing)
         * internal pages shouldn't leave them dirty.
         */
        if (F_ISSET(r, WT_REC_CLEAN_AFTER_REC | WT_REC_EVICT))
            return (__wt_set_return(session, EBUSY));
        cmsp->state = WT_CHILD_ORIGINAL;
        r->leave_dirty = true;
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
     *
     * As in the previous case, leave the page dirty. This is not strictly necessary as the prepared
     * truncation will also prevent eviction; but if we don't do it and someone adds the ability to
     * evict prepared truncates, the page apparently being clean might lead to truncations being
     * lost in hard-to-debug ways.
     */
    WT_ORDERED_READ(prepare_state, page_del->prepare_state);
    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        WT_ASSERT_ALWAYS(session, !F_ISSET(r, WT_REC_EVICT),
          "In progress prepares should never be seen in eviction");

        cmsp->state = WT_CHILD_ORIGINAL;
        r->leave_dirty = true;
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
    if (!__wt_page_del_visible(session, page_del, true)) {
        cmsp->del = *page_del;
        cmsp->state = WT_CHILD_PROXY;
        return (0);
    }

    /*
     * Globally visible truncate, discard the leaf page to the block manager and no cell needs to be
     * written. Done outside of the underlying tracking routines because this action is permanent
     * and irrevocable. (Clearing the address means we've lost track of the disk address in a
     * permanent way. This is safe because there's no path to reading the leaf page again: if there
     * is ever a read into this part of the name space again, the cache read function instantiates
     * an entirely new page.)
     */
    WT_RET(__wt_ref_block_free(session, ref));

    /*
     * Globally visible fast-truncate information is never used again, a NULL value is identical.
     * Fast-truncate information in the page-modify structure can be used more than once if this
     * reconciliation of the internal page were to fail.
     */
    if (page_del == ref->ft_info.del)
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
            // 9417 IGNORE
            WT_ASSERT(session, ref->addr != NULL);
            /* DISK pages do not have fast-truncate info. */
            WT_ASSERT(session, ref->ft_info.del == NULL);
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
            ret = __rec_child_deleted(session, r, ref, ref->ft_info.del, cmsp);
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

            /*
             * The child is potentially modified if the page's modify structure has been created. If
             * the modify structure exists and the page has been reconciled, set that state.
             */
            mod = ref->page->modify;
            if (mod != NULL && mod->rec_result != 0) {
                cmsp->state = WT_CHILD_MODIFIED;
                goto done;
            }

            /*
             * Deleted page instantiation can happen at any time during a checkpoint. If we found
             * the instantiated page in the first checkpoint pass, it will have been reconciled and
             * dealt with normally. However, if that didn't happen, we get here with a page that has
             * been modified and never reconciled.
             *
             * Ordinarily in that situation we'd write a reference to the original child page, and
             * in the ordinary case where the modifications were applied after the checkpoint
             * started that would be fine. However, for a deleted page it's possible that the
             * deletion predates the checkpoint and is visible, and only the instantiation happened
             * after the checkpoint started. In that case we need the modifications to appear in the
             * checkpoint, but if we didn't already reconcile the page it's too late to do it now.
             * Depending on visibility, we may need to write the original page, or write a proxy
             * (deleted-address) cell with the pre-instantiation page-delete information, or we may
             * be able to ignore the page entirely. We keep the original fast-truncate information
             * in the modify structure after instantiation to make the visibility check possible.
             *
             * The key is the page-modify.instantiated flag, removed during page reconciliation. If
             * it's set, instantiation happened after checkpoint passed the leaf page and we treat
             * this page like a WT_REF_DELETED page, evaluating it as it was before instantiation.
             *
             * We do not need additional locking: with a hazard pointer the page can't be evicted,
             * and reconciliation is the only thing that can clear the page-modify info.
             */
            if (mod != NULL && mod->instantiated) {
                WT_RET(__rec_child_deleted(session, r, ref, mod->page_del, cmsp));
                goto done;
            }

            /*
             * Insert splits are permitted during checkpoint. Checkpoints first walk the internal
             * page's page-index and write out any dirty pages we find, then we write out the
             * internal page in post-order traversal. If we found the split page in the first step,
             * it will have an address; if we didn't find the split page in the first step, it won't
             * have an address and we ignore it, it's not part of the checkpoint.
             */
            if (ref->addr == NULL)
                cmsp->state = WT_CHILD_IGNORE;
            goto done;

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

done:
    WT_DIAGNOSTIC_YIELD;
    return (ret);
}
