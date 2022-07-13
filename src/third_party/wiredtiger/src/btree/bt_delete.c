/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Fast-delete support.
 *
 * This file contains most of the code that allows WiredTiger to delete pages of data without
 * reading them into the cache. (This feature is currently only available for row-store objects.)
 *
 * The way cursor truncate works in a row-store object is it explicitly reads the first and last
 * pages of the truncate range, then walks the tree with a flag so the tree walk code skips reading
 * eligible pages within the range and instead just marks them as deleted, by changing their WT_REF
 * state to WT_REF_DELETED. Pages ineligible for this fast path include pages already in the cache,
 * having overflow items, or requiring history store records. Ineligible pages are read and have
 * their rows updated/deleted individually. The transaction for the delete operation is stored in
 * memory referenced by the WT_REF.ft_info.del field.
 *
 * Future cursor walks of the tree will skip the deleted page based on the transaction stored for
 * the delete, but it gets more complicated if a read is done using a random key, or a cursor walk
 * is done with a transaction where the delete is not visible. In those cases, we read the original
 * contents of the page. The page-read code notices a deleted page is being read, and as part of the
 * read instantiates the contents of the page, creating a WT_UPDATE with a tombstone, in the same
 * transaction as deleted the page. In other words, the read process makes it appear as if the page
 * was read and each individual row deleted, exactly as would have happened if the page had been in
 * the cache all along.
 *
 * There's an additional complication to support rollback of the page delete. When the page was
 * marked deleted, a pointer to the WT_REF was saved in the deleting session's transaction list and
 * the delete is unrolled by resetting the WT_REF_DELETED state back to WT_REF_DISK. However, if the
 * page has been instantiated by some reading thread, that's not enough, each individual row on the
 * page must have the delete operation reset. If the page split, the WT_UPDATE lists might have been
 * saved/restored during reconciliation and appear on multiple pages, and the WT_REF stored in the
 * deleting session's transaction list is no longer useful. For this reason, when the page is
 * instantiated by a read, a list of the WT_UPDATE structures on the page is stored in the
 * WT_REF.ft_info.update field, that way the session resolving the delete can find all WT_UPDATE
 * structures that require update.
 *
 * One final note: pages can also be marked deleted if emptied and evicted. In that case, the WT_REF
 * state will be set to WT_REF_DELETED but there will not be any associated WT_REF.ft_info.del
 * field. These pages are always skipped during cursor traversal (the page could not have been
 * evicted if there were updates that weren't globally visible), and if read is forced to
 * instantiate such a page, it simply creates an empty page from scratch.
 */

/*
 * __wt_delete_page --
 *     If deleting a range, try to delete the page without instantiating it.
 */
int
__wt_delete_page(WT_SESSION_IMPL *session, WT_REF *ref, bool *skipp)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;
    uint8_t previous_state;

    *skipp = false;

    /* If we have a clean page in memory, attempt to evict it. */
    previous_state = ref->state;
    if (previous_state == WT_REF_MEM &&
      WT_REF_CAS_STATE(session, ref, previous_state, WT_REF_LOCKED)) {
        if (__wt_page_is_modified(ref->page)) {
            WT_REF_SET_STATE(ref, previous_state);
            return (0);
        }

        WT_RET(__wt_curhs_cache(session));
        (void)__wt_atomic_addv32(&S2BT(session)->evict_busy, 1);
        ret = __wt_evict(session, ref, previous_state, 0);
        (void)__wt_atomic_subv32(&S2BT(session)->evict_busy, 1);
        WT_RET_BUSY_OK(ret);
        ret = 0;
    }

    /*
     * Fast check to see if it's worth locking, then atomically switch the page's state to lock it.
     */
    previous_state = ref->state;
    switch (previous_state) {
    case WT_REF_DISK:
        break;
    default:
        return (0);
    }
    if (!WT_REF_CAS_STATE(session, ref, previous_state, WT_REF_LOCKED))
        return (0);

    /*
     * There should be no previous page-delete information: if the previous fast-truncate didn't
     * instantiate the page, then we'd never get here to do another delete; if the previous fast-
     * truncate did instantiate the page, then (for a read-write tree; we can't get here in a
     * readonly tree) any fast-truncate information was removed at that point and/or when the
     * fast-truncate transaction was resolved.
     */
    WT_ASSERT(session, ref->ft_info.del == NULL);

    /*
     * We cannot truncate pages that have overflow key/value items as the overflow blocks have to be
     * discarded. The way we figure that out is to check the page's cell type, cells for leaf pages
     * without overflow items are special.
     *
     * Additionally, if the page has prepared updates or the aggregated start time point on the page
     * is not visible to us then we cannot truncate the page.
     */
    if (!__wt_ref_addr_copy(session, ref, &addr))
        goto err;
    if (addr.type != WT_ADDR_LEAF_NO)
        goto err;
    if (addr.ta.prepare)
        goto err;
    /* History store data are always visible. No need to check visibility. */
    if (!WT_IS_HS(session->dhandle) &&
      !__wt_txn_visible(session, addr.ta.newest_txn,
        WT_MAX(addr.ta.newest_start_durable_ts, addr.ta.newest_stop_durable_ts)))
        goto err;

    /*
     * This action dirties the parent page: mark it dirty now, there's no future reconciliation of
     * the child leaf page that will dirty it as we write the tree.
     */
    WT_ERR(__wt_page_parent_modify_set(session, ref, false));

    /* Allocate and initialize the page-deleted structure. */
    WT_ERR(__wt_calloc_one(session, &ref->ft_info.del));
    ref->ft_info.del->previous_ref_state = previous_state;

    /* History store truncation is non-transactional. */
    if (!WT_IS_HS(session->dhandle))
        WT_ERR(__wt_txn_modify_page_delete(session, ref));

    *skipp = true;
    WT_STAT_CONN_DATA_INCR(session, rec_page_delete_fast);

    /* Publish the page to its new state, ensuring visibility. */
    WT_REF_SET_STATE(ref, WT_REF_DELETED);
    return (0);

err:
    __wt_free(session, ref->ft_info.del);

    /* Publish the page to its previous state, ensuring visibility. */
    WT_REF_SET_STATE(ref, previous_state);
    return (ret);
}

/*
 * __wt_delete_page_rollback --
 *     Transaction rollback for a fast-truncate operation.
 */
int
__wt_delete_page_rollback(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_UPDATE **updp;
    uint64_t sleep_usecs, yield_count;
    uint8_t current_state;
    bool locked;

    /* Lock the reference. We cannot access ref->ft_info.del except when locked. */
    for (locked = false, sleep_usecs = yield_count = 0;;) {
        switch (current_state = ref->state) {
        case WT_REF_LOCKED:
            break;
        case WT_REF_DELETED:
        case WT_REF_MEM:
        case WT_REF_SPLIT:
            if (WT_REF_CAS_STATE(session, ref, current_state, WT_REF_LOCKED))
                locked = true;
            break;
        case WT_REF_DISK:
        default:
            return (__wt_illegal_value(session, current_state));
        }

        if (locked)
            break;

        /*
         * We wait for the change in page state, yield before retrying, and if we've yielded enough
         * times, start sleeping so we don't burn CPU to no purpose.
         */
        __wt_spin_backoff(&yield_count, &sleep_usecs);
        WT_STAT_CONN_INCRV(session, page_del_rollback_blocked, sleep_usecs);
    }

    /*
     * There are two possible cases:
     *
     * 1. The state is WT_REF_DELETED. In this case ft_info.del cannot be null, because the
     * operation cannot reach global visibility while its transaction remains uncommitted. The page
     * itself is as we left it, so we can just reset the state.
     *
     * 2. The state is WT_REF_MEM. We check ft_info.update for a list of updates to abort. Allow the
     * update list to be null to be conservative.
     */
    if (current_state == WT_REF_DELETED)
        current_state = ref->ft_info.del->previous_ref_state;
    else {
        if ((updp = ref->ft_info.update) != NULL)
            /*
             * Walk any list of update structures and abort them. We can't use the normal read path
             * to get the pages with updates (the original page may have split, so there may be more
             * than one page), because the session may have closed the cursor, and we no longer have
             * the reference to the tree required for a hazard pointer. We're safe since pages with
             * unresolved transactions aren't going anywhere.
             */
            for (; *updp != NULL; ++updp)
                (*updp)->txnid = WT_TXN_ABORTED;
        WT_ASSERT(session, ref->page != NULL && ref->page->modify != NULL);
        /*
         * Drop any page_deleted information that has been moved to the modify structure. Note that
         * while this must have been an instantiated page, the information (and flag) is only kept
         * until the page is reconciled for the first time after instantiation, so it might not be
         * set now.
         */
        if (ref->page->modify->instantiated) {
            ref->page->modify->instantiated = false;
            __wt_free(session, ref->page->modify->page_del);
        }
    }

    /*
     * Don't set the WT_PAGE_DELETED transaction ID to aborted, discard any WT_UPDATE list or set
     * the committed flag; instead, discard the structures, it has the same effect. It's a single
     * call, they're a union of two pointers.
     */
    __wt_free(session, ref->ft_info.del);

    WT_REF_SET_STATE(ref, current_state);
    return (0);
}

/*
 * __delete_redo_window_cleanup_internal --
 *     Process one internal page for __wt_delete_redo_window_cleanup. This fixes up the transaction
 *     IDs in the delete info. Since we're called at the end of recovery there's no need to lock the
 *     ref or worry about races.
 */
static void
__delete_redo_window_cleanup_internal(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_REF *child;

    WT_ASSERT(session, F_ISSET(ref, WT_REF_FLAG_INTERNAL));
    if (ref->page != NULL) {
        WT_INTL_FOREACH_BEGIN (session, ref->page, child) {
            if (child->state == WT_REF_DELETED && child->ft_info.del != NULL)
                __cell_redo_page_del_cleanup(session, ref->page->dsk, child->ft_info.del);
        }
        WT_INTL_FOREACH_END;
    }
}

/*
 * __delete_redo_window_cleanup_skip --
 *     Tree-walk skip function for __wt_delete_redo_window_cleanup. This skips all leaf pages; we'll
 *     visit all in-memory internal pages via the flag settings on the tree-walk call.
 */
static int
__delete_redo_window_cleanup_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_UNUSED(ref);
    WT_UNUSED(session);
    WT_UNUSED(context);
    WT_UNUSED(visible_all);

    *skipp = F_ISSET(ref, WT_REF_FLAG_LEAF);
    return (0);
}

/*
 * __wt_delete_redo_window_cleanup --
 *     Clear old transaction IDs from already-loaded page_del structures to make them look like we
 *     just unpacked the information. Called after the tree write generation is bumped during
 *     recovery so that old transaction IDs don't come back to life. Note that this can only fail if
 *     something goes wrong in the tree walk; it doesn't itself ever fail.
 */
int
__wt_delete_redo_window_cleanup(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;
    WT_REF *ref;

    /*
     * Walk the tree and look for internal pages holding fast-truncate information. Note: we pass
     * WT_READ_VISIBLE_ALL because we have no snapshot, but we aren't actually doing any visibility
     * checks.
     */
    ref = NULL;
    while ((ret = __wt_tree_walk_custom_skip(session, &ref, __delete_redo_window_cleanup_skip, NULL,
              WT_READ_CACHE | WT_READ_VISIBLE_ALL)) == 0 &&
      ref != NULL)
        WT_WITH_PAGE_INDEX(session, __delete_redo_window_cleanup_internal(session, ref));

    return (ret);
}

/*
 * __wt_delete_page_skip --
 *     If iterating a cursor, skip deleted pages that are either visible to us or globally visible.
 */
bool
__wt_delete_page_skip(WT_SESSION_IMPL *session, WT_REF *ref, bool visible_all)
{
    bool skip;

    /*
     * Deleted pages come from two sources: either it's a truncate as described above, or the page
     * has been emptied by other operations and eviction deleted it.
     *
     * In both cases, the WT_REF state will be WT_REF_DELETED. In the case of a truncated page,
     * there will be a WT_PAGE_DELETED structure with the transaction ID of the transaction that
     * deleted the page, and the page is visible if that transaction ID is visible. In the case of
     * an empty page, there will be no WT_PAGE_DELETED structure and the delete is by definition
     * visible, eviction could not have deleted the page if there were changes on it that were not
     * globally visible.
     *
     * We're here because we found a WT_REF state set to WT_REF_DELETED. It is possible the page is
     * being read into memory right now, though, and the page could switch to an in-memory state at
     * any time. Lock down the structure, just to be safe.
     */
    if (!WT_REF_CAS_STATE(session, ref, WT_REF_DELETED, WT_REF_LOCKED))
        return (false);

    skip = !__wt_page_del_active(session, ref, visible_all);

    /*
     * The fast-truncate structure can be freed as soon as the delete is stable: it is only read
     * when the ref state is locked. It is worth checking every time we come through because once
     * this is freed, we no longer need synchronization to check the ref.
     *
     * Note that if the visible_all flag is set, skip already reflects the visible_all result so we
     * don't need to do it twice.
     */
    if (skip && ref->ft_info.del != NULL &&
      (visible_all ||
        __wt_txn_visible_all(
          session, ref->ft_info.del->txnid, ref->ft_info.del->durable_timestamp)))
        __wt_overwrite_and_free(session, ref->ft_info.del);

    WT_REF_SET_STATE(ref, WT_REF_DELETED);
    return (skip);
}

/*
 * __tombstone_update_alloc --
 *     Allocate and initialize a page-deleted tombstone update structure.
 */
static int
__tombstone_update_alloc(
  WT_SESSION_IMPL *session, WT_PAGE_DELETED *page_del, WT_UPDATE **updp, size_t *sizep)
{
    WT_UPDATE *upd;

    WT_RET(__wt_upd_alloc_tombstone(session, &upd, sizep));
    F_SET(upd, WT_UPDATE_RESTORED_FAST_TRUNCATE);

    /*
     * Cleared memory matches the lowest possible transaction ID and timestamp, do nothing.
     */
    if (page_del != NULL) {
        upd->txnid = page_del->txnid;
        upd->durable_ts = page_del->durable_timestamp;
        upd->start_ts = page_del->timestamp;
        upd->prepare_state = page_del->prepare_state;
    }
    *updp = upd;
    return (0);
}

/*
 * __wt_delete_page_instantiate --
 *     Instantiate an entirely deleted row-store leaf page.
 */
int
__wt_delete_page_instantiate(WT_SESSION_IMPL *session, WT_REF *ref, WT_PAGE_DELETED *page_del)
{
    WT_DECL_RET;
    WT_PAGE *page;
    WT_ROW *rip;
    WT_TIME_WINDOW tw;
    WT_UPDATE **upd_array, **update_list, *upd;
    size_t size, total_size;
    uint32_t count, i;

    /*
     * An operation is accessing a "deleted" page, and we're building an in-memory version of the
     * page (making it look like all entries in the page were individually updated by a remove
     * operation). We end up here if a transaction used a truncate call to delete the page without
     * reading it, and something else that can't yet see the truncation decided to read the page.
     *
     * This can happen after the truncate transaction resolves, but it can also happen before. In
     * the latter case, we need to keep track of the updates we populate the page with, so they can
     * be found when the transaction resolves. The page we're loading might split, in which case
     * finding the updates any other way would become a problem.
     *
     * The page_del structure passed in is either ref->ft_info.del, or under certain circumstances
     * when that's unavailable, one extracted from the parent page's address cell.
     */

    page = ref->page;
    update_list = NULL;

    /* For now fast-truncate is only supported for row-store. */
    WT_ASSERT(session, page->type == WT_PAGE_ROW_LEAF);

    WT_STAT_CONN_DATA_INCR(session, cache_read_deleted);

    /* Track the prepared, fast-truncate pages we've had to instantiate. */
    if (page_del != NULL && page_del->prepare_state != WT_PREPARE_INIT)
        WT_STAT_CONN_DATA_INCR(session, cache_read_deleted_prepared);

    /*
     * Give the page a modify structure and mark the page dirty if the tree isn't read-only. If the
     * tree can be written, the page must be marked dirty: otherwise it can be discarded, and that
     * will lose the truncate information if the parent page hasn't been reconciled since the
     * truncation happened.
     *
     * If the tree cannot be written (checked in page-modify-set), we won't dirty the page. In this
     * case the truncate information must have been read from the parent page's on-disk cell, so we
     * can fetch it again if we discard the page and then reread it.
     *
     * Truncates can appear in read-only trees (whether a read-only open of the live database or via
     * a checkpoint cursor) if they were not yet globally visible when the tree was checkpointed.
     */
    WT_RET(__wt_page_modify_init(session, page));
    __wt_page_modify_set(session, page);

    /* Allocate the per-page update array if one doesn't already exist. */
    if (page->entries != 0 && page->modify->mod_row_update == NULL)
        WT_PAGE_ALLOC_AND_SWAP(
          session, page, page->modify->mod_row_update, upd_array, page->entries);

    /*
     * Copy the page-deleted structure's timestamp information into an update for each row on the
     * page. If the page-deleted structure is NULL, that means the truncate is globally visible, and
     * therefore committed.
     *
     * If the truncate operation is not yet resolved, link updates in the page-deleted structure so
     * they can be found when the transaction is resolved, even if they have moved to other pages.
     */
    if (page_del != NULL && !page_del->committed) {
        count = 0;
        WT_ROW_FOREACH (page, rip, i)
            ++count;
        WT_RET(__wt_calloc_def(session, count + 1, &update_list));
    }

    total_size = size = 0;
    count = 0;
    upd_array = page->modify->mod_row_update;

    /* We just read the page and it's still locked. The insert lists should be empty. */
    WT_ASSERT(session, WT_ROW_INSERT_SMALLEST(page) == NULL);

    /* Walk the page entries, giving each one a tombstone. */
    WT_ROW_FOREACH (page, rip, i) {
        /*
         * Retrieve the stop time point from the page's row. If we find an existing stop time point
         * we don't need to append a tombstone. Such rows would not have been visible to the
         * original truncate operation and were, logically, skipped over rather than re-deleted.
         */
        __wt_read_row_time_window(session, page, rip, &tw);
        if (!WT_TIME_WINDOW_HAS_STOP(&tw)) {
            WT_ERR(__tombstone_update_alloc(session, page_del, &upd, &size));
            total_size += size;
            upd->next = upd_array[WT_ROW_SLOT(page, rip)];
            upd_array[WT_ROW_SLOT(page, rip)] = upd;

            if (update_list != NULL)
                update_list[count++] = upd;
        }

        /* We just read the page and it's still locked. The insert lists should be empty. */
        WT_ASSERT(session, WT_ROW_INSERT(page, rip) == NULL);
    }

    /*
     * Move the WT_PAGE_DELETED structure to page->modify; all of its information has been copied to
     * the list of WT_UPDATE structures (if any), but we may still need it for internal page
     * reconciliation.
     *
     * Note: when the page_del passed in isn't the one in the ref, there should be none in the ref.
     * This only happens in readonly trees (see bt_page.c) and is a consequence of it being possible
     * for a deleted page to be in WT_REF_DISK state if it's already been instantiated once and then
     * evicted. In this case we can set modify->page_del to NULL regardless of the truncation's
     * visibility (rather than copying the passed-in information); modify->page_del is only used by
     * parent-page reconciliation and readonly trees shouldn't ever reach that code.
     */
    WT_ASSERT(session, page_del == ref->ft_info.del || ref->ft_info.del == NULL);
    page->modify->instantiated = true;
    page->modify->page_del = ref->ft_info.del;
    /* We don't need to null ft_info.del because assigning ft_info.update overwrites it. */
    ref->ft_info.update = update_list;

    __wt_cache_page_inmem_incr(session, page, total_size);

    return (0);

err:
    __wt_free(session, update_list);
    return (ret);
}
