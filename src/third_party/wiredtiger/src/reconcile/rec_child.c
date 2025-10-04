/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"
#include "reconcile_private.h"
#include "reconcile_inline.h"
/*
 * __rec_child_deleted --
 *     Handle pages with leaf pages in the WT_REF_DELETED state.
 */
static int
__rec_child_deleted(
  WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *ref, WTI_CHILD_MODIFY_STATE *cmsp)
{
    WT_CONNECTION_IMPL *conn;
    WT_PAGE_DELETED *page_del;
    uint8_t prepare_state;
    bool visible, visible_all;

    conn = S2C(session);
    visible = visible_all = false;
    page_del = ref->page_del;

    cmsp->state = WTI_CHILD_IGNORE;

    /*
     * If there's no page-delete structure, the truncate must be globally visible. Discard any
     * underlying disk blocks and don't write anything in the internal page.
     */
    if (page_del == NULL)
        return (__wt_ref_block_free(session, ref, false));

    /*
     * Check visibility. If the truncation is visible to us, we'll also want to know if it's visible
     * to everyone. Use the special-case logic in __wt_page_del_visible to hide prepared truncations
     * as we can't write them to disk.
     *
     * We can't write out uncommitted truncations so we need to check the committed flag on the page
     * delete structure. The committed flag indicates that the truncation has finished being
     * processed by the transaction commit call and is a separate concept to the visibility, which
     * means that while the truncation may be visible it hasn't finished committing. This can occur
     * with prepared truncations, which go through two distinct phases in __wt_txn_commit:
     *   - Firstly the operations on the transaction are walked and the page delete structure has
     *     its prepare state set to resolved. At this stage the truncate can appear to be visible.
     *   - After the operations have been resolved the page delete structure is marked as being
     *     committed.
     *
     * Given the order of these operations we must perform the inverse sequence. First check the
     * committed flag and then check the visibility. There is a concurrency concern here as if the
     * write to the page delete structure is reordered we may see it be set early. However this is
     * handled by locking the ref in the commit path. Additionally this function locks the ref. Thus
     * setting the page delete structure committed flag cannot overlap with us checking the flag.
     */
    if (__wt_page_del_committed_set(page_del)) {
        if (F_ISSET(r, WT_REC_VISIBLE_NO_SNAPSHOT)) {
            visible = page_del->txnid < r->rec_start_pinned_id;

            if (visible) {
                WT_ACQUIRE_READ(prepare_state, page_del->prepare_state);
                if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED)
                    visible = false;
            }

            if (visible && F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT) &&
              page_del->pg_del_durable_ts > r->rec_start_pinned_stable_ts)
                visible = false;

            visible_all = visible ? __wt_page_del_visible_all(session, page_del, true) : false;
        } else if (F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT)) {
            visible = __wt_page_del_visible(session, page_del, true);

            if (visible && F_ISSET(conn, WT_CONN_PRECISE_CHECKPOINT) &&
              page_del->pg_del_durable_ts > r->rec_start_pinned_stable_ts)
                visible = false;

            visible_all = visible ? __wt_page_del_visible_all(session, page_del, true) : false;
        } else
            visible = visible_all = __wt_page_del_visible_all(session, page_del, true);
    }
    /*
     * If an earlier reconciliation chose to write the fast truncate information to the page, we
     * should select it regardless of visibility unless it is globally visible. This is important as
     * it is never ok to shift the on-disk value backwards.
     */
    if (page_del->selected_for_write && !visible_all) {
        cmsp->del = *page_del;
        cmsp->state = WTI_CHILD_PROXY;
        return (0);
    }

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
    if (!visible) {
        WT_ASSERT(session, !visible_all);
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
        cmsp->state = WTI_CHILD_ORIGINAL;
        r->leave_dirty = true;
        return (0);
    }

    /*
     * We should never get this far with an uncommitted deletion: in a checkpoint an uncommitted
     * deletion should not be visible, and while an uncommitted deletion might be visible to an
     * application thread doing eviction, the check for whether an internal page is evictable should
     * only allow committed deletions.
     */
    WT_ASSERT_ALWAYS(session, page_del->committed, "Uncommitted deletions cannot be written out");

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
    WT_ACQUIRE_READ(prepare_state, page_del->prepare_state);
    if (prepare_state == WT_PREPARE_INPROGRESS || prepare_state == WT_PREPARE_LOCKED) {
        WT_ASSERT_ALWAYS(session, !F_ISSET(r, WT_REC_EVICT),
          "In progress prepares should never be seen in eviction");
        WT_ASSERT(session, !visible_all);

        cmsp->state = WTI_CHILD_ORIGINAL;
        r->leave_dirty = true;
        return (0);
    }

    /*
     * If there are readers that might want to see the page's state before it's deleted, or the
     * fast-delete can be undone by RTS, we can't discard the pages. Write a cell to the internal
     * page with information describing the fast-delete.
     *
     * We have the WT_REF locked, but that lock is released before returning to the function writing
     * cells to the page. Copy out the current fast-truncate information for that function.
     */
    if (!visible_all) {
        cmsp->del = *page_del;
        cmsp->state = WTI_CHILD_PROXY;
        page_del->selected_for_write = true;
        return (0);
    }

    /*
     * Deal with underlying disk blocks.
     *
     * Globally visible truncate, discard the leaf page to the block manager and no cell needs to be
     * written. Done outside of the underlying tracking routines because this action is permanent
     * and irrevocable. (Clearing the address means we've lost track of the disk address in a
     * permanent way. This is safe because there's no path to reading the leaf page again: if there
     * is ever a read into this part of the name space again, the cache read function instantiates
     * an entirely new page.)
     */
    WT_RET(__wt_ref_block_free(session, ref, false));

    /* Globally visible fast-truncate information is never used again, a NULL value is identical. */
    __wt_overwrite_and_free(session, ref->page_del);

    return (0);
}

/*
 * __wti_rec_child_modify --
 *     Return if the internal page's child references any modifications.
 */
int
__wti_rec_child_modify(WT_SESSION_IMPL *session, WTI_RECONCILE *r, WT_REF *ref,
  WTI_CHILD_MODIFY_STATE *cmsp, bool *build_delta)
{
    WT_DECL_RET;
    WT_PAGE_MODIFY *mod;

    /* We may acquire a hazard pointer our caller must release. */
    cmsp->hazard = false;

    /* Default to using the original child address. */
    cmsp->state = WTI_CHILD_ORIGINAL;

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
        switch (r->tested_ref_state = WT_REF_GET_STATE(ref)) {
        case WT_REF_DISK:
            /* On disk, not modified by definition. */
            WT_ASSERT(session, ref->addr != NULL);
            /* DISK pages do not have fast-truncate info. */
            WT_ASSERT(session, ref->page_del == NULL);
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

            /* FIXME-WT-14879: support delta for fast truncate. */
            if (build_delta != NULL) {
                *build_delta = false;
                r->delta.size = 0;
            }
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
            WT_RET_ASSERT(session, WT_DIAGNOSTIC_EVICTION_CHECK, !F_ISSET(r, WT_REC_EVICT), EBUSY,
              "unexpected WT_REF_LOCKED child state during eviction reconciliation");

            /* If the page is being read from disk, it's not modified by definition. */
            if (F_ISSET_ATOMIC_8(ref, WT_REF_FLAG_READING))
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
            WT_RET_ASSERT(session, WT_DIAGNOSTIC_EVICTION_CHECK, !F_ISSET(r, WT_REC_EVICT), EBUSY,
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
             *
             * If we retried from below this point and already have a hazard pointer, don't do it
             * again.
             */
            if (cmsp->hazard == false) {
                ret = __wt_page_in(session, ref,
                  WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_INTERNAL_OP | WT_READ_NO_WAIT);
                if (ret == WT_NOTFOUND) {
                    ret = 0;
                    break;
                }
                WT_RET(ret);
                cmsp->hazard = true;
            }

            /*
             * The child is potentially modified if the page's modify structure has been created. If
             * the modify structure exists and the page has been reconciled, set that state.
             */
            mod = ref->page->modify;
            if (mod != NULL && mod->rec_result != 0) {
                cmsp->state = WTI_CHILD_MODIFIED;
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
             * in the ref after instantiation to make the visibility check possible.
             *
             * The key is the page-modify.instantiated flag, removed during page reconciliation. If
             * it's set, instantiation happened after checkpoint passed the leaf page and we treat
             * this page like a WT_REF_DELETED page, evaluating it as it was before instantiation.
             *
             * We need to lock the ref for it to be safe to examine the page_del structure, in case
             * the transaction in it is unresolved and tries to roll back (which discards the
             * structure) while we're looking at it. It should be possible to skip the locking if
             * the instantiation update list is NULL (that means the transaction is resolved) but
             * for now let's do the conservatively safe thing.
             */
            if (mod != NULL && mod->instantiated) {
                if (!WT_REF_CAS_STATE(session, ref, WT_REF_MEM, WT_REF_LOCKED))
                    /* Oops. Retry... */
                    break;

                /* This is a very small race window, but check just in case. */
                if (mod->instantiated == false) {
                    WT_REF_SET_STATE(ref, WT_REF_MEM);
                    /* Retry from the top; we may now have a rec_result. */
                    break;
                }

                WT_RET(__rec_child_deleted(session, r, ref, cmsp));
                WT_REF_SET_STATE(ref, WT_REF_MEM);
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
                cmsp->state = WTI_CHILD_IGNORE;
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
            WT_RET_ASSERT(session, WT_DIAGNOSTIC_EVICTION_CHECK, false, EBUSY,
              "unexpected WT_REF_SPLIT child state during reconciliation");
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
