/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __free_page_modify(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_col_var(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_int(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_page_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static void __free_skip_array(WT_SESSION_IMPL *, WT_INSERT_HEAD **, uint32_t, bool);
static void __free_skip_list(WT_SESSION_IMPL *, WT_INSERT *, bool);
static void __free_update(WT_SESSION_IMPL *, WT_UPDATE **, uint32_t, bool);

/*
 * __wt_ref_out --
 *     Discard an in-memory page, freeing all memory associated with it.
 */
void
__wt_ref_out(WT_SESSION_IMPL *session, WT_REF *ref)
{
    /*
     * A version of the page-out function that allows us to make additional diagnostic checks.
     *
     * The WT_REF cannot be the eviction thread's location.
     */
    WT_ASSERT(session, S2BT(session)->evict_ref != ref);

    /*
     * Make sure no other thread has a hazard pointer on the page we are about to discard. This is
     * complicated by the fact that readers publish their hazard pointer before re-checking the page
     * state, so our check can race with readers without indicating a real problem. If we find a
     * hazard pointer, wait for it to be cleared.
     */
    WT_ASSERT_OPTIONAL(session, WT_DIAGNOSTIC_EVICTION_CHECK,
      __wt_hazard_check_assert(session, ref, true),
      "Attempted to free a page with active hazard pointers");

    __wt_page_out(session, &ref->page);
}

/*
 * __wt_page_out --
 *     Discard an in-memory page, freeing all memory associated with it.
 */
void
__wt_page_out(WT_SESSION_IMPL *session, WT_PAGE **pagep)
{
    WT_PAGE *page;
    WT_PAGE_HEADER *dsk;
    WT_PAGE_MODIFY *mod;

    /*
     * Kill our caller's reference, do our best to catch races.
     */
    page = *pagep;
    *pagep = NULL;

    /*
     * Ensure that we are not evicting a page ahead of the materialization frontier, unless we are
     * simply discarding the page due to the dhandle being dead or the connection close.
     *
     * Here we are using the old_rec_lsn_max. This is because if we have done a dirty eviction, the
     * new value holds the max lsn that is reloaded to memory. If we have done a clean eviction of
     * the page that is read from the disk, the old value is the same as the new value. The only
     * exception is the clean eviction for a page that has been reconciled before. We should use the
     * new value but we cannot detect this case here.
     *
     * FIXME-WT-14720: this check needs a bit more thought. There isn't really an LSN that makes
     * sense as a comparison point. Scrub eviction leaves the content in the cache, so we won't
     * issue a read for the page we're evicting. That means we're free to write the page even if
     * it's ahead of the materialization frontier.
     */

    if (page->disagg_info != NULL &&
      !(F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
        F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING)))
        if (!__wt_page_materialization_check(session, page->disagg_info->old_rec_lsn_max))
            WT_STAT_CONN_DSRC_INCR(session, cache_eviction_ahead_of_last_materialized_lsn);

    /*
     * Unless we have a dead handle or we're closing the database, we should never discard a dirty
     * page. We do ordinary eviction from dead trees until sweep gets to them, so we may not in the
     * WT_SYNC_DISCARD loop.
     */
    if (F_ISSET(session->dhandle, WT_DHANDLE_DEAD) ||
      F_ISSET_ATOMIC_32(S2C(session), WT_CONN_CLOSING))
        __wt_page_modify_clear(session, page);

    WT_ASSERT_ALWAYS(session, !__wt_page_is_modified(page), "Attempting to discard dirty page");
    WT_ASSERT_ALWAYS(
      session, !__wt_page_is_reconciling(page), "Attempting to discard page being reconciled");
    WT_ASSERT_ALWAYS(session, !F_ISSET_ATOMIC_16(page, WT_PAGE_EVICT_LRU),
      "Attempting to discard page queued for eviction");

    /*
     * If a root page split, there may be one or more pages linked from the page; walk the list,
     * discarding pages.
     */
    switch (page->type) {
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        mod = page->modify;
        if (mod != NULL && mod->mod_root_split != NULL)
            __wt_page_out(session, &mod->mod_root_split);
        break;
    }

    /* Update the page history information for debugging. */
    WT_IGNORE_RET(__wt_conn_page_history_track_evict(session, page));

    /* Update the cache's information. */
    __wt_evict_page_cache_bytes_decr(session, page);

    dsk = (WT_PAGE_HEADER *)page->dsk;
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_ALLOC))
        __wt_cache_page_image_decr(session, page);

    /* Discard any mapped image. */
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_MAPPED))
        (void)S2BT(session)->bm->map_discard(
          S2BT(session)->bm, session, dsk, (size_t)dsk->mem_size);

    /*
     * If discarding the page as part of process exit, the application may configure to leak the
     * memory rather than do the work.
     */
    if (F_ISSET_ATOMIC_32(S2C(session), WT_CONN_LEAK_MEMORY))
        return;

    /* Free the page modification information. */
    if (page->modify != NULL)
        __free_page_modify(session, page);

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        __free_page_col_fix(session, page);
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        __free_page_int(session, page);
        break;
    case WT_PAGE_COL_VAR:
        __free_page_col_var(session, page);
        break;
    case WT_PAGE_ROW_LEAF:
        __free_page_row_leaf(session, page);
        break;
    }

    /* Discard any allocated disk image. */
    if (F_ISSET_ATOMIC_16(page, WT_PAGE_DISK_ALLOC))
        __wt_overwrite_and_free_len(session, dsk, dsk->mem_size);

    __wt_overwrite_and_free(session, page);
}

/*
 * __free_page_modify --
 *     Discard the page's associated modification structures.
 */
static void
__free_page_modify(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_INSERT_HEAD *append;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t i;
    bool update_ignore;

    mod = page->modify;

    /* In some failed-split cases, we can't discard updates. */
    update_ignore = F_ISSET_ATOMIC_16(page, WT_PAGE_UPDATE_IGNORE);

    switch (mod->rec_result) {
    case WT_PM_REC_MULTIBLOCK:
        /* Free list of replacement blocks. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            switch (page->type) {
            case WT_PAGE_ROW_INT:
            case WT_PAGE_ROW_LEAF:
                __wt_free(session, multi->key.ikey);
                break;
            }
            __wt_free(session, multi->supd);
            /*
             * Discard the new disk images if they are not NULL. If the new disk images are NULL,
             * they must have been instantiated into memory. Otherwise, we have a failure in
             * eviction after reconciliation. If the split code only successfully instantiates a
             * subset of new pages into memory, free the instantiated pages and the new disk images
             * of the pages not in memory. We will redo reconciliation next time we visit this page.
             */
            __wt_free(session, multi->disk_image);
            __wt_free(session, multi->addr.block_cookie);
            if (multi->block_meta != NULL)
                __wt_free(session, multi->block_meta);
        }
        __wt_free(session, mod->mod_multi);
        break;
    case WT_PM_REC_REPLACE:
        /*
         * Discard any replacement address: this memory is usually moved into the parent's WT_REF,
         * but at the root that can't happen.
         *
         * Discard the new disk image if it is not NULL. If the new disk image is NULL, it must have
         * been instantiated into memory. Otherwise, we have a failure in eviction after
         * reconciliation and later we decide to discard the old disk image without loading the new
         * disk image into memory. Free the new disk image in this case. If a checkpoint visits this
         * page, it would write the new disk image even it hasn't been instantiated into memory.
         * Therefore, no need to reconcile the page again if it remains clean.
         */
        __wt_free(session, mod->mod_replace.block_cookie);
        __wt_free(session, mod->mod_disk_image);
        break;
    }

    switch (page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        /* Free the append array. */
        if ((append = WT_COL_APPEND(page)) != NULL) {
            __free_skip_list(session, WT_SKIP_FIRST(append), update_ignore);
            __wt_free(session, append);
            __wt_free(session, mod->mod_col_append);
        }

        /* Free the insert/update array. */
        if (mod->mod_col_update != NULL)
            __free_skip_array(session, mod->mod_col_update,
              page->type == WT_PAGE_COL_FIX ? 1 : page->entries, update_ignore);
        break;
    case WT_PAGE_ROW_LEAF:
        /*
         * Free the insert array.
         *
         * Row-store tables have one additional slot in the insert array (the insert array has an
         * extra slot to hold keys that sort before keys found on the original page).
         */
        if (mod->mod_row_insert != NULL)
            __free_skip_array(session, mod->mod_row_insert, page->entries + 1, update_ignore);

        /* Free the update array. */
        if (mod->mod_row_update != NULL)
            __free_update(session, mod->mod_row_update, page->entries, update_ignore);
        break;
    }

    /* Free the overflow on-page and reuse skiplists. */
    __wt_ovfl_reuse_free(session, page);
    __wt_ovfl_discard_free(session, page);

    __wt_free(session, page->modify->ovfl_track);
    __wt_free(session, page->modify->inst_updates);
    __wt_free(session, page->modify->stop_ta);
    __wt_spin_destroy(session, &page->modify->page_lock);

    __wt_free(session, page->modify);
}

/*
 * __wti_ref_addr_safe_free --
 *     Any thread that is reviewing the address in a WT_REF, must also be holding a split generation
 *     to ensure that the page index they are using remains valid. Utilize the same generation type
 *     to safely free the address once all users of it have left the generation.
 */
void
__wti_ref_addr_safe_free(WT_SESSION_IMPL *session, void *p, size_t len)
{
    WT_DECL_RET;
    uint64_t split_gen;

    /*
     * The reading thread is always inside a split generation when it reads the ref, so we make use
     * of WT_GEN_SPLIT type generation mechanism to protect the address in a WT_REF rather than
     * creating a whole new generation counter. There are no page splits taking place.
     */
    split_gen = __wt_gen(session, WT_GEN_SPLIT);
    WT_TRET(__wt_stash_add(session, WT_GEN_SPLIT, split_gen, p, len));
    __wt_gen_next(session, WT_GEN_SPLIT, NULL);

    if (ret != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "fatal error during ref address free"));
}

/*
 * __wt_ref_addr_free --
 *     Free the address in a reference, if necessary.
 */
void
__wt_ref_addr_free(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_PAGE *home;
    void *ref_addr;

    /*
     * In order to free the WT_REF.addr field we need to read and clear the address without a race.
     * The WT_REF may be a child of a page being split, in which case the addr field could be
     * instantiated concurrently which changes the addr field. Once we swap in NULL we effectively
     * own the addr. Then provided the addr is off page we can free the memory.
     *
     * However as we could be the child of a page being split the ref->home pointer which tells us
     * whether the addr is on or off page could change concurrently. To avoid this we save the home
     * pointer before we do the compare and swap. While the second acquire read should be sufficient
     * we use an acquire read on the ref->home pointer as that is the standard mechanism to
     * guarantee we read the current value.
     *
     * We don't reread this value inside loop as if it was to change then we would be pointing at a
     * new parent, which would mean that our ref->addr must have been instantiated and thus we are
     * safe to free it at the end of this function.
     */
    WT_ACQUIRE_READ_WITH_BARRIER(home, ref->home);
    do {
        WT_ACQUIRE_READ_WITH_BARRIER(ref_addr, ref->addr);
        if (ref_addr == NULL)
            return;
    } while (!__wt_atomic_cas_ptr(&ref->addr, ref_addr, NULL));

    /* Encourage races. */
    if (FLD_ISSET(S2C(session)->timing_stress_flags, WT_TIMING_STRESS_SPLIT_8)) {
        __wt_yield();
        __wt_yield();
    }

    if (home == NULL || __wt_off_page(home, ref_addr)) {
        __wti_ref_addr_safe_free(
          session, ((WT_ADDR *)ref_addr)->block_cookie, ((WT_ADDR *)ref_addr)->block_cookie_size);
        __wti_ref_addr_safe_free(session, ref_addr, sizeof(WT_ADDR));
    }
}

/*
 * __wti_free_ref --
 *     Discard the contents of a WT_REF structure (optionally including the pages it references).
 */
void
__wti_free_ref(WT_SESSION_IMPL *session, WT_REF *ref, int page_type, bool free_pages)
{
    WT_IKEY *ikey;

    if (ref == NULL)
        return;

    /*
     * We create WT_REFs in many places, assert a WT_REF has been configured as either an internal
     * page or a leaf page, to catch any we've missed.
     */
    WT_ASSERT(session, F_ISSET(ref, WT_REF_FLAG_INTERNAL) || F_ISSET(ref, WT_REF_FLAG_LEAF));

    /*
     * Optionally free the referenced pages. (The path to free referenced page is used for error
     * cleanup, no instantiated and then discarded page should have WT_REF entries with real pages.
     * The page may have been marked dirty as well; page discard checks for that, so we mark it
     * clean explicitly.)
     */
    if (free_pages && ref->page != NULL) {
        WT_ASSERT_ALWAYS(session, !__wt_page_is_reconciling(ref->page),
          "Attempting to discard ref to a page being reconciled");
        __wt_page_modify_clear(session, ref->page);
        __wt_page_out(session, &ref->page);
    }

    /*
     * Optionally free row-store WT_REF key allocation. Historic versions of this code looked in a
     * passed-in page argument, but that is dangerous, some of our error-path callers create WT_REF
     * structures without ever setting WT_REF.home or having a parent page to which the WT_REF will
     * be linked. Those WT_REF structures invariably have instantiated keys, (they obviously cannot
     * be on-page keys), and we must free the memory.
     */
    switch (page_type) {
    case WT_PAGE_ROW_INT:
    case WT_PAGE_ROW_LEAF:
        if ((ikey = __wt_ref_key_instantiated(ref)) != NULL)
            __wt_free(session, ikey);
        break;
    }

    /* Free any address allocation. */
    __wt_ref_addr_free(session, ref);

    /* Free any backing fast-truncate memory. */
    __wt_free(session, ref->page_del);

    __wt_overwrite_and_free_len(session, ref, WT_REF_CLEAR_SIZE);
}

/*
 * __free_page_int --
 *     Discard a WT_PAGE_COL_INT or WT_PAGE_ROW_INT page.
 */
static void
__free_page_int(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_PAGE_INDEX *pindex;
    uint32_t i;

    WT_INTL_INDEX_GET_SAFE(page, pindex);
    for (i = 0; i < pindex->entries; ++i)
        __wti_free_ref(session, pindex->index[i], page->type, false);

    __wt_free(session, pindex);
}

/*
 * __wti_free_ref_index --
 *     Discard a page index and its references.
 */
void
__wti_free_ref_index(
  WT_SESSION_IMPL *session, WT_PAGE *page, WT_PAGE_INDEX *pindex, bool free_pages)
{
    WT_REF *ref;
    uint32_t i;

    if (pindex == NULL)
        return;

    WT_ASSERT_ALWAYS(session, !__wt_page_is_reconciling(page),
      "Attempting to discard ref to a page being reconciled");

    for (i = 0; i < pindex->entries; ++i) {
        ref = pindex->index[i];

        /*
         * Used when unrolling splits and other error paths where there should never have been a
         * hazard pointer taken.
         */
        WT_ASSERT_OPTIONAL(session, WT_DIAGNOSTIC_EVICTION_CHECK,
          __wt_hazard_check_assert(session, ref, false),
          "Attempting to discard ref to a page with hazard pointers");

        __wti_free_ref(session, ref, page->type, free_pages);
    }
    __wt_free(session, pindex);
}

/*
 * __free_page_col_fix --
 *     Discard a WT_PAGE_COL_FIX page.
 */
static void
__free_page_col_fix(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /* Free the time window lookup array. */
    __wt_free(session, page->u.col_fix.fix_tw);
}

/*
 * __free_page_col_var --
 *     Discard a WT_PAGE_COL_VAR page.
 */
static void
__free_page_col_var(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    /* Free the RLE lookup array. */
    __wt_free(session, page->u.col_var.repeats);
}

/*
 * __free_page_row_leaf --
 *     Discard a WT_PAGE_ROW_LEAF page.
 */
static void
__free_page_row_leaf(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_ROW *rip;
    uint32_t i;

    /* Free any allocated memory used by instantiated keys. */
    WT_ROW_FOREACH (page, rip, i)
        __wt_row_leaf_key_free(session, page, rip);
}

/*
 * __free_skip_array --
 *     Discard an array of skip list headers.
 */
static void
__free_skip_array(
  WT_SESSION_IMPL *session, WT_INSERT_HEAD **head_arg, uint32_t entries, bool update_ignore)
{
    WT_INSERT_HEAD **head;

    /*
     * For each non-NULL slot in the page's array of inserts, free the linked list anchored in that
     * slot.
     */
    for (head = head_arg; entries > 0; --entries, ++head)
        if (*head != NULL) {
            __free_skip_list(session, WT_SKIP_FIRST(*head), update_ignore);
            __wt_free(session, *head);
        }

    /* Free the header array. */
    __wt_free(session, head_arg);
}

/*
 * __free_skip_list --
 *     Walk a WT_INSERT forward-linked list and free the per-thread combination of a WT_INSERT
 *     structure and its associated chain of WT_UPDATE structures.
 */
static void
__free_skip_list(WT_SESSION_IMPL *session, WT_INSERT *ins, bool update_ignore)
{
    WT_INSERT *next;

    for (; ins != NULL; ins = next) {
        if (!update_ignore)
            __wt_free_update_list(session, &ins->upd);
        next = WT_SKIP_NEXT(ins);
        __wt_free(session, ins);
    }
}

/*
 * __free_update --
 *     Discard the update array.
 */
static void
__free_update(
  WT_SESSION_IMPL *session, WT_UPDATE **update_head, uint32_t entries, bool update_ignore)
{
    WT_UPDATE **updp;

    /*
     * For each non-NULL slot in the page's array of updates, free the linked list anchored in that
     * slot.
     */
    if (!update_ignore)
        for (updp = update_head; entries > 0; --entries, ++updp)
            __wt_free_update_list(session, updp);

    /* Free the update array. */
    __wt_free(session, update_head);
}

/*
 * __wt_free_update_list --
 *     Walk a WT_UPDATE forward-linked list and free the per-thread combination of a WT_UPDATE
 *     structure and its associated data.
 */
void
__wt_free_update_list(WT_SESSION_IMPL *session, WT_UPDATE **updp)
{
    WT_UPDATE *next, *upd;

    for (upd = *updp; upd != NULL; upd = next) {
        next = upd->next;
        __wt_free(session, upd);
    }
    *updp = NULL;
}

/*
 * __wt_free_obsolete_updates --
 *     Following a globally visible update, free any obsolete updates in the update chain. After a
 *     globally visible update, no reader finds any updates. It is the responsibility of the caller
 *     to lock the page before freeing the updates.
 */
void
__wt_free_obsolete_updates(WT_SESSION_IMPL *session, WT_PAGE *page, WT_UPDATE *visible_all_upd)
{
    WT_UPDATE *next, *upd;
    size_t delta_upd_size, size;

    delta_upd_size = size = 0;

    next = visible_all_upd->next;

    /*
     * No need to use a compare and swap because we have obtained a page lock. The page lock
     * protects freeing the updates concurrently by other threads. Whereas the reader threads use
     * transaction visibility to avoid traversing obsolete updates beyond the globally visible
     * update.
     */
    visible_all_upd->next = NULL;

    /* There must be at least a single obsolete update. */
    WT_ASSERT(session, next != NULL);

    for (upd = next; upd != NULL; upd = next) {
        next = upd->next;
        size += WT_UPDATE_MEMSIZE(upd);
        if (F_ISSET(upd, WT_UPDATE_RESTORED_FROM_DELTA))
            delta_upd_size += WT_UPDATE_MEMSIZE(upd);
        __wt_free(session, upd);
    }

    WT_ASSERT(session, size != 0);
    __wt_cache_page_inmem_decr(session, page, size);
    if (delta_upd_size != 0)
        __wt_cache_page_inmem_decr_delta_updates(session, page, delta_upd_size);
}
