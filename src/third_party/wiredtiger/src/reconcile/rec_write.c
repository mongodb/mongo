/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __rec_cleanup(WT_SESSION_IMPL *, WT_RECONCILE *);
static void __rec_destroy(WT_SESSION_IMPL *, void *);
static void __rec_destroy_session(WT_SESSION_IMPL *);
static int __rec_init(WT_SESSION_IMPL *, WT_REF *, uint32_t, WT_SALVAGE_COOKIE *, void *);
static int __rec_hs_wrapup(WT_SESSION_IMPL *, WT_RECONCILE *);
static int __rec_root_write(WT_SESSION_IMPL *, WT_PAGE *, uint32_t);
static int __rec_split_discard(WT_SESSION_IMPL *, WT_PAGE *);
static int __rec_split_row_promote(WT_SESSION_IMPL *, WT_RECONCILE *, WT_ITEM *, uint8_t);
static int __rec_split_write(WT_SESSION_IMPL *, WT_RECONCILE *, WT_REC_CHUNK *, WT_ITEM *, bool);
static void __rec_write_page_status(WT_SESSION_IMPL *, WT_RECONCILE *);
static int __rec_write_err(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int __rec_write_wrapup(WT_SESSION_IMPL *, WT_RECONCILE *, WT_PAGE *);
static int __reconcile(WT_SESSION_IMPL *, WT_REF *, WT_SALVAGE_COOKIE *, uint32_t, bool *);

/*
 * __wt_reconcile --
 *     Reconcile an in-memory page into its on-disk format, and write it.
 */
int
__wt_reconcile(WT_SESSION_IMPL *session, WT_REF *ref, WT_SALVAGE_COOKIE *salvage, uint32_t flags)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    uint64_t start, now;
    bool no_reconcile_set, page_locked;

    btree = S2BT(session);
    page = ref->page;

    __wt_seconds(session, &start);

    __wt_verbose(session, WT_VERB_RECONCILE, "%p reconcile %s (%s%s)", (void *)ref,
      __wt_page_type_string(page->type), LF_ISSET(WT_REC_EVICT) ? "evict" : "checkpoint",
      LF_ISSET(WT_REC_HS) ? ", history store" : "");

    /*
     * Sanity check flags.
     *
     * If we try to do eviction using transaction visibility, we had better have a snapshot. This
     * doesn't apply to checkpoints: there are (rare) cases where we write data at read-uncommitted
     * isolation.
     */
    WT_ASSERT(session,
      !LF_ISSET(WT_REC_EVICT) || LF_ISSET(WT_REC_VISIBLE_ALL) ||
        F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT));

    /* Can't do history store eviction for history store itself or for metadata. */
    WT_ASSERT(session,
      !LF_ISSET(WT_REC_HS) || (!WT_IS_HS(btree->dhandle) && !WT_IS_METADATA(btree->dhandle)));
    /* Flag as unused for non diagnostic builds. */
    WT_UNUSED(btree);

    /* It's an error to be called with a clean page. */
    WT_ASSERT(session, __wt_page_is_modified(page));

    /*
     * Reconciliation acquires and releases pages, and in rare cases that page release triggers
     * eviction. If the page is dirty, eviction can trigger reconciliation, and we re-enter this
     * code. Reconciliation isn't re-entrant, so we need to ensure that doesn't happen.
     */
    no_reconcile_set = F_ISSET(session, WT_SESSION_NO_RECONCILE);
    F_SET(session, WT_SESSION_NO_RECONCILE);

    /*
     * Reconciliation locks the page for two reasons:
     *    Reconciliation reads the lists of page updates, obsolete updates
     * cannot be discarded while reconciliation is in progress;
     *    In-memory splits: reconciliation of an internal page cannot handle
     * a child page splitting during the reconciliation.
     */
    WT_PAGE_LOCK(session, page);
    page_locked = true;

    /*
     * Now that the page is locked, if attempting to evict it, check again whether eviction is
     * permitted. The page's state could have changed while we were waiting to acquire the lock
     * (e.g., the page could have split).
     */
    if (LF_ISSET(WT_REC_EVICT) && !__wt_page_can_evict(session, ref, NULL))
        WT_ERR(__wt_set_return(session, EBUSY));

    /*
     * Reconcile the page. The reconciliation code unlocks the page as soon as possible, and returns
     * that information.
     */
    ret = __reconcile(session, ref, salvage, flags, &page_locked);

    /* If writing a page in service of compaction, we're done, clear the flag. */
    F_CLR_ATOMIC_16(ref->page, WT_PAGE_COMPACTION_WRITE);

err:
    if (page_locked)
        WT_PAGE_UNLOCK(session, page);
    if (!no_reconcile_set)
        F_CLR(session, WT_SESSION_NO_RECONCILE);

    /* Track the longest reconciliation, ignoring races (it's just a statistic). */
    __wt_seconds(session, &now);
    if (now - start > S2C(session)->rec_maximum_seconds)
        S2C(session)->rec_maximum_seconds = now - start;

    return (ret);
}

/*
 * __reconcile_save_evict_state --
 *     Save the transaction state that causes history to be pinned, whether reconciliation succeeds
 *     or fails.
 */
static void
__reconcile_save_evict_state(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
    WT_PAGE_MODIFY *mod;
    uint64_t oldest_id;

    mod = ref->page->modify;
    oldest_id = __wt_txn_oldest_id(session);

    /*
     * During eviction, save the transaction state that causes history to be pinned, regardless of
     * whether reconciliation succeeds or fails. There is usually no point retrying eviction until
     * this state changes.
     */
    if (LF_ISSET(WT_REC_EVICT)) {
        mod->last_eviction_id = oldest_id;
        __wt_txn_pinned_timestamp(session, &mod->last_eviction_timestamp);
        mod->last_evict_pass_gen = S2C(session)->cache->evict_pass_gen;
    }

#ifdef HAVE_DIAGNOSTIC
    /*
     * Check that transaction time always moves forward for a given page. If this check fails,
     * reconciliation can free something that a future reconciliation will need.
     */
    WT_ASSERT(session, WT_TXNID_LE(mod->last_oldest_id, oldest_id));
    mod->last_oldest_id = oldest_id;
#endif
}

/*
 * __reconcile --
 *     Reconcile an in-memory page into its on-disk format, and write it.
 */
static int
__reconcile(WT_SESSION_IMPL *session, WT_REF *ref, WT_SALVAGE_COOKIE *salvage, uint32_t flags,
  bool *page_lockedp)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_RECONCILE *r;
#ifdef HAVE_DIAGNOSTIC
    void *addr;
#endif

    btree = S2BT(session);
    page = ref->page;

    /* Save the eviction state. */
    __reconcile_save_evict_state(session, ref, flags);

    /* Initialize the reconciliation structure for each new run. */
    WT_RET(__rec_init(session, ref, flags, salvage, &session->reconcile));
    r = session->reconcile;

    /* Reconcile the page. */
    switch (page->type) {
    case WT_PAGE_COL_FIX:
        ret = __wt_rec_col_fix(session, r, ref, salvage);
        break;
    case WT_PAGE_COL_INT:
        WT_WITH_PAGE_INDEX(session, ret = __wt_rec_col_int(session, r, ref));
        break;
    case WT_PAGE_COL_VAR:
        ret = __wt_rec_col_var(session, r, ref, salvage);
        break;
    case WT_PAGE_ROW_INT:
        WT_WITH_PAGE_INDEX(session, ret = __wt_rec_row_int(session, r, page));
        break;
    case WT_PAGE_ROW_LEAF:
        ret = __wt_rec_row_leaf(session, r, ref, salvage);
        break;
    default:
        ret = __wt_illegal_value(session, page->type);
        break;
    }

    /*
     * Update the global history store score. Only use observations during eviction, not checkpoints
     * and don't count eviction of the history store table itself.
     */
    if (F_ISSET(r, WT_REC_EVICT) && !WT_IS_HS(btree->dhandle))
        __wt_cache_update_hs_score(session, r->updates_seen, r->updates_unstable);

    /*
     * If eviction didn't use any updates and didn't split or delete the page, it didn't make
     * progress. Give up rather than silently succeeding in doing no work: this way threads know to
     * back off forced eviction rather than spinning.
     *
     * Do not return an error if we are syncing the file with eviction disabled or as part of a
     * checkpoint.
     */
    if (ret == 0 && !(btree->evict_disabled > 0 || !F_ISSET(btree->dhandle, WT_DHANDLE_OPEN)) &&
      F_ISSET(r, WT_REC_EVICT) && !WT_PAGE_IS_INTERNAL(r->page) && r->multi_next == 1 &&
      F_ISSET(r, WT_REC_CALL_URGENT) && !r->update_used && r->cache_write_restore)
        ret = __wt_set_return(session, EBUSY);

#ifdef HAVE_DIAGNOSTIC
    addr = ref->addr;
#endif

    /*
     * If we fail the reconciliation prior to calling __rec_write_wrapup then we can clean up our
     * state and return an error.
     *
     * If we fail the reconciliation after calling __rec_write_wrapup then we must panic as
     * inserting updates to the history store and then failing can leave us in a bad state.
     */
    if (ret != 0) {
        /* Make sure that reconciliation doesn't free the page that has been written to disk. */
        WT_ASSERT(session, addr == NULL || ref->addr != NULL);
        WT_TRET(__rec_write_err(session, r, page));
    } else {
        /* Wrap up the page reconciliation. Panic on failure. */
        if ((ret = __rec_write_wrapup(session, r, page)) != 0)
            goto panic;
        __rec_write_page_status(session, r);
    }

    /* Release the reconciliation lock. */
    *page_lockedp = false;
    WT_PAGE_UNLOCK(session, page);

    /* Update statistics. */
    WT_STAT_CONN_INCR(session, rec_pages);
    WT_STAT_DATA_INCR(session, rec_pages);
    if (LF_ISSET(WT_REC_EVICT))
        WT_STAT_CONN_DATA_INCR(session, rec_pages_eviction);
    if (r->cache_write_hs)
        WT_STAT_CONN_DATA_INCR(session, cache_write_hs);
    if (r->cache_write_restore)
        WT_STAT_CONN_DATA_INCR(session, cache_write_restore);
    if (!WT_IS_HS(btree->dhandle)) {
        if (r->rec_page_cell_with_txn_id)
            WT_STAT_CONN_INCR(session, rec_pages_with_txn);
        if (r->rec_page_cell_with_ts)
            WT_STAT_CONN_INCR(session, rec_pages_with_ts);
        if (r->rec_page_cell_with_prepared_txn)
            WT_STAT_CONN_INCR(session, rec_pages_with_prepare);
    }
    if (r->multi_next > btree->rec_multiblock_max)
        btree->rec_multiblock_max = r->multi_next;

    /* Clean up the reconciliation structure. */
    __rec_cleanup(session, r);

    /*
     * When threads perform eviction, don't cache block manager structures (even across calls), we
     * can have a significant number of threads doing eviction at the same time with large items.
     * Ignore checkpoints, once the checkpoint completes, all unnecessary session resources will be
     * discarded.
     */
    if (!WT_SESSION_IS_CHECKPOINT(session)) {
        /*
         * Clean up the underlying block manager memory too: it's not reconciliation, but threads
         * discarding reconciliation structures want to clean up the block manager's structures as
         * well, and there's no obvious place to do that.
         */
        if (session->block_manager_cleanup != NULL) {
            if (ret == 0 && (ret = session->block_manager_cleanup(session)) != 0) {
                goto panic;
            } else
                WT_TRET(session->block_manager_cleanup(session));
        }

        __rec_destroy_session(session);
    }
    /*
     * This return statement covers non-panic error scenarios, any failure beyond this point is a
     * panic.
     */
    WT_RET(ret);

    /*
     * Root pages are special, splits have to be done, we can't put it off as the parent's problem
     * any more.
     */
    if (__wt_ref_is_root(ref)) {
        WT_WITH_PAGE_INDEX(session, ret = __rec_root_write(session, page, flags));
        if (ret != 0)
            goto panic;
        return (0);
    }

    /*
     * Otherwise, mark the page's parent dirty. Don't mark the tree dirty: if this reconciliation is
     * in service of a checkpoint, it's cleared the tree's dirty flag, and we don't want to set it
     * again as part of that walk.
     */
    if ((ret = __wt_page_parent_modify_set(session, ref, true)) == 0)
        return (0);

panic:
    WT_RET_PANIC(session, ret, "reconciliation failed after building the disk image");
}

/*
 * __rec_write_page_status --
 *     Set the page status after reconciliation.
 */
static void
__rec_write_page_status(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    WT_PAGE_MODIFY *mod;

    btree = S2BT(session);
    page = r->page;
    mod = page->modify;

    /*
     * Set the page's status based on whether or not we cleaned the page.
     */
    if (r->leave_dirty) {
        /*
         * The page remains dirty.
         *
         * Any checkpoint call cleared the tree's modified flag before writing pages, so we must
         * explicitly reset it. We insert a barrier after the change for clarity (the requirement is
         * the flag be set before a subsequent checkpoint reads it, and as the current checkpoint is
         * waiting on this reconciliation to complete, there's no risk of that happening).
         */
        btree->modified = true;
        WT_FULL_BARRIER();
        if (!S2C(session)->modified)
            S2C(session)->modified = true;

        /*
         * Eviction should only be here if allowing writes to history store or in the in-memory
         * eviction case. Otherwise, we must be reconciling the metadata (which does not allow
         * history store content).
         */
        WT_ASSERT(session,
          !F_ISSET(r, WT_REC_EVICT) ||
            (F_ISSET(r, WT_REC_HS | WT_REC_IN_MEMORY) || WT_IS_METADATA(btree->dhandle)));
    } else {
        /*
         * Track the page's maximum transaction ID (used to decide if we can evict a clean page and
         * discard its history).
         */
        mod->rec_max_txn = r->max_txn;
        mod->rec_max_timestamp = r->max_ts;

        /*
         * Track the tree's maximum transaction ID (used to decide if it's safe to discard the
         * tree). Reconciliation for eviction is multi-threaded, only update the tree's maximum
         * transaction ID when doing a checkpoint. That's sufficient, we only care about the maximum
         * transaction ID of current updates in the tree, and checkpoint visits every dirty page in
         * the tree.
         */
        if (!F_ISSET(r, WT_REC_EVICT)) {
            if (WT_TXNID_LT(btree->rec_max_txn, r->max_txn))
                btree->rec_max_txn = r->max_txn;
            if (btree->rec_max_timestamp < r->max_ts)
                btree->rec_max_timestamp = r->max_ts;
        }

        /*
         * We set the page state to mark it as having been dirtied for the first time prior to
         * reconciliation. A failed atomic cas indicates that an update has taken place during
         * reconciliation.
         *
         * The page only might be clean; if the page state is unchanged since reconciliation
         * started, it's clean.
         *
         * If the page state changed, the page has been written since reconciliation started and
         * remains dirty (that can't happen when evicting, the page is exclusively locked).
         */
        if (__wt_atomic_cas32(&mod->page_state, WT_PAGE_DIRTY_FIRST, WT_PAGE_CLEAN))
            __wt_cache_dirty_decr(session, page);
        else
            WT_ASSERT(session, !F_ISSET(r, WT_REC_EVICT));
    }
}

/*
 * __rec_root_write --
 *     Handle the write of a root page.
 */
static int
__rec_root_write(WT_SESSION_IMPL *session, WT_PAGE *page, uint32_t flags)
{
    WT_DECL_RET;
    WT_PAGE *next;
    WT_PAGE_INDEX *pindex;
    WT_PAGE_MODIFY *mod;
    WT_REF fake_ref;
    uint32_t i;

    mod = page->modify;

    /*
     * If a single root page was written (either an empty page or there was a 1-for-1 page swap),
     * we've written root and checkpoint, we're done. Clear the result of the reconciliation, a root
     * page never has the structures that would normally be associated with (at least), the
     * replaced-object flag. If the root page split, write the resulting WT_REF array. We already
     * have an infrastructure for writing pages, create a fake root page and write it instead of
     * adding code to write blocks based on the list of blocks resulting from a multiblock
     * reconciliation.
     *
     */
    switch (mod->rec_result) {
    case WT_PM_REC_EMPTY:   /* Page is empty */
    case WT_PM_REC_REPLACE: /* 1-for-1 page swap */
        mod->rec_result = 0;
        return (0);
    case WT_PM_REC_MULTIBLOCK: /* Multiple blocks */
        break;
    default:
        return (__wt_illegal_value(session, mod->rec_result));
    }

    __wt_verbose(
      session, WT_VERB_SPLIT, "root page split -> %" PRIu32 " pages", mod->mod_multi_entries);

    /*
     * Create a new root page, initialize the array of child references, mark it dirty, then write
     * it.
     *
     * Don't count the eviction of this page as progress, checkpoint can repeatedly create and
     * discard these pages.
     */
    WT_RET(__wt_page_alloc(session, page->type, mod->mod_multi_entries, false, &next));
    F_SET_ATOMIC_16(next, WT_PAGE_EVICT_NO_PROGRESS);

    WT_INTL_INDEX_GET(session, next, pindex);
    for (i = 0; i < mod->mod_multi_entries; ++i) {
        /*
         * There's special error handling required when re-instantiating pages in memory; it's not
         * needed here, asserted for safety.
         */
        WT_ASSERT(session, mod->mod_multi[i].supd == NULL);
        WT_ASSERT(session, mod->mod_multi[i].disk_image == NULL);

        WT_ERR(
          __wt_multi_to_ref(session, next, &mod->mod_multi[i], &pindex->index[i], NULL, false));
        pindex->index[i]->home = next;
    }

    /*
     * We maintain a list of pages written for the root in order to free the backing blocks the next
     * time the root is written.
     */
    mod->mod_root_split = next;

    /*
     * Mark the page dirty. Don't mark the tree dirty: if this reconciliation is in service of a
     * checkpoint, it's cleared the tree's dirty flag, and we don't want to set it again as part of
     * that walk.
     */
    WT_ERR(__wt_page_modify_init(session, next));
    __wt_page_only_modify_set(session, next);

    /*
     * Fake up a reference structure, and write the next root page.
     */
    __wt_root_ref_init(session, &fake_ref, next, page->type == WT_PAGE_COL_INT);
    return (__wt_reconcile(session, &fake_ref, NULL, flags));

err:
    __wt_page_out(session, &next);
    return (ret);
}

/*
 * __rec_init --
 *     Initialize the reconciliation structure.
 */
static int
__rec_init(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags, WT_SALVAGE_COOKIE *salvage,
  void *reconcilep)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *page;
    WT_RECONCILE *r;
    WT_TXN_GLOBAL *txn_global;
    uint64_t ckpt_txn;

    btree = S2BT(session);
    page = ref->page;

    /*
     * Reconciliation is not re-entrant, make sure that doesn't happen. Our caller sets
     * WT_SESSION_IMPL.WT_SESSION_NO_RECONCILE to prevent it, but it's been a problem in the past,
     * check to be sure.
     */
    r = *(WT_RECONCILE **)reconcilep;
    if (r != NULL && r->ref != NULL)
        WT_RET_MSG(session, WT_ERROR, "reconciliation re-entered");

    if (r == NULL) {
        WT_RET(__wt_calloc_one(session, &r));
        session->reconcile_cleanup = __rec_destroy_session;

        /* Connect pointers/buffers. */
        r->cur = &r->_cur;
        r->last = &r->_last;

        /* Disk buffers need to be aligned for writing. */
        F_SET(&r->chunk_A.image, WT_ITEM_ALIGNED);
        F_SET(&r->chunk_B.image, WT_ITEM_ALIGNED);
    }

    /* Remember the configuration. */
    r->ref = ref;
    r->page = page;

    /*
     * Save the transaction generations before reading the page. These are all ordered reads, but we
     * only need one.
     */
    r->orig_btree_checkpoint_gen = btree->checkpoint_gen;
    r->orig_txn_checkpoint_gen = __wt_gen(session, WT_GEN_CHECKPOINT);

    /*
     * Update the page state to indicate that all currently installed updates will be included in
     * this reconciliation if it would mark the page clean.
     *
     * Add a write barrier to make it more likely that a thread adding an update will see this state
     * change.
     */
    page->modify->page_state = WT_PAGE_DIRTY_FIRST;
    WT_FULL_BARRIER();

    /*
     * Cache the oldest running transaction ID. This is used to check whether updates seen by
     * reconciliation have committed. We keep a cached copy to avoid races where a concurrent
     * transaction could abort while reconciliation is examining its updates. This way, any
     * transaction running when reconciliation starts is considered uncommitted.
     */
    txn_global = &S2C(session)->txn_global;
    WT_ORDERED_READ(r->last_running, txn_global->last_running);

    /*
     * Cache the pinned timestamp and oldest id, these are used to when we clear obsolete timestamps
     * and ids from time windows later in reconciliation.
     */
    __wt_txn_pinned_timestamp(session, &r->rec_start_pinned_ts);
    r->rec_start_oldest_id = __wt_txn_oldest_id(session);

    /*
     * The checkpoint transaction doesn't pin the oldest txn id, therefore the global last_running
     * can move beyond the checkpoint transaction id. When reconciling the metadata, we have to take
     * checkpoints into account.
     */
    if (WT_IS_METADATA(session->dhandle)) {
        WT_ORDERED_READ(ckpt_txn, txn_global->checkpoint_txn_shared.id);
        if (ckpt_txn != WT_TXN_NONE && WT_TXNID_LT(ckpt_txn, r->last_running))
            r->last_running = ckpt_txn;
    }
    /* When operating on the history store table, we should never try history store eviction. */
    WT_ASSERT(session, !F_ISSET(btree->dhandle, WT_DHANDLE_HS) || !LF_ISSET(WT_REC_HS));

    /*
     * History store table eviction is configured when eviction gets aggressive, adjust the flags
     * for cases we don't support.
     */

    r->flags = flags;

    /* Track the page's min/maximum transaction */
    r->max_txn = WT_TXN_NONE;
    r->max_ts = WT_TS_NONE;
    r->min_skipped_ts = WT_TS_MAX;

    /* Track if updates were used and/or uncommitted. */
    r->updates_seen = r->updates_unstable = 0;
    r->update_used = false;

    /* Track if the page can be marked clean. */
    r->leave_dirty = false;

    /* Track overflow items. */
    r->ovfl_items = false;

    /* Track empty values. */
    r->all_empty_value = true;
    r->any_empty_value = false;

    /* The list of saved updates is reused. */
    r->supd_next = 0;
    r->supd_memsize = 0;

    /* The list of pages we've written. */
    r->multi = NULL;
    r->multi_next = 0;
    r->multi_allocated = 0;

    r->wrapup_checkpoint = NULL;
    r->wrapup_checkpoint_compressed = false;

    r->evict_matching_checksum_failed = false;

    /*
     * Dictionary compression only writes repeated values once. We grow the dictionary as necessary,
     * always using the largest size we've seen.
     *
     * Reset the dictionary.
     *
     * Sanity check the size: 100 slots is the smallest dictionary we use.
     */
    if (btree->dictionary != 0 && btree->dictionary > r->dictionary_slots)
        WT_ERR(
          __wt_rec_dictionary_init(session, r, btree->dictionary < 100 ? 100 : btree->dictionary));
    __wt_rec_dictionary_reset(r);

    /*
     * Prefix compression discards repeated prefix bytes from row-store leaf page keys.
     */
    r->key_pfx_compress_conf = false;
    if (btree->prefix_compression && page->type == WT_PAGE_ROW_LEAF)
        r->key_pfx_compress_conf = true;

    /*
     * Suffix compression shortens internal page keys by discarding trailing bytes that aren't
     * necessary for tree navigation. We don't do suffix compression if there is a custom collator
     * because we don't know what bytes a custom collator might use. Some custom collators (for
     * example, a collator implementing reverse ordering of strings), won't have any problem with
     * suffix compression: if there's ever a reason to implement suffix compression for custom
     * collators, we can add a setting to the collator, configured when the collator is added, that
     * turns on suffix compression.
     */
    r->key_sfx_compress_conf = false;
    if (btree->collator == NULL && btree->internal_key_truncate)
        r->key_sfx_compress_conf = true;

    r->is_bulk_load = false;

    r->salvage = salvage;

    r->cache_write_hs = r->cache_write_restore = false;

    /*
     * The fake cursor used to figure out modified update values points to the enclosing WT_REF as a
     * way to access the page, and also needs to set the format.
     */
    r->update_modify_cbt.ref = ref;
    r->update_modify_cbt.iface.value_format = btree->value_format;
    r->update_modify_cbt.upd_value = &r->update_modify_cbt._upd_value;

    /* Clear stats related data. */
    r->rec_page_cell_with_ts = false;
    r->rec_page_cell_with_txn_id = false;
    r->rec_page_cell_with_prepared_txn = false;

/*
 * If we allocated the reconciliation structure and there was an error, clean up. If our caller
 * passed in a structure, they own it.
 */
err:
    if (*(WT_RECONCILE **)reconcilep == NULL) {
        if (ret == 0)
            *(WT_RECONCILE **)reconcilep = r;
        else {
            __rec_cleanup(session, r);
            __rec_destroy(session, &r);
        }
    }

    return (ret);
}

/*
 * __rec_cleanup --
 *     Clean up after a reconciliation run, except for structures cached across runs.
 */
static void
__rec_cleanup(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_MULTI *multi;
    uint32_t i;

    btree = S2BT(session);

    if (btree->type == BTREE_ROW)
        for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
            __wt_free(session, multi->key.ikey);
    for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i) {
        __wt_free(session, multi->disk_image);
        __wt_free(session, multi->supd);
        __wt_free(session, multi->addr.addr);
    }
    __wt_free(session, r->multi);

    /* Reconciliation is not re-entrant, make sure that doesn't happen. */
    r->ref = NULL;
}

/*
 * __rec_destroy --
 *     Clean up the reconciliation structure.
 */
static void
__rec_destroy(WT_SESSION_IMPL *session, void *reconcilep)
{
    WT_RECONCILE *r;

    if ((r = *(WT_RECONCILE **)reconcilep) == NULL)
        return;
    *(WT_RECONCILE **)reconcilep = NULL;

    __wt_buf_free(session, &r->chunk_A.key);
    __wt_buf_free(session, &r->chunk_A.min_key);
    __wt_buf_free(session, &r->chunk_A.image);
    __wt_buf_free(session, &r->chunk_B.key);
    __wt_buf_free(session, &r->chunk_B.min_key);
    __wt_buf_free(session, &r->chunk_B.image);

    __wt_free(session, r->supd);

    __wt_rec_dictionary_free(session, r);

    __wt_buf_free(session, &r->k.buf);
    __wt_buf_free(session, &r->v.buf);
    __wt_buf_free(session, &r->_cur);
    __wt_buf_free(session, &r->_last);

    __wt_buf_free(session, &r->update_modify_cbt.iface.value);
    __wt_buf_free(session, &r->update_modify_cbt._upd_value.buf);

    __wt_free(session, r);
}

/*
 * __rec_destroy_session --
 *     Clean up the reconciliation structure, session version.
 */
static void
__rec_destroy_session(WT_SESSION_IMPL *session)
{
    __rec_destroy(session, &session->reconcile);
}

/*
 * __rec_leaf_page_max_slvg --
 *     Figure out the maximum leaf page size for a salvage reconciliation.
 */
static inline uint32_t
__rec_leaf_page_max_slvg(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_PAGE *page;
    uint32_t page_size;

    btree = S2BT(session);
    page = r->page;

    page_size = 0;
    switch (page->type) {
    case WT_PAGE_COL_FIX:
        /*
         * Column-store pages can grow if there are missing records (that is, we lost a chunk of the
         * range, and have to write deleted records). Fixed-length objects are a problem, if there's
         * a big missing range, we could theoretically have to write large numbers of missing
         * objects.
         *
         * The code in rec_col.c already figured this out for us, including both space for missing
         * chunks of the namespace and space for time windows, so we will take what it says. Thus,
         * we shouldn't come here.
         */
        WT_ASSERT(session, false);
        break;
    case WT_PAGE_COL_VAR:
        /*
         * Column-store pages can grow if there are missing records (that is, we lost a chunk of the
         * range, and have to write deleted records). Variable-length objects aren't usually a
         * problem because we can write any number of deleted records in a single page entry because
         * of the RLE, we just need to ensure that additional entry fits.
         */
        break;
    case WT_PAGE_ROW_LEAF:
    default:
        /*
         * Row-store pages can't grow, salvage never does anything other than reduce the size of a
         * page read from disk.
         */
        break;
    }

    /*
     * Default size for variable-length column-store and row-store pages during salvage is the
     * maximum leaf page size.
     */
    if (page_size < btree->maxleafpage)
        page_size = btree->maxleafpage;

    /*
     * The page we read from the disk should be smaller than the page size we just calculated, check
     * out of paranoia.
     */
    if (page_size < page->dsk->mem_size)
        page_size = page->dsk->mem_size;

    /*
     * Salvage is the backup plan: don't let this fail.
     */
    return (page_size * 2);
}

/*
 * __wt_split_page_size --
 *     Given a split percentage, calculate split page size in bytes.
 */
uint32_t
__wt_split_page_size(int split_pct, uint32_t maxpagesize, uint32_t allocsize)
{
    uintmax_t a;
    uint32_t split_size;

    /*
     * Ideally, the split page size is some percentage of the maximum page size rounded to an
     * allocation unit (round to an allocation unit so we don't waste space when we write).
     */
    a = maxpagesize; /* Don't overflow. */
    split_size = (uint32_t)WT_ALIGN_NEAREST((a * (u_int)split_pct) / 100, allocsize);

    /*
     * Respect the configured split percentage if the calculated split size is either zero or a full
     * page. The user has either configured an allocation size that matches the page size, or a
     * split percentage that is close to zero or one hundred. Rounding is going to provide a worse
     * outcome than having a split point that doesn't fall on an allocation size boundary in those
     * cases.
     */
    if (split_size == 0 || split_size == maxpagesize)
        split_size = (uint32_t)((a * (u_int)split_pct) / 100);

    return (split_size);
}

/*
 * __rec_split_chunk_init --
 *     Initialize a single chunk structure.
 */
static int
__rec_split_chunk_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_CHUNK *chunk)
{
    chunk->recno = WT_RECNO_OOB;
    /* Don't touch the key item memory, that memory is reused. */
    chunk->key.size = 0;
    chunk->entries = 0;
    WT_TIME_AGGREGATE_INIT_MERGE(&chunk->ta);

    chunk->min_recno = WT_RECNO_OOB;
    /* Don't touch the key item memory, that memory is reused. */
    chunk->min_key.size = 0;
    chunk->min_entries = 0;
    WT_TIME_AGGREGATE_INIT_MERGE(&chunk->ta_min);
    chunk->min_offset = 0;

    /*
     * Allocate and clear the disk image buffer.
     *
     * Don't touch the disk image item memory, that memory is reused.
     *
     * Clear the disk page header to ensure all of it is initialized, even the unused fields.
     *
     * In the case of fixed-length column-store, clear the entire buffer: fixed-length column-store
     * sets bits in bytes, where the bytes are assumed to initially be 0.
     *
     * FUTURE: pretty sure clearing the whole buffer is not necessary; also in any event the
     * auxiliary space doesn't need to be cleared.
     */
    WT_RET(__wt_buf_init(session, &chunk->image, r->disk_img_buf_size));
    memset(chunk->image.mem, 0,
      r->page->type == WT_PAGE_COL_FIX ? r->disk_img_buf_size : WT_PAGE_HEADER_SIZE);

    return (0);
}

/*
 * __wt_rec_split_init --
 *     Initialization for the reconciliation split functions.
 */
int
__wt_rec_split_init(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page, uint64_t recno,
  uint64_t primary_size, uint32_t auxiliary_size)
{
    /* FUTURE: primary_size should probably also be 32 bits. */

    WT_BM *bm;
    WT_BTREE *btree;
    WT_REC_CHUNK *chunk;
    WT_REF *ref;
    size_t corrected_page_size;

    btree = S2BT(session);
    bm = btree->bm;

    /*
     * The maximum leaf page size governs when an in-memory leaf page splits into multiple on-disk
     * pages; however, salvage can't be allowed to split, there's no parent page yet. If we're doing
     * salvage, override the caller's selection of a maximum page size, choosing a page size that
     * ensures we won't split.
     *
     * For FLCS, the salvage page size can get very large indeed if pieces of the namespace have
     * vanished, so don't second-guess the caller, who's figured it out for us.
     */
    if (r->salvage != NULL && page->type != WT_PAGE_COL_FIX)
        primary_size = __rec_leaf_page_max_slvg(session, r);

    /*
     * Set the page sizes.
     *
     * Only fixed-length column store pages use auxiliary space; this is where time windows are
     * placed. r->page_size is the complete page size; we'll use r->space_avail to track how much
     * more primary space is remaining, and r->aux_space_avail to track how much more auxiliary
     * space there is.
     *
     * Because (for FLCS) we need to start writing time windows into the auxiliary space before we
     * know for sure how much bitmap data there is, we always start the time window data at a fixed
     * offset from the page start: the place where it goes naturally if the page is full. If the
     * page is not full (and there was at least one timestamp to write), we waste the intervening
     * unused space. Odd-sized pages are supposed to be rare (ideally only the last page in the
     * tree, though currently there are some other ways they can appear) so only a few KB is wasted
     * and not enough to be particularly concerned about.
     *
     * For FLCS, primary_size will always be the tree's configured maximum leaf page size, except
     * for pages created or rewritten during salvage, which might be larger. (This is not ideal,
     * because once created larger they cannot be split again later, but for the moment at least it
     * isn't readily avoided.)
     */
    WT_ASSERT(session, auxiliary_size == 0 || page->type == WT_PAGE_COL_FIX);
    r->page_size = (uint32_t)(primary_size + auxiliary_size);

    /*
     * If we have to split, we want to choose a smaller page size for the split pages, because
     * otherwise we could end up splitting one large packed page over and over. We don't want to
     * pick the minimum size either, because that penalizes an application that did a bulk load and
     * subsequently inserted a few items into packed pages. Currently defaulted to 75%, but I have
     * no empirical evidence that's "correct".
     *
     * The maximum page size may be a multiple of the split page size (for example, there's a
     * maximum page size of 128KB, but because the table is active and we don't want to split a lot,
     * the split size is 20KB). The maximum page size may NOT be an exact multiple of the split page
     * size.
     *
     * It's lots of work to build these pages and don't want to start over when we reach the maximum
     * page size (it's painful to restart after creating overflow items and compacted data, for
     * example, as those items have already been written to disk). So, the loop calls the helper
     * functions when approaching a split boundary, and we save the information at that point. We
     * also save the boundary information at the minimum split size. We maintain two chunks (each
     * boundary represents a chunk that gets written as a page) in the memory, writing out the older
     * one to the disk as a page when we need to make space for a new chunk. On reaching the last
     * chunk, if it turns out to be smaller than the minimum split size, we go back into the
     * penultimate chunk and split at this minimum split size boundary. This moves some data from
     * the penultimate chunk to the last chunk, hence increasing the size of the last page written
     * without decreasing the penultimate page size beyond the minimum split size.
     *
     * FLCS pages are different, because they have two pieces: bitmap data ("primary") and time
     * window data ("auxiliary"); the bitmap data is supposed to be a fixed amount per page. FLCS
     * pages therefore split based on the bitmap size, and the time window data comes along for the
     * ride no matter how large it is. If the time window data gets larger than expected (it can at
     * least in theory get rather large), we have to realloc the page image.
     *
     * Finally, all this doesn't matter at all for salvage; as noted above, in salvage we can't
     * split at all.
     */
    if (page->type == WT_PAGE_COL_FIX) {
        r->split_size = r->salvage != NULL ? 0 : btree->maxleafpage;
        r->space_avail = primary_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
        r->aux_space_avail = auxiliary_size - WT_COL_FIX_AUXHEADER_RESERVATION;
    } else if (r->salvage != NULL) {
        r->split_size = 0;
        r->space_avail = r->page_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
    } else {
        r->split_size = __wt_split_page_size(btree->split_pct, r->page_size, btree->allocsize);
        r->space_avail = r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
        r->min_split_size =
          __wt_split_page_size(WT_BTREE_MIN_SPLIT_PCT, r->page_size, btree->allocsize);
        r->min_space_avail = r->min_split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
    }

    /*
     * Ensure the disk image buffer is large enough for the max object, as corrected by the
     * underlying block manager.
     *
     * Since we want to support split_size values larger than the page size (to allow for
     * adjustments based on the compression), this buffer should be the greater of split_size and
     * page_size, then aligned to the next allocation size boundary. The latter shouldn't be an
     * issue, but it's a possible scenario if, for example, the compression engine is expected to
     * give us 5x compression and gives us nothing at all.
     */
    corrected_page_size = r->page_size;
    WT_RET(bm->write_size(bm, session, &corrected_page_size));
    r->disk_img_buf_size = WT_ALIGN(WT_MAX(corrected_page_size, r->split_size), btree->allocsize);

    /* Initialize the first split chunk. */
    WT_RET(__rec_split_chunk_init(session, r, &r->chunk_A));
    r->cur_ptr = &r->chunk_A;
    r->prev_ptr = NULL;

    /* Starting record number, entries, first free byte. */
    r->recno = recno;
    r->entries = 0;
    r->first_free = WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem);

    if (page->type == WT_PAGE_COL_FIX) {
        r->aux_start_offset = (uint32_t)(primary_size + WT_COL_FIX_AUXHEADER_RESERVATION);
        r->aux_entries = 0;
        r->aux_first_free = (uint8_t *)r->cur_ptr->image.mem + r->aux_start_offset;
    }

    /* New page, compression off. */
    r->key_pfx_compress = r->key_sfx_compress = false;

    /* Set the first chunk's key. */
    chunk = r->cur_ptr;
    if (btree->type == BTREE_ROW) {
        ref = r->ref;
        if (__wt_ref_is_root(ref))
            WT_RET(__wt_buf_set(session, &chunk->key, "", 1));
        else
            __wt_ref_key(ref->home, ref, &chunk->key.data, &chunk->key.size);
    } else
        chunk->recno = recno;

    return (0);
}

/*
 * __rec_is_checkpoint --
 *     Return if we're writing a checkpoint.
 */
static bool
__rec_is_checkpoint(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;

    btree = S2BT(session);

    /*
     * Check to see if we're going to create a checkpoint.
     *
     * This function exists as a place to hang this comment.
     *
     * Any time we write the root page of the tree without splitting we are creating a checkpoint
     * (and have to tell the underlying block manager so it creates and writes the additional
     * information checkpoints require). However, checkpoints are completely consistent, and so we
     * have to resolve information about the blocks we're expecting to free as part of the
     * checkpoint, before writing the checkpoint. In short, we don't do checkpoint writes here;
     * clear the boundary information as a reminder and create the checkpoint during wrapup.
     */
    return (!F_ISSET(btree, WT_BTREE_NO_CHECKPOINT) && __wt_ref_is_root(r->ref));
}

/*
 * __rec_split_row_promote --
 *     Key promotion for a row-store.
 */
static int
__rec_split_row_promote(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_ITEM *key, uint8_t type)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(update);
    WT_DECL_RET;
    WT_ITEM *max;
    WT_SAVE_UPD *supd;
    size_t cnt, len, size;
    uint32_t i;
    const uint8_t *pa, *pb;
    int cmp;

    /*
     * For a column-store, the promoted key is the recno and we already have a copy. For a
     * row-store, it's the first key on the page, a variable-length byte string, get a copy.
     *
     * This function is called from the split code at each split boundary, but that means we're not
     * called before the first boundary, and we will eventually have to get the first key explicitly
     * when splitting a page.
     *
     * For the current slot, take the last key we built, after doing suffix compression. The "last
     * key we built" describes some process: before calling the split code, we must place the last
     * key on the page before the boundary into the "last" key structure, and the first key on the
     * page after the boundary into the "current" key structure, we're going to compare them for
     * suffix compression.
     *
     * Suffix compression is a hack to shorten keys on internal pages. We only need enough bytes in
     * the promoted key to ensure searches go to the correct page: the promoted key has to be larger
     * than the last key on the leaf page preceding it, but we don't need any more bytes than that.
     * In other words, we can discard any suffix bytes not required to distinguish between the key
     * being promoted and the last key on the leaf page preceding it. This can only be done for the
     * first level of internal pages, you cannot repeat suffix truncation as you split up the tree,
     * it loses too much information.
     *
     * Note #1: if the last key on the previous page was an overflow key, we don't have the
     * in-memory key against which to compare, and don't try to do suffix compression. The code for
     * that case turns suffix compression off for the next key, we don't have to deal with it here.
     */
    if (type != WT_PAGE_ROW_LEAF || !r->key_sfx_compress)
        return (__wt_buf_set(session, key, r->cur->data, r->cur->size));

    btree = S2BT(session);
    WT_RET(__wt_scr_alloc(session, 0, &update));

    /*
     * Note #2: if we skipped updates, an update key may be larger than the last key stored in the
     * previous block (probable for append-centric workloads). If there are skipped updates and we
     * cannot evict the page, check for one larger than the last key and smaller than the current
     * key.
     */
    max = r->last;
    if (r->cache_write_restore)
        for (i = r->supd_next; i > 0; --i) {
            supd = &r->supd[i - 1];
            if (supd->ins == NULL)
                WT_ERR(__wt_row_leaf_key(session, r->page, supd->rip, update, false));
            else {
                update->data = WT_INSERT_KEY(supd->ins);
                update->size = WT_INSERT_KEY_SIZE(supd->ins);
            }

            /* Compare against the current key, it must be less. */
            WT_ERR(__wt_compare(session, btree->collator, update, r->cur, &cmp));
            if (cmp >= 0)
                continue;

            /* Compare against the last key, it must be greater. */
            WT_ERR(__wt_compare(session, btree->collator, update, r->last, &cmp));
            if (cmp >= 0)
                max = update;

            /*
             * The saved updates are in key-sort order so the entry we're looking for is either the
             * last or the next-to- last one in the list. Once we've compared an entry against the
             * last key on the page, we're done.
             */
            break;
        }

    /*
     * The largest key on the last block must sort before the current key, so we'll either find a
     * larger byte value in the current key, or the current key will be a longer key, and the
     * interesting byte is one past the length of the shorter key.
     */
    pa = max->data;
    pb = r->cur->data;
    len = WT_MIN(max->size, r->cur->size);
    size = len + 1;
    for (cnt = 1; len > 0; ++cnt, --len, ++pa, ++pb)
        if (*pa != *pb) {
            if (size != cnt) {
                WT_STAT_DATA_INCRV(session, rec_suffix_compression, size - cnt);
                size = cnt;
            }
            break;
        }
    ret = __wt_buf_set(session, key, r->cur->data, size);

err:
    __wt_scr_free(session, &update);
    return (ret);
}

/*
 * __wt_rec_split_grow --
 *     Grow the split buffer.
 */
int
__wt_rec_split_grow(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t add_len)
{
    WT_BM *bm;
    WT_BTREE *btree;
    size_t aux_first_free, corrected_page_size, first_free, inuse;

    aux_first_free = 0; /* gcc -Werror=maybe-uninitialized, with -O3 */
    btree = S2BT(session);
    bm = btree->bm;

    /* The free space is tracked with a pointer; convert to an integer. */
    first_free = WT_PTRDIFF(r->first_free, r->cur_ptr->image.mem);
    if (r->page->type == WT_PAGE_COL_FIX)
        aux_first_free = WT_PTRDIFF(r->aux_first_free, r->cur_ptr->image.mem);

    inuse = r->page->type == WT_PAGE_COL_FIX ? aux_first_free : first_free;
    corrected_page_size = inuse + add_len;

    WT_RET(bm->write_size(bm, session, &corrected_page_size));
    WT_RET(__wt_buf_grow(session, &r->cur_ptr->image, corrected_page_size));

    WT_ASSERT(session, corrected_page_size >= inuse);

    /* Convert the free space back to pointers. */
    r->first_free = (uint8_t *)r->cur_ptr->image.mem + first_free;
    if (r->page->type == WT_PAGE_COL_FIX)
        r->aux_first_free = (uint8_t *)r->cur_ptr->image.mem + aux_first_free;

    /* Adjust the available space. */
    if (r->page->type == WT_PAGE_COL_FIX) {
        /* Reallocating an FLCS page increases the auxiliary space. */
        r->aux_space_avail = corrected_page_size - aux_first_free;
        WT_ASSERT(session, r->aux_space_avail >= add_len);
    } else {
        r->space_avail = corrected_page_size - first_free;
        WT_ASSERT(session, r->space_avail >= add_len);
    }

    return (0);
}

/* The minimum number of entries before we'll split a row-store internal page. */
#define WT_PAGE_INTL_MINIMUM_ENTRIES 20

/*
 * __wt_rec_split --
 *     Handle the page reconciliation bookkeeping. (Did you know "bookkeeper" has 3 doubled letters
 *     in a row? Sweet-tooth does, too.)
 */
int
__wt_rec_split(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
{
    WT_BTREE *btree;
    WT_REC_CHUNK *tmp;
    size_t inuse;

    btree = S2BT(session);

    /*
     * We should never split during salvage, and we're about to drop core because there's no parent
     * page.
     */
    if (r->salvage != NULL)
        WT_RET_PANIC(session, WT_PANIC, "%s page too large, attempted split during salvage",
          __wt_page_type_string(r->page->type));

    /*
     * We can get here if the first key/value pair won't fit. Grow the buffer to contain the current
     * item if we haven't already consumed a reasonable portion of a split chunk. This logic should
     * not trigger for FLCS, because FLCS splits happen at very definite places; and if it does, the
     * interaction between here and there will corrupt the database, so assert otherwise.
     *
     * If we're promoting huge keys into an internal page, we might be about to write an internal
     * page with too few items, which isn't good for tree depth or search. Grow the buffer to
     * contain the current item if we don't have enough items to split an internal page.
     */
    inuse = WT_PTRDIFF(r->first_free, r->cur_ptr->image.mem);
    if (inuse < r->split_size / 2 && !__wt_rec_need_split(r, 0)) {
        WT_ASSERT(session, r->page->type != WT_PAGE_COL_FIX);
        goto done;
    }

    if (r->page->type == WT_PAGE_ROW_INT && r->entries < WT_PAGE_INTL_MINIMUM_ENTRIES)
        goto done;

    /* All page boundaries reset the dictionary. */
    __wt_rec_dictionary_reset(r);

    /* Set the entries, timestamps and size for the just finished chunk. */
    r->cur_ptr->entries = r->entries;
    if (r->page->type == WT_PAGE_COL_FIX && (r->cur_ptr->auxentries = r->aux_entries) != 0)
        r->cur_ptr->image.size = WT_PTRDIFF(r->aux_first_free, r->cur_ptr->image.mem);
    else
        r->cur_ptr->image.size = inuse;

    /*
     * Normally we keep two chunks in memory at a given time, and we write the previous chunk at
     * each boundary, switching the previous and current check references. The exception is when
     * doing a bulk load.
     */
    if (r->is_bulk_load)
        WT_RET(__rec_split_write(session, r, r->cur_ptr, NULL, false));
    else {
        if (r->prev_ptr != NULL)
            WT_RET(__rec_split_write(session, r, r->prev_ptr, NULL, false));

        if (r->prev_ptr == NULL) {
            WT_RET(__rec_split_chunk_init(session, r, &r->chunk_B));
            r->prev_ptr = &r->chunk_B;
        }
        tmp = r->prev_ptr;
        r->prev_ptr = r->cur_ptr;
        r->cur_ptr = tmp;
    }

    /* Initialize the next chunk, including the key. */
    WT_RET(__rec_split_chunk_init(session, r, r->cur_ptr));
    r->cur_ptr->recno = r->recno;
    if (btree->type == BTREE_ROW)
        WT_RET(__rec_split_row_promote(session, r, &r->cur_ptr->key, r->page->type));

    /* Reset tracking information. */
    r->entries = 0;
    r->first_free = WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem);

    if (r->page->type == WT_PAGE_COL_FIX) {
        /*
         * In the first chunk, we use the passed-in primary size, whatever it is, as the size for
         * the bitmap data; the auxiliary space follows it. It might be larger than the configured
         * maximum leaf page size if we're in salvage. For the second and subsequent chunks, we
         * aren't in salvage so always use the maximum leaf page size; that will produce the fixed
         * size pages we want.
         */
        r->aux_start_offset = btree->maxleafpage + WT_COL_FIX_AUXHEADER_RESERVATION;
        r->aux_entries = 0;
        r->aux_first_free = (uint8_t *)r->cur_ptr->image.mem + r->aux_start_offset;
    }

    /*
     * Set the space available to another split-size and minimum split-size chunk. For FLCS,
     * min_space_avail and min_split_size are both left as zero.
     */
    r->space_avail = r->split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);
    if (r->page->type == WT_PAGE_COL_FIX) {
        r->aux_space_avail = r->page_size - btree->maxleafpage - WT_COL_FIX_AUXHEADER_RESERVATION;
    } else
        r->min_space_avail = r->min_split_size - WT_PAGE_HEADER_BYTE_SIZE(btree);

done:
    /*
     * We may have declined the split as described above, in which case grow the buffer based on the
     * next key/value pair's length. In the internal page minimum-key case, we could grow more than
     * a single key/value pair's length to avoid repeatedly calling this function, but we'd prefer
     * not to have internal pages that are larger than they need to be, and repeatedly trying to
     * split means we will split as soon as we can.
     *
     * Also, overflow values can be larger than the maximum page size but still be "on-page". If the
     * next key/value pair is larger than space available after a split has happened (in other
     * words, larger than the maximum page size), create a page sized to hold that one key/value
     * pair. This generally splits the page into key/value pairs before a large object, the object,
     * and key/value pairs after the object. It's possible other key/value pairs will also be
     * aggregated onto the bigger page before or after, if the page happens to hold them, but it
     * won't necessarily happen that way.
     */
    if (r->space_avail < next_len)
        WT_RET(__wt_rec_split_grow(session, r, next_len));

    return (0);
}

/*
 * __wt_rec_split_crossing_bnd --
 *     Save the details for the minimum split size boundary or call for a split.
 */
int
__wt_rec_split_crossing_bnd(WT_SESSION_IMPL *session, WT_RECONCILE *r, size_t next_len)
{
    /*
     * If crossing the minimum split size boundary, store the boundary details at the current
     * location in the buffer. If we are crossing the split boundary at the same time, possible when
     * the next record is large enough, just split at this point.
     */
    if (WT_CROSSING_MIN_BND(r, next_len) && !WT_CROSSING_SPLIT_BND(r, next_len) &&
      !__wt_rec_need_split(r, 0)) {
        /*
         * If the first record doesn't fit into the minimum split size, we end up here. Write the
         * record without setting a boundary here. We will get the opportunity to setup a boundary
         * before writing out the next record.
         */
        if (r->entries == 0)
            return (0);

        r->cur_ptr->min_entries = r->entries;
        r->cur_ptr->min_recno = r->recno;
        if (S2BT(session)->type == BTREE_ROW)
            WT_RET(__rec_split_row_promote(session, r, &r->cur_ptr->min_key, r->page->type));
        WT_TIME_AGGREGATE_COPY(&r->cur_ptr->ta_min, &r->cur_ptr->ta);

        /* Assert we're not re-entering this code. */
        WT_ASSERT(session, r->cur_ptr->min_offset == 0);
        r->cur_ptr->min_offset = WT_PTRDIFF(r->first_free, r->cur_ptr->image.mem);

        /* All page boundaries reset the dictionary. */
        __wt_rec_dictionary_reset(r);

        return (0);
    }

    /* We are crossing a split boundary */
    return (__wt_rec_split(session, r, next_len));
}

/*
 * __rec_split_finish_process_prev --
 *     If the two split chunks together fit in a single page, merge them into one. If they do not
 *     fit in a single page but the last is smaller than the minimum desired, move some data from
 *     the penultimate chunk to the last chunk and write out the previous/penultimate. Finally,
 *     update the pointer to the current image buffer. After this function exits, we will have one
 *     (last) buffer in memory, pointed to by the current image pointer.
 */
static int
__rec_split_finish_process_prev(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_PAGE_HEADER *dsk;
    WT_REC_CHUNK *cur_ptr, *prev_ptr, *tmp;
    size_t combined_size, len_to_move;
    uint8_t *cur_dsk_start;

    WT_ASSERT(session, r->prev_ptr != NULL);

    btree = S2BT(session);
    cur_ptr = r->cur_ptr;
    prev_ptr = r->prev_ptr;

    /*
     * The sizes in the chunk include the header, so when calculating the combined size, be sure not
     * to include the header twice.
     */
    combined_size = prev_ptr->image.size + (cur_ptr->image.size - WT_PAGE_HEADER_BYTE_SIZE(btree));

    if (combined_size <= r->page_size) {
        /* This won't work for FLCS pages, so make sure we don't get here by accident. */
        WT_ASSERT(session, r->page->type != WT_PAGE_COL_FIX);

        /*
         * We have two boundaries, but the data in the buffers can fit a single page. Merge the
         * boundaries and create a single chunk.
         */
        prev_ptr->entries += cur_ptr->entries;
        WT_TIME_AGGREGATE_MERGE(session, &prev_ptr->ta, &cur_ptr->ta);
        dsk = r->cur_ptr->image.mem;
        memcpy((uint8_t *)r->prev_ptr->image.mem + prev_ptr->image.size,
          WT_PAGE_HEADER_BYTE(btree, dsk), cur_ptr->image.size - WT_PAGE_HEADER_BYTE_SIZE(btree));
        prev_ptr->image.size = combined_size;

        /*
         * At this point, there is only one disk image in the memory, the previous chunk. Update the
         * current chunk to that chunk, discard the unused chunk.
         */
        tmp = r->prev_ptr;
        r->prev_ptr = r->cur_ptr;
        r->cur_ptr = tmp;
        return (__rec_split_chunk_init(session, r, r->prev_ptr));
    }

    if (prev_ptr->min_offset != 0 && cur_ptr->image.size < r->min_split_size) {
        /* This won't work for FLCS pages, so make sure we don't get here by accident. */
        WT_ASSERT(session, r->page->type != WT_PAGE_COL_FIX);

        /*
         * The last chunk, pointed to by the current image pointer, has less than the minimum data.
         * Let's move any data more than the minimum from the previous image into the current.
         *
         * Grow the current buffer if it is not large enough.
         */
        len_to_move = prev_ptr->image.size - prev_ptr->min_offset;
        if (r->space_avail < len_to_move)
            WT_RET(__wt_rec_split_grow(session, r, len_to_move));
        cur_dsk_start = WT_PAGE_HEADER_BYTE(btree, r->cur_ptr->image.mem);

        /*
         * Shift the contents of the current buffer to make space for the data that will be
         * prepended into the current buffer. Copy the data from the previous buffer to the start of
         * the current.
         */
        memmove(cur_dsk_start + len_to_move, cur_dsk_start,
          cur_ptr->image.size - WT_PAGE_HEADER_BYTE_SIZE(btree));
        memcpy(
          cur_dsk_start, (uint8_t *)r->prev_ptr->image.mem + prev_ptr->min_offset, len_to_move);

        /* Update boundary information */
        cur_ptr->entries += prev_ptr->entries - prev_ptr->min_entries;
        cur_ptr->recno = prev_ptr->min_recno;
        WT_RET(
          __wt_buf_set(session, &cur_ptr->key, prev_ptr->min_key.data, prev_ptr->min_key.size));
        WT_TIME_AGGREGATE_MERGE(session, &cur_ptr->ta, &prev_ptr->ta);
        cur_ptr->image.size += len_to_move;

        prev_ptr->entries = prev_ptr->min_entries;
        WT_TIME_AGGREGATE_COPY(&prev_ptr->ta, &prev_ptr->ta_min);
        prev_ptr->image.size -= len_to_move;
    }

    /* Write out the previous image */
    return (__rec_split_write(session, r, r->prev_ptr, NULL, false));
}

/*
 * __wt_rec_split_finish --
 *     Finish processing a page.
 */
int
__wt_rec_split_finish(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    /*
     * We're done reconciling, write the final page. We may arrive here with no entries to write if
     * the page was entirely empty or if nothing on the page was visible to us.
     *
     * Pages with skipped or not-yet-globally visible updates aren't really empty; otherwise, the
     * page is truly empty and we will merge it into its parent during the parent's reconciliation.
     *
     * Checkpoint never writes uncommitted changes to disk and only saves the updates to move older
     * updates to the history store. Thus it can consider the reconciliation done if there are no
     * more entries left to write. This will also remove its reference entry from its parent.
     */
    if (r->entries == 0 && (r->supd_next == 0 || F_ISSET(r, WT_REC_CHECKPOINT)))
        return (0);

    /* Set the number of entries and size for the just finished chunk. */
    r->cur_ptr->entries = r->entries;
    if (r->page->type == WT_PAGE_COL_FIX && (r->cur_ptr->auxentries = r->aux_entries) != 0)
        r->cur_ptr->image.size = WT_PTRDIFF(r->aux_first_free, r->cur_ptr->image.mem);
    else
        r->cur_ptr->image.size = WT_PTRDIFF(r->first_free, r->cur_ptr->image.mem);

    /*
     *  Potentially reconsider a previous chunk.
     *
     * Skip for FLCS because (a) pages can be combined only if the combined bitmap data size is in
     * range, not the overall page size (which requires entirely different logic) and (b) this
     * cannot happen because we only split when we've fully filled the previous page. This is true
     * even when in-memory splits give us odd page sizes to work with -- some of those might be
     * mergeable (though more likely not) but we can't see them on this code path. So instead just
     * write the previous chunk out.
     */
    if (r->prev_ptr != NULL) {
        if (r->page->type != WT_PAGE_COL_FIX)
            WT_RET(__rec_split_finish_process_prev(session, r));
        else
            WT_RET(__rec_split_write(session, r, r->prev_ptr, NULL, false));
    }

    /* Write the remaining data/last page. */
    return (__rec_split_write(session, r, r->cur_ptr, NULL, true));
}

/*
 * __rec_supd_move --
 *     Move a saved WT_UPDATE list from the per-page cache to a specific block's list.
 */
static int
__rec_supd_move(WT_SESSION_IMPL *session, WT_MULTI *multi, WT_SAVE_UPD *supd, uint32_t n)
{
    uint32_t i;

    multi->supd_restore = false;

    WT_RET(__wt_calloc_def(session, n, &multi->supd));

    for (i = 0; i < n; ++i) {
        if (supd->restore)
            multi->supd_restore = true;
        multi->supd[i] = *supd++;
    }

    multi->supd_entries = n;
    return (0);
}

/*
 * __rec_split_write_supd --
 *     Check if we've saved updates that belong to this block, and move any to the per-block
 *     structure.
 */
static int
__rec_split_write_supd(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_CHUNK *chunk, WT_MULTI *multi, bool last_block)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_REC_CHUNK *next;
    WT_SAVE_UPD *supd;
    WT_UPDATE *upd;
    uint32_t i, j;
    int cmp;

    /*
     * Check if we've saved updates that belong to this block, and move any to the per-block
     * structure.
     *
     * This code requires a key be filled in for the next block (or the last block flag be set, if
     * there's no next block).
     *
     * The last block gets all remaining saved updates.
     */
    if (last_block) {
        WT_RET(__rec_supd_move(session, multi, r->supd, r->supd_next));
        r->supd_next = 0;
        r->supd_memsize = 0;
        return (ret);
    }

    /*
     * Get the saved update's key and compare it with the block's key range. If the saved update
     * list belongs with the block we're about to write, move it to the per-block memory. Check only
     * to the first update that doesn't go with the block, they must be in sorted order.
     *
     * The other chunk will have the key for the next page, that's what we compare against.
     */
    next = chunk == r->cur_ptr ? r->prev_ptr : r->cur_ptr;
    page = r->page;
    if (page->type == WT_PAGE_ROW_LEAF) {
        btree = S2BT(session);
        WT_RET(__wt_scr_alloc(session, 0, &key));

        for (i = 0, supd = r->supd; i < r->supd_next; ++i, ++supd) {
            if (supd->ins == NULL)
                WT_ERR(__wt_row_leaf_key(session, page, supd->rip, key, false));
            else {
                key->data = WT_INSERT_KEY(supd->ins);
                key->size = WT_INSERT_KEY_SIZE(supd->ins);
            }
            WT_ASSERT(session, next != NULL);
            WT_ERR(__wt_compare(session, btree->collator, key, &next->key, &cmp));
            if (cmp >= 0)
                break;
        }
    } else
        for (i = 0, supd = r->supd; i < r->supd_next; ++i, ++supd)
            if (WT_INSERT_RECNO(supd->ins) >= next->recno)
                break;
    if (i != 0) {
        WT_ERR(__rec_supd_move(session, multi, r->supd, i));

        /*
         * If there are updates that weren't moved to the block, shuffle them to the beginning of
         * the cached list (we maintain the saved updates in sorted order, new saved updates must be
         * appended to the list).
         */
        r->supd_memsize = 0;
        for (j = 0; i < r->supd_next; ++j, ++i) {
            /* Account for the remaining update memory. */
            if (r->supd[i].ins == NULL)
                /* Note: ins is never NULL for column-store */
                upd = page->modify->mod_row_update[WT_ROW_SLOT(page, r->supd[i].rip)];
            else
                upd = r->supd[i].ins->upd;
            r->supd_memsize += __wt_update_list_memsize(upd);
            r->supd[j] = r->supd[i];
        }
        r->supd_next = j;
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}

/*
 * __rec_set_page_write_gen --
 *     Initialize the page write generation number.
 */
static void
__rec_set_page_write_gen(WT_BTREE *btree, WT_PAGE_HEADER *dsk)
{
    /*
     * We increment the block's write generation so it's easy to identify newer versions of blocks
     * during salvage. (It's common in WiredTiger, at least for the default block manager, for
     * multiple blocks to be internally consistent with identical first and last keys, so we need a
     * way to know the most recent state of the block. We could check which leaf is referenced by a
     * valid internal page, but that implies salvaging internal pages, which I don't want to do, and
     * it's not as good anyway, because the internal page may not have been written after the leaf
     * page was updated. So, write generations it is.
     *
     * Nothing is locked at this point but two versions of a page with the same generation is pretty
     * unlikely, and if we did, they're going to be roughly identical for the purposes of salvage,
     * anyway.
     *
     * Other than salvage, the write generation number is used to reset the stale transaction id's
     * present on the page upon server restart.
     */
    dsk->write_gen = ++btree->write_gen;
}

/*
 * __rec_split_write_header --
 *     Initialize a disk page's header.
 */
static void
__rec_split_write_header(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_CHUNK *chunk,
  WT_MULTI *multi, WT_PAGE_HEADER *dsk)
{
    WT_BTREE *btree;
    WT_PAGE *page;

    btree = S2BT(session);
    page = r->page;

    dsk->recno = btree->type == BTREE_ROW ? WT_RECNO_OOB : multi->key.recno;

    __rec_set_page_write_gen(btree, dsk);
    dsk->mem_size = multi->size;
    dsk->u.entries = chunk->entries;
    dsk->type = page->type;

    dsk->flags = 0;

    /* Set the zero-length value flag in the page header. */
    if (page->type == WT_PAGE_ROW_LEAF) {
        F_CLR(dsk, WT_PAGE_EMPTY_V_ALL | WT_PAGE_EMPTY_V_NONE);

        if (chunk->entries != 0 && r->all_empty_value)
            F_SET(dsk, WT_PAGE_EMPTY_V_ALL);
        if (chunk->entries != 0 && !r->any_empty_value)
            F_SET(dsk, WT_PAGE_EMPTY_V_NONE);
    }

    dsk->unused = 0;
    dsk->version = WT_PAGE_VERSION_TS;

    /* Clear the memory owned by the block manager. */
    memset(WT_BLOCK_HEADER_REF(dsk), 0, btree->block_header);
}

/*
 * __rec_split_write_reuse --
 *     Check if a previously written block can be reused.
 */
static bool
__rec_split_write_reuse(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_MULTI *multi, WT_ITEM *image, bool last_block)
{
    WT_MULTI *multi_match;
    WT_PAGE_MODIFY *mod;

    mod = r->page->modify;

    /*
     * Don't bother calculating checksums for bulk loads, there's no reason to believe they'll be
     * useful. Check because LSM does bulk-loads as part of normal operations and the check is
     * cheap.
     */
    if (r->is_bulk_load)
        return (false);

    /*
     * Calculating the checksum is the expensive part, try to avoid it.
     *
     * Ignore the last block of any reconciliation. Pages are written in the same block order every
     * time, so the last block written for a page is unlikely to match any previously written block
     * or block written in the future, (absent a point-update earlier in the page which didn't
     * change the size of the on-page object in any way).
     */
    if (last_block)
        return (false);

    /*
     * Quit if evicting with no previously written block to compare against. (In other words, if
     * there's eviction pressure and the page was never written by a checkpoint, calculating a
     * checksum is worthless.)
     *
     * Quit if evicting and a previous check failed, once there's a miss no future block will match.
     */
    if (F_ISSET(r, WT_REC_EVICT)) {
        if (mod->rec_result != WT_PM_REC_MULTIBLOCK || mod->mod_multi_entries < r->multi_next)
            return (false);
        if (r->evict_matching_checksum_failed)
            return (false);
    }

    /* Calculate the checksum for this block. */
    multi->checksum = __wt_checksum(image->data, image->size);

    /*
     * Don't check for a block match when writing a page for compaction, the whole idea is to move
     * those blocks. Check after calculating the checksum, there's a possibility the calculated
     * checksum will be useful in the future.
     */
    if (F_ISSET_ATOMIC_16(r->page, WT_PAGE_COMPACTION_WRITE))
        return (false);

    /*
     * Pages are written in the same block order every time, only check the appropriate slot.
     */
    if (mod->rec_result != WT_PM_REC_MULTIBLOCK || mod->mod_multi_entries < r->multi_next)
        return (false);

    multi_match = &mod->mod_multi[r->multi_next - 1];
    if (multi_match->size != multi->size || multi_match->checksum != multi->checksum) {
        r->evict_matching_checksum_failed = true;
        return (false);
    }

    multi_match->addr.reuse = 1;
    multi->addr = multi_match->addr;

    WT_STAT_DATA_INCR(session, rec_page_match);
    return (true);
}

/*
 * __rec_compression_adjust --
 *     Adjust the pre-compression page size based on compression results.
 */
static inline void
__rec_compression_adjust(WT_SESSION_IMPL *session, uint32_t max, size_t compressed_size,
  bool last_block, uint64_t *adjustp)
{
    WT_BTREE *btree;
    uint64_t adjust, current, new;
    u_int ten_percent;

    btree = S2BT(session);
    ten_percent = max / 10;

    /*
     * Changing the pre-compression size updates a shared memory location
     * and it's not uncommon to be pushing out large numbers of pages from
     * the same file. If compression creates a page larger than the target
     * size, decrease the pre-compression size. If compression creates a
     * page smaller than the target size, increase the pre-compression size.
     * Once we get under the target size, try and stay there to minimize
     * shared memory updates, but don't go over the target size, that means
     * we're writing bad page sizes.
     *	Writing a shared memory location without a lock and letting it
     * race, minor trickiness so we only read and write the value once.
     */
    WT_ORDERED_READ(current, *adjustp);
    WT_ASSERT(session, current >= max);

    if (compressed_size > max) {
        /*
         * The compressed size is GT the page maximum. Check if the pre-compression size is larger
         * than the maximum. If 10% of the page size larger than the maximum, decrease it by that
         * amount. Else if it's not already at the page maximum, set it there.
         *
         * Note we're using 10% of the maximum page size as our test for when to adjust the
         * pre-compression size as well as the amount by which we adjust it. Not updating the value
         * when it's close to the page size keeps us from constantly updating a shared memory
         * location, and 10% of the page size is an OK step value as well, so we use it in both
         * cases.
         */
        adjust = current - max;
        if (adjust > ten_percent)
            new = current - ten_percent;
        else if (adjust != 0)
            new = max;
        else
            return;
    } else {
        /*
         * The compressed size is LTE the page maximum.
         *
         * Don't increase the pre-compressed size on the last block, the last block might be tiny.
         *
         * If the compressed size is less than the page maximum by 10%, increase the pre-compression
         * size by 10% of the page, or up to the maximum in-memory image size.
         *
         * Note we're using 10% of the maximum page size... see above.
         */
        if (last_block || compressed_size > max - ten_percent)
            return;

        adjust = current + ten_percent;
        if (adjust < btree->maxmempage_image)
            new = adjust;
        else if (current != btree->maxmempage_image)
            new = btree->maxmempage_image;
        else
            return;
    }
    *adjustp = new;
}

/*
 * __rec_split_write --
 *     Write a disk block out for the split helper functions.
 */
static int
__rec_split_write(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_CHUNK *chunk,
  WT_ITEM *compressed_image, bool last_block)
{
    WT_BTREE *btree;
    WT_MULTI *multi;
    WT_PAGE *page;
    size_t addr_size, compressed_size;
    uint8_t addr[WT_BTREE_MAX_ADDR_COOKIE];
#ifdef HAVE_DIAGNOSTIC
    bool verify_image;
#endif

    btree = S2BT(session);
    page = r->page;
#ifdef HAVE_DIAGNOSTIC
    verify_image = true;
#endif

    /*
     * If reconciliation requires multiple blocks and checkpoint is running we'll eventually fail,
     * unless we're the checkpoint thread. Big pages take a lot of writes, avoid wasting work.
     */
    if (!last_block && WT_BTREE_SYNCING(btree) && !WT_SESSION_BTREE_SYNC(session))
        return (__wt_set_return(session, EBUSY));

    /* Make sure there's enough room for another write. */
    WT_RET(__wt_realloc_def(session, &r->multi_allocated, r->multi_next + 1, &r->multi));
    multi = &r->multi[r->multi_next++];

    /* Initialize the address (set the addr type for the parent). */
    WT_TIME_AGGREGATE_COPY(&multi->addr.ta, &chunk->ta);

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        multi->addr.type = WT_ADDR_LEAF_NO;
        break;
    case WT_PAGE_COL_VAR:
    case WT_PAGE_ROW_LEAF:
        multi->addr.type = r->ovfl_items ? WT_ADDR_LEAF : WT_ADDR_LEAF_NO;
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        multi->addr.type = WT_ADDR_INT;
        break;
    default:
        return (__wt_illegal_value(session, page->type));
    }
    multi->size = WT_STORE_SIZE(chunk->image.size);
    multi->checksum = 0;
    multi->supd_restore = false;

    /* Set the key. */
    if (btree->type == BTREE_ROW)
        WT_RET(__wt_row_ikey_alloc(session, 0, chunk->key.data, chunk->key.size, &multi->key.ikey));
    else
        multi->key.recno = chunk->recno;

    /* Check if there are saved updates that might belong to this block. */
    if (r->supd_next != 0)
        WT_RET(__rec_split_write_supd(session, r, chunk, multi, last_block));

    /* Initialize the page header(s). */
    __rec_split_write_header(session, r, chunk, multi, chunk->image.mem);
    if (r->page->type == WT_PAGE_COL_FIX)
        __wt_rec_col_fix_write_auxheader(
          session, r, chunk->entries, chunk->auxentries, chunk->image.mem, chunk->image.size);
    if (compressed_image != NULL)
        __rec_split_write_header(session, r, chunk, multi, compressed_image->mem);

    /*
     * If we are writing the whole page in our first/only attempt, it might be a checkpoint
     * (checkpoints are only a single page, by definition). Checkpoints aren't written here, the
     * wrapup functions do the write.
     *
     * Track the buffer with the image. (This is bad layering, but we can't write the image until
     * the wrapup code, and we don't have a code path from here to there.)
     */
    if (last_block && r->multi_next == 1 && __rec_is_checkpoint(session, r)) {
        WT_ASSERT(session, r->supd_next == 0);

        if (compressed_image == NULL)
            r->wrapup_checkpoint = &chunk->image;
        else {
            r->wrapup_checkpoint = compressed_image;
            r->wrapup_checkpoint_compressed = true;
        }
        return (0);
    }

    /*
     * If configured for an in-memory database, we can't actually write it. Instead, we will
     * re-instantiate the page using the disk image and any list of updates we skipped.
     */
    if (F_ISSET(r, WT_REC_IN_MEMORY))
        goto copy_image;

    /* Check the eviction flag as checkpoint also saves updates. */
    if (F_ISSET(r, WT_REC_EVICT) && multi->supd != NULL) {
        /*
         * XXX If no entries were used, the page is empty and we can only restore eviction/restore
         * or history store updates against empty row-store leaf pages, column-store modify attempts
         * to allocate a zero-length array.
         */
        if (r->page->type != WT_PAGE_ROW_LEAF && chunk->entries == 0)
            return (__wt_set_return(session, EBUSY));

        /* If we need to restore the page to memory, copy the disk image. */
        if (multi->supd_restore)
            goto copy_image;

        WT_ASSERT(session, chunk->entries > 0);
    }

    /*
     * If we wrote this block before, re-use it. Prefer a checksum of the compressed image. It's an
     * identical test and should be faster.
     */
    if (__rec_split_write_reuse(session, r, multi,
          compressed_image == NULL ? &chunk->image : compressed_image, last_block))
        goto copy_image;

    /* Write the disk image and get an address. */
    WT_RET(__wt_bt_write(session, compressed_image == NULL ? &chunk->image : compressed_image, addr,
      &addr_size, &compressed_size, false, F_ISSET(r, WT_REC_CHECKPOINT),
      compressed_image != NULL));
#ifdef HAVE_DIAGNOSTIC
    verify_image = false;
#endif
    WT_RET(__wt_memdup(session, addr, addr_size, &multi->addr.addr));
    multi->addr.size = (uint8_t)addr_size;

    /* Adjust the pre-compression page size based on compression results. */
    if (WT_PAGE_IS_INTERNAL(page) && compressed_size != 0 && btree->intlpage_compadjust)
        __rec_compression_adjust(
          session, btree->maxintlpage, compressed_size, last_block, &btree->maxintlpage_precomp);
    if (!WT_PAGE_IS_INTERNAL(page) && compressed_size != 0 && btree->leafpage_compadjust)
        __rec_compression_adjust(
          session, btree->maxleafpage, compressed_size, last_block, &btree->maxleafpage_precomp);

    /* Update the per-page reconciliation time statistics now that we've written something. */
    __rec_page_time_stats(session, r);

copy_image:
#ifdef HAVE_DIAGNOSTIC
    /*
     * The I/O routines verify all disk images we write, but there are paths in reconciliation that
     * don't do I/O. Verify those images, too.
     */
    WT_ASSERT(session,
      verify_image == false ||
        __wt_verify_dsk_image(
          session, "[reconcile-image]", chunk->image.data, 0, &multi->addr, true) == 0);
#endif
    /*
     * If re-instantiating this page in memory (either because eviction wants to, or because we
     * skipped updates to build the disk image), save a copy of the disk image.
     */
    if (F_ISSET(r, WT_REC_SCRUB) || multi->supd_restore)
        WT_RET(__wt_memdup(session, chunk->image.data, chunk->image.size, &multi->disk_image));

    /* Whether we wrote or not, clear the accumulated time statistics. */
    __rec_page_time_stats_clear(r);

    return (0);
}

/*
 * __wt_bulk_init --
 *     Bulk insert initialization.
 */
int
__wt_bulk_init(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
    WT_BTREE *btree;
    WT_PAGE_INDEX *pindex;
    WT_RECONCILE *r;
    uint64_t recno;

    btree = S2BT(session);

    /*
     * Bulk-load is only permitted on newly created files, not any empty file -- see the checkpoint
     * code for a discussion.
     */
    if (!btree->original)
        WT_RET_MSG(session, EINVAL, "bulk-load is only possible for newly created trees");

    /*
     * Get a reference to the empty leaf page; we have exclusive access so we can take a copy of the
     * page, confident the parent won't split.
     */
    pindex = WT_INTL_INDEX_GET_SAFE(btree->root.page);
    cbulk->ref = pindex->index[0];
    cbulk->leaf = cbulk->ref->page;

    WT_RET(__rec_init(session, cbulk->ref, 0, NULL, &cbulk->reconcile));
    r = cbulk->reconcile;
    r->is_bulk_load = true;

    recno = btree->type == BTREE_ROW ? WT_RECNO_OOB : 1;

    return (__wt_rec_split_init(session, r, cbulk->leaf, recno, btree->maxleafpage_precomp, 0));
}

/*
 * __wt_bulk_wrapup --
 *     Bulk insert cleanup.
 */
int
__wt_bulk_wrapup(WT_SESSION_IMPL *session, WT_CURSOR_BULK *cbulk)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_PAGE *parent;
    WT_RECONCILE *r;

    btree = S2BT(session);
    if ((r = cbulk->reconcile) == NULL)
        return (0);

    switch (btree->type) {
    case BTREE_COL_FIX:
        if (cbulk->entry != 0)
            __wt_rec_incr(
              session, r, cbulk->entry, __bitstr_size((size_t)cbulk->entry * btree->bitcnt));
        break;
    case BTREE_COL_VAR:
        if (cbulk->rle != 0)
            WT_ERR(__wt_bulk_insert_var(session, cbulk, false));
        break;
    case BTREE_ROW:
        break;
    }

    WT_ERR(__wt_rec_split_finish(session, r));
    WT_ERR(__rec_write_wrapup(session, r, r->page));
    __rec_write_page_status(session, r);

    /* Mark the page's parent and the tree dirty. */
    parent = r->ref->home;
    WT_ERR(__wt_page_modify_init(session, parent));
    __wt_page_modify_set(session, parent);

err:
    __rec_cleanup(session, r);
    __rec_destroy(session, &cbulk->reconcile);

    return (ret);
}

/*
 * __rec_split_discard --
 *     Discard the pages resulting from a previous split.
 */
static int
__rec_split_discard(WT_SESSION_IMPL *session, WT_PAGE *page)
{
    WT_BTREE *btree;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t i;

    btree = S2BT(session);
    mod = page->modify;

    /*
     * A page that split is being reconciled for the second, or subsequent time; discard underlying
     * block space used in the last reconciliation that is not being reused for this reconciliation.
     */
    for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
        if (btree->type == BTREE_ROW)
            __wt_free(session, multi->key);

        __wt_free(session, multi->disk_image);
        __wt_free(session, multi->supd);

        /*
         * If the page was re-written free the backing disk blocks used in the previous write
         * (unless the blocks were reused in this write). The page may instead have been a disk
         * image with associated saved updates: ownership of the disk image is transferred when
         * rewriting the page in-memory and there may not have been saved updates. We've gotten this
         * wrong a few times, so use the existence of an address to confirm backing blocks we care
         * about, and free any disk image/saved updates.
         */
        if (multi->addr.addr != NULL && !multi->addr.reuse) {
            WT_RET(__wt_btree_block_free(session, multi->addr.addr, multi->addr.size));
            __wt_free(session, multi->addr.addr);
        }
    }
    __wt_free(session, mod->mod_multi);
    mod->mod_multi_entries = 0;

    /*
     * This routine would be trivial, and only walk a single page freeing any blocks written to
     * support the split, except for root splits. In the case of root splits, we have to cope with
     * multiple pages in a linked list, and we also have to discard overflow items written for the
     * page.
     */
    if (WT_PAGE_IS_INTERNAL(page) && mod->mod_root_split != NULL) {
        WT_RET(__rec_split_discard(session, mod->mod_root_split));
        WT_RET(__wt_ovfl_track_wrapup(session, mod->mod_root_split));
        __wt_page_out(session, &mod->mod_root_split);
    }

    return (0);
}

/*
 * __rec_split_dump_keys --
 *     Dump out the split keys in verbose mode.
 */
static int
__rec_split_dump_keys(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(tkey);
    WT_MULTI *multi;
    uint32_t i;

    btree = S2BT(session);

    __wt_verbose(session, WT_VERB_SPLIT, "split: %" PRIu32 " pages", r->multi_next);

    if (btree->type == BTREE_ROW) {
        WT_RET(__wt_scr_alloc(session, 0, &tkey));
        for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
            __wt_verbose(session, WT_VERB_SPLIT, "starting key %s",
              __wt_buf_set_printable(
                session, WT_IKEY_DATA(multi->key.ikey), multi->key.ikey->size, false, tkey));
        __wt_scr_free(session, &tkey);
    } else
        for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
            __wt_verbose(session, WT_VERB_SPLIT, "starting recno %" PRIu64, multi->key.recno);
    return (0);
}

/*
 * __rec_write_wrapup --
 *     Finish the reconciliation.
 */
static int
__rec_write_wrapup(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_REF *ref;
    WT_TIME_AGGREGATE ta;
    uint32_t i;

    btree = S2BT(session);
    bm = btree->bm;
    mod = page->modify;
    ref = r->ref;
    WT_TIME_AGGREGATE_INIT(&ta);

    /*
     * If using the history store table eviction path and we found updates that weren't globally
     * visible when reconciling this page, copy them into the database's history store. This can
     * fail, so try before clearing the page's previous reconciliation state.
     */
    if (F_ISSET(r, WT_REC_HS))
        WT_RET(__rec_hs_wrapup(session, r));

    /*
     * Wrap up overflow tracking. If we are about to create a checkpoint, the system must be
     * entirely consistent at that point (the underlying block manager is presumably going to do
     * some action to resolve the list of allocated/free/whatever blocks that are associated with
     * the checkpoint).
     */
    WT_RET(__wt_ovfl_track_wrapup(session, page));

    /*
     * This page may have previously been reconciled, and that information is now about to be
     * replaced. Make sure it's discarded at some point, and clear the underlying modification
     * information, we're creating a new reality.
     */
    switch (mod->rec_result) {
    case 0: /*
             * The page has never been reconciled before, free the original
             * address blocks (if any).  The "if any" is for empty trees
             * created when a new tree is opened or previously deleted pages
             * instantiated in memory.
             *
             * The exception is root pages are never tracked or free'd, they
             * are checkpoints, and must be explicitly dropped.
             */
        if (__wt_ref_is_root(ref))
            break;

        WT_RET(__wt_ref_block_free(session, ref));
        break;
    case WT_PM_REC_EMPTY: /* Page deleted */
        break;
    case WT_PM_REC_MULTIBLOCK: /* Multiple blocks */
                               /*
                                * Discard the multiple replacement blocks.
                                */
        WT_RET(__rec_split_discard(session, page));
        break;
    case WT_PM_REC_REPLACE: /* 1-for-1 page swap */
                            /*
                             * Discard the replacement leaf page's blocks.
                             *
                             * The exception is root pages are never tracked or free'd, they are
                             * checkpoints, and must be explicitly dropped.
                             */
        if (!__wt_ref_is_root(ref))
            WT_RET(__wt_btree_block_free(session, mod->mod_replace.addr, mod->mod_replace.size));

        /* Discard the replacement page's address and disk image. */
        __wt_free(session, mod->mod_replace.addr);
        mod->mod_replace.size = 0;
        __wt_free(session, mod->mod_disk_image);
        break;
    default:
        return (__wt_illegal_value(session, mod->rec_result));
    }

    /* Reset the reconciliation state. */
    mod->rec_result = 0;

    __wt_verbose(session, WT_VERB_RECONCILE, "%p reconciled into %" PRIu32 " pages", (void *)ref,
      r->multi_next);

    switch (r->multi_next) {
    case 0: /* Page delete */
        WT_STAT_CONN_DATA_INCR(session, rec_page_delete);

        /*
         * If this is the root page, we need to create a sync point. For a page to be empty, it has
         * to contain nothing at all, which means it has no records of any kind and is durable.
         */
        ref = r->ref;
        if (__wt_ref_is_root(ref)) {
            __wt_checkpoint_tree_reconcile_update(session, &ta);
            WT_RET(bm->checkpoint(bm, session, NULL, btree->ckpt, false));
        }

        /*
         * If the page was empty, we want to discard it from the tree by discarding the parent's key
         * when evicting the parent. Mark the page as deleted, then return success, leaving the page
         * in memory. If the page is subsequently modified, that is OK, we'll just reconcile it
         * again.
         */
        mod->rec_result = WT_PM_REC_EMPTY;
        break;
    case 1: /* 1-for-1 page swap */
        /*
         * Because WiredTiger's pages grow without splitting, we're replacing a single page with
         * another single page most of the time.
         *
         * If in-memory, or saving/restoring changes for this page and there's only one block,
         * there's nothing to write. Set up a single block as if to split, then use that disk image
         * to rewrite the page in memory. This is separate from simple replacements where eviction
         * has decided to retain the page in memory because the latter can't handle update lists and
         * splits can.
         */
        if (F_ISSET(r, WT_REC_IN_MEMORY) || r->multi->supd_restore) {
            WT_ASSERT(session,
              F_ISSET(r, WT_REC_IN_MEMORY) ||
                (F_ISSET(r, WT_REC_EVICT) && r->leave_dirty && r->multi->supd_entries != 0));
            goto split;
        }

        /*
         * We may have a root page, create a sync point. (The write code ignores root page updates,
         * leaving that work to us.)
         */
        if (r->wrapup_checkpoint == NULL) {
            mod->mod_replace = r->multi->addr;
            r->multi->addr.addr = NULL;
            mod->mod_disk_image = r->multi->disk_image;
            r->multi->disk_image = NULL;
        } else {
            __wt_checkpoint_tree_reconcile_update(session, &r->multi->addr.ta);
            WT_RET(__wt_bt_write(session, r->wrapup_checkpoint, NULL, NULL, NULL, true,
              F_ISSET(r, WT_REC_CHECKPOINT), r->wrapup_checkpoint_compressed));
        }

        mod->rec_result = WT_PM_REC_REPLACE;
        break;
    default: /* Page split */
        if (WT_PAGE_IS_INTERNAL(page))
            WT_STAT_DATA_INCR(session, rec_multiblock_internal);
        else
            WT_STAT_DATA_INCR(session, rec_multiblock_leaf);

        /* Optionally display the actual split keys in verbose mode. */
        if (WT_VERBOSE_ISSET(session, WT_VERB_SPLIT))
            WT_RET(__rec_split_dump_keys(session, r));

        /*
         * The reuse flag was set in some cases, but we have to clear it, otherwise on subsequent
         * reconciliation we would fail to remove blocks that are being discarded.
         */
split:
        for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
            multi->addr.reuse = 0;

        mod->mod_multi = r->multi;
        mod->mod_multi_entries = r->multi_next;
        mod->rec_result = WT_PM_REC_MULTIBLOCK;

        r->multi = NULL;
        r->multi_next = 0;
        break;
    }

    return (0);
}

/*
 * __rec_write_err --
 *     Finish the reconciliation on error.
 */
static int
__rec_write_err(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_PAGE *page)
{
    WT_DECL_RET;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    uint32_t i;

    mod = page->modify;

    /*
     * Clear the address-reused flag from the multiblock reconciliation information (otherwise we
     * might think the backing block is being reused on a subsequent reconciliation where we want to
     * free it).
     */
    if (mod->rec_result == WT_PM_REC_MULTIBLOCK)
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i)
            multi->addr.reuse = 0;

    /*
     * On error, discard blocks we've written, they're unreferenced by the tree. This is not a
     * question of correctness, we're avoiding block leaks.
     *
     * Don't discard backing blocks marked for reuse, they remain part of a previous reconciliation.
     */
    for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
        if (multi->addr.addr != NULL) {
            if (multi->addr.reuse)
                multi->addr.addr = NULL;
            else
                WT_TRET(__wt_btree_block_free(session, multi->addr.addr, multi->addr.size));
        }

    WT_TRET(__wt_ovfl_track_wrapup_err(session, page));

    return (ret);
}

/*
 * __rec_hs_wrapup --
 *     Copy all of the saved updates into the database's history store table.
 */
static int
__rec_hs_wrapup(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_MULTI *multi;
    uint32_t i;

    btree = S2BT(session);

    /*
     * Sanity check: Can't insert updates into history store from the history store itself or from
     * the metadata file.
     */
    WT_ASSERT(session, !WT_IS_HS(btree->dhandle) && !WT_IS_METADATA(btree->dhandle));
    /* Flag as unused for non diagnostic builds. */
    WT_UNUSED(btree);

    /* Check if there's work to do. */
    for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
        if (multi->supd != NULL)
            break;
    if (i == r->multi_next)
        return (0);

    for (multi = r->multi, i = 0; i < r->multi_next; ++multi, ++i)
        if (multi->supd != NULL) {
            WT_ERR(__wt_hs_insert_updates(session, r, multi));
            if (!multi->supd_restore) {
                __wt_free(session, multi->supd);
                multi->supd_entries = 0;
            }
        }

err:
    return (ret);
}

/*
 * __wt_rec_cell_build_ovfl --
 *     Store overflow items in the file, returning the address cookie.
 */
int
__wt_rec_cell_build_ovfl(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_REC_KV *kv, uint8_t type,
  WT_TIME_WINDOW *tw, uint64_t rle)
{
    WT_BM *bm;
    WT_BTREE *btree;
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_HEADER *dsk;
    size_t size;
    uint8_t *addr, buf[WT_BTREE_MAX_ADDR_COOKIE];

    btree = S2BT(session);
    bm = btree->bm;
    page = r->page;

    /* Track if page has overflow items. */
    r->ovfl_items = true;

    /*
     * See if this overflow record has already been written and reuse it if possible, otherwise
     * write a new overflow record.
     */
    WT_RET(__wt_ovfl_reuse_search(session, page, &addr, &size, kv->buf.data, kv->buf.size));
    if (addr == NULL) {
        /* Allocate a buffer big enough to write the overflow record. */
        size = kv->buf.size;
        WT_RET(bm->write_size(bm, session, &size));
        WT_RET(__wt_scr_alloc(session, size, &tmp));

        /* Initialize the buffer: disk header and overflow record. */
        dsk = tmp->mem;
        memset(dsk, 0, WT_PAGE_HEADER_SIZE);
        dsk->type = WT_PAGE_OVFL;
        __rec_set_page_write_gen(btree, dsk);
        dsk->u.datalen = (uint32_t)kv->buf.size;
        memcpy(WT_PAGE_HEADER_BYTE(btree, dsk), kv->buf.data, kv->buf.size);
        dsk->mem_size = WT_PAGE_HEADER_BYTE_SIZE(btree) + (uint32_t)kv->buf.size;
        tmp->size = dsk->mem_size;

        /* Write the buffer. */
        addr = buf;
        WT_ERR(__wt_bt_write(
          session, tmp, addr, &size, NULL, false, F_ISSET(r, WT_REC_CHECKPOINT), false));

        /*
         * Track the overflow record (unless it's a bulk load, which by definition won't ever reuse
         * a record.
         */
        if (!r->is_bulk_load)
            WT_ERR(__wt_ovfl_reuse_add(session, page, addr, size, kv->buf.data, kv->buf.size));
    }

    /* Set the callers K/V to reference the overflow record's address. */
    WT_ERR(__wt_buf_set(session, &kv->buf, addr, size));

    /* Build the cell and return. */
    kv->cell_len = __wt_cell_pack_ovfl(session, &kv->cell, type, tw, rle, kv->buf.size);
    kv->len = kv->cell_len + kv->buf.size;

err:
    __wt_scr_free(session, &tmp);
    return (ret);
}
