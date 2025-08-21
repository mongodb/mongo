/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#define WT_CHECKPOINT_CLEANUP_DEFAULT_WAKE_UP_INTERVAL 5 /* 5 seconds */
#define WT_URI_FILE_PREFIX "file:"

/*
 * __sync_obsolete_limit_reached --
 *     This function checks whether checkpoint cleanup can continue operating on the obsolete time
 *     window pages.
 */
static bool
__sync_obsolete_limit_reached(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);
    btree = S2BT(session);

    /* Check current progress against max. */
    if (__wt_atomic_load32(&btree->checkpoint_cleanup_obsolete_tw_pages) >=
      conn->heuristic_controls.checkpoint_cleanup_obsolete_tw_pages_dirty_max)
        return (true);

    /*
     * If the current btree has not contributed to the cleanup yet, only process it if we can track
     * another btree.
     */
    if (__wt_atomic_load32(&btree->eviction_obsolete_tw_pages) == 0 &&
      __wt_atomic_load32(&btree->checkpoint_cleanup_obsolete_tw_pages) == 0 &&
      __wt_atomic_load32(&conn->heuristic_controls.obsolete_tw_btree_count) >=
        conn->heuristic_controls.obsolete_tw_btree_max)
        return (true);

    return (false);
}

/*
 * __sync_obsolete_tw_check --
 *     This function checks whether the given time aggregate refers to a globally visible time
 *     window and if checkpoint cleanup can process it. Note that time aggregates corresponding to
 *     fully deleted pages are not considered.
 */
static bool
__sync_obsolete_tw_check(WT_SESSION_IMPL *session, WT_TIME_AGGREGATE ta)
{
    /* Don't process fully deleted pages. */
    if (WT_TIME_AGGREGATE_HAS_STOP(&ta))
        return (false);

    /* Limit the activity to reduce the load. */
    if (__sync_obsolete_limit_reached(session))
        return (false);

    /* Ensure there is globally visible content. */
    if (!__wt_txn_has_newest_and_visible_all(
          session, ta.newest_txn, WT_MAX(ta.newest_start_durable_ts, ta.newest_stop_durable_ts)))
        return (false);

    return (true);
}

/*
 * __sync_obsolete_inmem_evict_or_mark_dirty --
 *     This function checks whether the in-memory ref contains obsolete information and takes
 *     necessary action.
 */
static int
__sync_obsolete_inmem_evict_or_mark_dirty(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_ADDR_COPY addr;
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_MULTI *multi;
    WT_PAGE_MODIFY *mod;
    WT_TIME_AGGREGATE newest_ta;
    uint32_t i;
    char time_string[WT_TIME_STRING_SIZE];
    const char *tag = "unexpected page state";
    bool do_visibility_check, has_stop, obsolete, ovfl_items;

    btree = S2BT(session);
    conn = S2C(session);

    /*
     * Skip the modified pages as their reconciliation results are not valid any more. Check for the
     * page modification only after acquiring the hazard pointer to protect against the page being
     * freed in parallel.
     */
    WT_ASSERT(session, ref->page != NULL);
    if (__wt_page_is_modified(ref->page))
        return (0);

    /*
     * Initialize the time aggregate via the merge initialization, so that stop visibility is copied
     * across correctly. That is we need the stop timestamp/transaction IDs to start as none,
     * otherwise we'd never mark anything as obsolete.
     */
    WT_TIME_AGGREGATE_INIT_MERGE(&newest_ta);
    do_visibility_check = obsolete = ovfl_items = false;

    /*
     *  The obsolete information on a page is identified as follows:
     *  1. The newest stop time indicates the entire content is deleted and if it is obsolete,
     *     the page is therefore no longer needed by anyone and can be marked for urgent eviction.
     *  2. If the ref contains obsolete time window information using the newest durable
     *     time. In this case, mark the ref dirty so reconciliation removes the obsolete data.
     */
    mod = ref->page->modify;
    if (mod != NULL && mod->rec_result == WT_PM_REC_EMPTY) {
        tag = "reconciled empty";
        obsolete = true;
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_MULTIBLOCK) {
        tag = "reconciled multi-block";

        /* Calculate the max time point by traversing all multi addresses. */
        for (multi = mod->mod_multi, i = 0; i < mod->mod_multi_entries; ++multi, ++i) {
            WT_TIME_AGGREGATE_MERGE(session, &newest_ta, &multi->addr.ta);
            if (multi->addr.type == WT_ADDR_LEAF)
                ovfl_items = true;
        }
        do_visibility_check = true;
    } else if (mod != NULL && mod->rec_result == WT_PM_REC_REPLACE) {
        tag = "reconciled replacement block";

        WT_TIME_AGGREGATE_MERGE(session, &newest_ta, &mod->mod_replace.ta);
        if (mod->mod_replace.type == WT_ADDR_LEAF)
            ovfl_items = true;
        do_visibility_check = true;
    } else if (__wt_ref_addr_copy(session, ref, &addr)) {
        tag = "WT_REF address";

        WT_TIME_AGGREGATE_MERGE(session, &newest_ta, &addr.ta);
        if (addr.type == WT_ADDR_LEAF)
            ovfl_items = true;
        do_visibility_check = true;
    }

    has_stop = WT_TIME_AGGREGATE_HAS_STOP(&newest_ta);
    if (do_visibility_check && has_stop)
        obsolete = __wt_txn_visible_all(
          session, newest_ta.newest_stop_txn, newest_ta.newest_stop_durable_ts);

    if (obsolete) {
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p in-memory page with %s obsolete has a stop time aggregate %s", (void *)ref, tag,
          __wt_time_aggregate_to_string(&newest_ta, time_string));

        /*
         * Dirty the obsolete page with overflow items to let the page reconciliation remove all the
         * overflow items.
         */
        if (ovfl_items) {
            WT_RET(__wt_page_modify_init(session, ref->page));
            __wt_page_modify_set(session, ref->page);
        }

        /* Mark the obsolete page to evict soon. */
        __wt_evict_page_soon(session, ref);
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_evict);
    } else if (__sync_obsolete_tw_check(session, newest_ta)) {

        /*
         * Dirty the page with an obsolete time window to let the page reconciliation remove all the
         * obsolete time window information.
         */
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p in-memory page %s obsolete time window: time aggregate %s", (void *)ref, tag,
          __wt_time_aggregate_to_string(&newest_ta, time_string));

        WT_RET(__wt_page_modify_init(session, ref->page));
        __wt_page_modify_set(session, ref->page);

        /*
         * Save that another tree has been processed if that's the first time it gets cleaned and
         * update the number of pages made dirty for that tree.
         */
        if (__wt_atomic_load32(&btree->eviction_obsolete_tw_pages) == 0 &&
          __wt_atomic_load32(&btree->checkpoint_cleanup_obsolete_tw_pages) == 0)
            __wt_atomic_addv32(&conn->heuristic_controls.obsolete_tw_btree_count, 1);

        __wt_atomic_addv32(&btree->checkpoint_cleanup_obsolete_tw_pages, 1);
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_obsolete_tw);
    }

    return (0);
}

/*
 * __sync_obsolete_deleted_cleanup --
 *     Check whether the deleted ref is obsolete according to the newest stop time point and mark
 *     its parent page dirty to remove it.
 */
static int
__sync_obsolete_deleted_cleanup(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_PAGE_DELETED *page_del;

    page_del = ref->page_del;
    if (page_del == NULL ||
      __wt_txn_visible_all(session, page_del->txnid, page_del->pg_del_durable_ts)) {
        WT_RET(__wt_page_parent_modify_set(session, ref, false));
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p: marking obsolete deleted page parent dirty", (void *)ref);
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_removed);
    } else
        __wt_verbose_debug2(
          session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping deleted page", (void *)ref);

    return (0);
}

/*
 * __sync_obsolete_disk_cleanup --
 *     Check whether the on-disk ref is obsolete according to the newest stop time point and mark
 *     its parent page dirty by changing the ref status as deleted.
 */
static int
__sync_obsolete_disk_cleanup(WT_SESSION_IMPL *session, WT_REF *ref, bool *ref_deleted)
{
    WT_ADDR_COPY addr;
    WT_DECL_RET;
    WT_TIME_AGGREGATE newest_ta;
    char time_string[WT_TIME_STRING_SIZE];
    bool obsolete;

    *ref_deleted = false;

    /*
     * If the page is on-disk and obsolete, mark the page as deleted and also set the parent page as
     * dirty. This is to ensure the parent is written during the checkpoint and the child page
     * discarded.
     */
    WT_TIME_AGGREGATE_INIT_MERGE(&newest_ta);
    obsolete = false;

    /*
     * There should be an address, but simply skip any page where we don't find one. Also skip the
     * pages that have overflow keys as part of fast delete flow. These overflow keys pages are
     * handled as an in-memory obsolete page flow.
     */
    if (__wt_ref_addr_copy(session, ref, &addr) && addr.type == WT_ADDR_LEAF_NO) {
        /*
         * Max stop timestamp is possible only when the prepared update is written to the data
         * store.
         */
        WT_TIME_AGGREGATE_MERGE_OBSOLETE_VISIBLE(session, &newest_ta, &addr.ta);
        obsolete = __wt_txn_visible_all(
          session, newest_ta.newest_stop_txn, newest_ta.newest_stop_durable_ts);
    }

    __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
      "%p on-disk page obsolete check: %s"
      "obsolete, stop time aggregate %s",
      (void *)ref, obsolete ? "" : "not ", __wt_time_aggregate_to_string(&newest_ta, time_string));

    if (obsolete && ((ret = __wt_page_parent_modify_set(session, ref, false)) == 0)) {
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p: marking obsolete disk page parent dirty", (void *)ref);
        *ref_deleted = true;
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_removed);
        return (0);
    }

    return (ret);
}

/*
 * __sync_obsolete_cleanup_one --
 *     Check whether the ref is obsolete according to the newest stop time point and handle the
 *     obsolete page by either removing it or marking it for urgent eviction. This code is a best
 *     effort - it isn't necessary that all obsolete references are noticed and resolved
 *     immediately. To that end some of the state checking takes the easy option if changes happen
 *     between operations.
 */
static int
__sync_obsolete_cleanup_one(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_DECL_RET;
    WT_REF_STATE new_state, previous_state, ref_state;
    bool ref_deleted;

    ref_deleted = false;

    /* Ignore root pages as they can never be deleted. */
    if (__wt_ref_is_root(ref)) {
        __wt_verbose_debug2(
          session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping root page", (void *)ref);
        return (0);
    }

    /* Ignore internal pages, these are taken care of during reconciliation. */
    if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
          "%p: skipping internal page with parent: %p", (void *)ref, (void *)ref->home);
        return (0);
    }

    /*
     * Check in memory, deleted and on-disk pages for obsolescence. An initial state check is done
     * without holding the ref locked - this is to avoid switching refs to locked if it's not
     * worthwhile doing the check.
     */
    ref_state = WT_REF_GET_STATE(ref);
    if (ref_state == WT_REF_DELETED || ref_state == WT_REF_DISK) {
        WT_REF_LOCK(session, ref, &previous_state);
        /*
         * There are two possible outcomes from the subsequent checks:
         * * The ref will be returned to it's previous state.
         * * The ref will change from disk to deleted.
         * Use a parameter to allow the functions to request a state change.
         */
        new_state = previous_state;
        if (previous_state == WT_REF_DELETED)
            ret = __sync_obsolete_deleted_cleanup(session, ref);
        else if (previous_state == WT_REF_DISK) {
            ret = __sync_obsolete_disk_cleanup(session, ref, &ref_deleted);
            if (ref_deleted)
                new_state = WT_REF_DELETED;
        }
        /*
         * For deleted and on-disk pages, increment ref_changes if there has been a change in the
         * ref's state. There's nothing to do for in-memory pages as we don't change those.
         */
        if (WT_DELTA_INT_ENABLED(S2BT(session), S2C(session)) && previous_state != new_state)
            __wt_atomic_addv8(&ref->ref_changes, 1);
        WT_REF_UNLOCK(ref, new_state);
        WT_RET(ret);
    } else
        /*
         * There is nothing to do for pages that aren't in one of the states we already checked, for
         * example they might have split or changed to deleted between checking the ref state. Log a
         * diagnostic message for skipped pages and move along.
         */
        __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP, "%p: skipping page", (void *)ref);

    return (ret);
}

/*
 * __checkpoint_cleanup_obsolete_cleanup --
 *     Traverse an internal page and identify the leaf pages that are obsolete and mark them as
 *     deleted.
 */
static int
__checkpoint_cleanup_obsolete_cleanup(WT_SESSION_IMPL *session, WT_REF *parent)
{
    WT_PAGE_INDEX *pindex;
    WT_REF *ref;
    uint32_t slot;

    WT_ASSERT_ALWAYS(session, WT_PAGE_IS_INTERNAL(parent->page),
      "Checkpoint obsolete cleanup requires an internal page");

    __wt_verbose_debug2(session, WT_VERB_CHECKPOINT_CLEANUP,
      "%p: traversing the internal page %p for obsolete child pages", (void *)parent,
      (void *)parent->page);

    WT_INTL_INDEX_GET(session, parent->page, pindex);
    for (slot = 0; slot < pindex->entries; slot++) {
        ref = pindex->index[slot];

        WT_RET(__sync_obsolete_cleanup_one(session, ref));
    }

    WT_STAT_CONN_DSRC_INCRV(session, checkpoint_cleanup_pages_visited, pindex->entries);

    return (0);
}

/*
 * __checkpoint_cleanup_run_chk --
 *     Check to decide if the checkpoint cleanup should continue running.
 */
static bool
__checkpoint_cleanup_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CHECKPOINT_CLEANUP));
}

/*
 * __checkpoint_cleanup_page_skip --
 *     Return if checkpoint cleanup should read this page.
 */
static int
__checkpoint_cleanup_page_skip(
  WT_SESSION_IMPL *session, WT_REF *ref, void *context, bool visible_all, bool *skipp)
{
    WT_ADDR_COPY addr;

    WT_UNUSED(context);
    WT_UNUSED(visible_all);

    *skipp = false; /* Default to reading */

    /*
     * Skip deleted pages as they are no longer required for the checkpoint. The checkpoint never
     * needs to review the content of those pages - if they should be included in the checkpoint the
     * existing page on disk contains the right information and will be linked into the checkpoint
     * as the internal tree structure is built.
     */
    if (WT_REF_GET_STATE(ref) == WT_REF_DELETED) {
        *skipp = true;
        return (0);
    }

    /* If the page is in-memory, we want to look at it. */
    if (WT_REF_GET_STATE(ref) != WT_REF_DISK)
        return (0);

    /*
     * Reading any page that is not in the cache will increase the cache size. Perform a set of
     * checks to verify the cache can handle it. Checkpoint cleanup reads clean pages into the cache
     * and also it can dirty the already existing in-memory page in the cache, skip if eviction is
     * needed.
     */
    if (__wt_evict_needed(session, false, false, NULL) || __wt_evict_aggressive(session) ||
      __wt_cache_full(session) || __wt_evict_cache_stuck(session)) {
        *skipp = true;
        return (0);
    }

    /*
     * Ignore the pages with no on-disk address. It is possible that a page with deleted state may
     * not have an on-disk address.
     */
    if (!__wt_ref_addr_copy(session, ref, &addr)) {
        *skipp = true;
        return (0);
    }

    /*
     * From this point, the page is on disk and checkpoint cleanup should NOT induce a read if at
     * least of one of the following conditions is met:
     *
     * - The page is a leaf with no overflow items because obsolete leaf pages with overflow
     * keys/values cannot be fast deleted to free the overflow blocks. The overflow blocks can be
     * freed during reconciliation after being marked dirty.
     *
     * - The page does not have any deletes (checked using the aggregated stop durable timestamp)
     * AND
     *  - the checkpoint cleanup is not configured with the reclaim space method OR
     *  - the table is not logged
     *
     * FIXME: Read internal pages from non-logged tables when the remove/truncate
     * operation is performed using no timestamp.
     */
    if (addr.type == WT_ADDR_LEAF_NO)
        *skipp = true;
    else if (addr.ta.newest_stop_durable_ts == WT_TS_NONE) {
        /* Only process logged tables when checkpoint cleanup is configured to be aggressive. */
        *skipp = !F_ISSET(S2C(session), WT_CONN_CKPT_CLEANUP_RECLAIM_SPACE) ||
          !F_ISSET(S2BT(session), WT_BTREE_LOGGED);
        if (!*skipp)
            WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_read_reclaim_space);
    }

    /*
     * While we may have decided to skip the page, check if there is obsolete content that can be
     * cleaned up.
     */
    if (*skipp && __sync_obsolete_tw_check(session, addr.ta)) {
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_read_obsolete_tw);
        *skipp = false;
    }

    if (*skipp) {
        __wt_verbose_debug2(
          session, WT_VERB_CHECKPOINT_CLEANUP, "%p: page walk skipped", (void *)ref);
        WT_STAT_CONN_DSRC_INCR(session, checkpoint_cleanup_pages_walk_skipped);
    }

    return (0);
}

/*
 * __checkpoint_cleanup_walk_btree --
 *     Check and perform checkpoint cleanup on the uri.
 */
static int
__checkpoint_cleanup_walk_btree(WT_SESSION_IMPL *session, WT_ITEM *uri)
{
    WT_BTREE *btree;
    WT_DECL_RET;
    WT_REF *ref;
    uint32_t flags;

    ref = NULL;
    flags = WT_READ_NO_EVICT | WT_READ_VISIBLE_ALL;

    /* Open a handle for processing. */
    ret = __wt_session_get_dhandle(session, uri->data, NULL, NULL, 0);
    if (ret != 0) {
        __wt_verbose_debug1(session, WT_VERB_CHECKPOINT_CLEANUP, "%s: unable to open handle%s",
          (char *)uri->data,
          ret == EBUSY ? ", error indicates handle is unavailable due to concurrent use" : "");
        return (ret);
    }

    btree = S2BT(session);
    /* There is nothing to do on an empty tree. */
    if (btree->root.page == NULL)
        goto err;

    /* Ignore tables that are empty or is currently in a bulk-load phase. */
    if (btree->original)
        goto err;
    /*
     * FLCS pages cannot be discarded and must be rewritten as implicitly filling in missing chunks
     * of FLCS namespace is problematic.
     */
    if (btree->type == BTREE_COL_FIX)
        goto err;

    /* Walk the tree. */
    while ((ret = __wt_tree_walk_custom_skip(
              session, &ref, __checkpoint_cleanup_page_skip, NULL, flags)) == 0 &&
      ref != NULL) {
        if (F_ISSET(ref, WT_REF_FLAG_INTERNAL)) {
            WT_WITH_PAGE_INDEX(session, ret = __checkpoint_cleanup_obsolete_cleanup(session, ref));
        } else {
            WT_ENTER_GENERATION(session, WT_GEN_SPLIT);
            ret = __sync_obsolete_inmem_evict_or_mark_dirty(session, ref);
            WT_LEAVE_GENERATION(session, WT_GEN_SPLIT);
        }
        WT_ERR(ret);

        /* Check if we're quitting. */
        if (!__checkpoint_cleanup_run_chk(session))
            break;
    }

err:
    /* On error, clear any left-over tree walk. */
    WT_TRET(__wt_page_release(session, ref, flags));
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __checkpoint_cleanup_eligibility --
 *     Function to check whether the specified URI is eligible for checkpoint cleanup.
 */
static bool
__checkpoint_cleanup_eligibility(WT_SESSION_IMPL *session, const char *uri, const char *config)
{
    WT_CONFIG ckptconf;
    WT_CONFIG_ITEM cval, key, value;
    WT_DECL_RET;
    wt_timestamp_t newest_start_durable_ts, newest_stop_durable_ts;
    size_t addr_size;
    uint64_t newest_txn, write_gen;

    newest_start_durable_ts = newest_stop_durable_ts = WT_TS_NONE;
    newest_txn = WT_TXN_NONE;
    addr_size = write_gen = 0;

    /* Checkpoint cleanup cannot remove obsolete pages from tiered tables. */
    if (WT_SUFFIX_MATCH(uri, ".wtobj"))
        return (false);

    /*
     * History store table. This table contains the historical versions that are needed to be
     * removed regularly.
     */
    if (strcmp(uri, WT_HS_URI) == 0)
        return (true);

    /*
     * To reduce the impact of checkpoint cleanup on the running database, it operates only on the
     * dhandles that are already opened.
     */
    WT_WITHOUT_DHANDLE(session,
      WT_WITH_HANDLE_LIST_READ_LOCK(session, (ret = __wt_conn_dhandle_find(session, uri, NULL))));
    if (ret == WT_NOTFOUND)
        return (false);

    /*
     * Logged table. The logged tables do not support timestamps, so we need to check for obsolete
     * pages in them.
     */
    if (F_ISSET(&S2C(session)->log_mgr, WT_LOG_ENABLED)) {
        WT_RET(__wt_config_getones(session, config, "log.enabled", &cval));
        if (cval.val)
            return (true);
    }

    WT_RET(__wt_config_getones(session, config, "checkpoint", &cval));
    __wt_config_subinit(session, &ckptconf, &cval);
    while ((ret = __wt_config_next(&ckptconf, &key, &cval)) == 0) {
        ret = __wt_config_subgets(session, &cval, "addr", &value);
        if (ret == 0)
            addr_size = value.len;
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "newest_start_durable_ts", &value);
        if (ret == 0)
            newest_start_durable_ts = WT_MAX(newest_start_durable_ts, (wt_timestamp_t)value.val);
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "newest_stop_durable_ts", &value);
        if (ret == 0)
            newest_stop_durable_ts = WT_MAX(newest_stop_durable_ts, (wt_timestamp_t)value.val);
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "newest_txn", &value);
        if (ret == 0)
            newest_txn = WT_MAX(newest_txn, (uint64_t)value.val);
        WT_RET_NOTFOUND_OK(ret);
        ret = __wt_config_subgets(session, &cval, "write_gen", &value);
        if (ret == 0)
            write_gen = WT_MAX(write_gen, (uint64_t)value.val);
        WT_RET_NOTFOUND_OK(ret);
    }
    WT_RET_NOTFOUND_OK(ret);

    /* Skip an empty or newly created table. */
    if (addr_size == 0)
        return (false);

    /* The table has a durable stop timestamp. */
    if (newest_stop_durable_ts != WT_TS_NONE)
        return (true);

    /*
     * We don't want to rely on transaction IDs that come from a previous run because they will be
     * very likely bigger than the current oldest one as WiredTiger restarts the transaction
     * counters upon restart. Transaction IDs from a previous run can be detected using the write
     * generation number.
     */
    if (write_gen < S2C(session)->base_write_gen)
        newest_txn = WT_TXN_NONE;

    /*
     * The checkpoint has some obsolete time window information that is no longer required to exist
     * in the btree. Remove the obsolete data to reduce the checkpoint size. Note the visibility
     * check is done later on at the page level when walking the tree to find the right page that
     * has obsolete content.
     *
     * FIXME-WT-13321 Rely on the oldest_start_ts rather than the newest_*_ts.
     */
    if (__wt_txn_has_newest_and_visible_all(
          session, newest_txn, WT_MAX(newest_start_durable_ts, newest_stop_durable_ts)))
        return (true);

    return (false);
}

/*
 * __checkpoint_cleanup_get_uri --
 *     Given a URI, find the next one in the metadata.
 */
static int
__checkpoint_cleanup_get_uri(WT_SESSION_IMPL *session, WT_ITEM *uri)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    int exact;
    const char *key, *value;

    cursor = NULL;
    exact = 0;
    key = value = NULL;

    /* Use a metadata cursor to have access to the existing URIs. */
    WT_ERR(__wt_metadata_cursor(session, &cursor));

    /* Position the cursor on the given URI. */
    cursor->set_key(cursor, (const char *)uri->data);
    WT_ERR(cursor->search_near(cursor, &exact));

    /*
     * The given URI may not exist in the metadata file. Since we always want to return a URI that
     * is lexicographically larger the given one, make sure not to go backwards.
     */
    if (exact <= 0)
        WT_ERR(cursor->next(cursor));

    /* Loop through the eligible candidates. */
    do {
        WT_ERR(cursor->get_key(cursor, &key));

        /* Check we are still dealing with keys that have the right prefix. */
        if (!WT_PREFIX_MATCH(key, WT_URI_FILE_PREFIX)) {
            ret = WT_NOTFOUND;
            break;
        }

        WT_ERR(cursor->get_value(cursor, &value));
        /* Check the given uri needs checkpoint cleanup. */
        if (__checkpoint_cleanup_eligibility(session, key, value))
            break;
    } while ((ret = cursor->next(cursor)) == 0);
    WT_ERR(ret);

    /* Save the selected uri. */
    WT_ERR(__wt_buf_set(session, uri, cursor->key.data, cursor->key.size));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __checkpoint_cleanup_int --
 *     Internal function to perform checkpoint cleanup of all eligible files.
 */
static int
__checkpoint_cleanup_int(WT_SESSION_IMPL *session)
{
    WT_DECL_ITEM(uri);
    WT_DECL_RET;

    WT_RET(__wt_scr_alloc(session, 1024, &uri));
    WT_ERR(__wt_buf_set(session, uri, WT_URI_FILE_PREFIX, strlen(WT_URI_FILE_PREFIX) + 1));

    while ((ret = __checkpoint_cleanup_get_uri(session, uri)) == 0) {
        ret = __checkpoint_cleanup_walk_btree(session, uri);
        if (ret == ENOENT || ret == EBUSY) {
            __wt_verbose_debug1(session, WT_VERB_CHECKPOINT_CLEANUP,
              "%s: skipped performing checkpoint cleanup because the file %s", (char *)uri->data,
              ret == ENOENT ? "does not exist" : "is busy");
            continue;
        }
        WT_ERR(ret);

        /* Check if we need to wait before continuing with the next file to minimize impact. */
        if (S2C(session)->cc_cleanup.file_wait_ms > 0) {
            __wt_verbose_debug1(session, WT_VERB_CHECKPOINT_CLEANUP,
              "waiting for %" PRIu64
              " milliseconds after %s cleanup before continuing with the next file",
              S2C(session)->cc_cleanup.file_wait_ms, (char *)uri->data);

            __wt_cond_wait(session, S2C(session)->cc_cleanup.cond,
              S2C(session)->cc_cleanup.file_wait_ms * WT_THOUSAND, __checkpoint_cleanup_run_chk);
        }

        /* Check if we're quitting. */
        if (!__checkpoint_cleanup_run_chk(session))
            break;
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_scr_free(session, &uri);
    return (ret);
}

/*
 * __checkpoint_cleanup --
 *     The checkpoint cleanup thread.
 */
static WT_THREAD_RET
__checkpoint_cleanup(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t cleanup_interval, last, now;
    bool cv_signalled;

    session = arg;
    conn = S2C(session);

    __wt_seconds(session, &last);
    for (;;) {
        /* We want to ensure the thread checks often enough if it is supposed to work. */
        cleanup_interval =
          WT_MIN(conn->cc_cleanup.interval, WT_CHECKPOINT_CLEANUP_DEFAULT_WAKE_UP_INTERVAL);

        /* Check periodically in case the signal was missed. */
        __wt_cond_wait_signal(session, conn->cc_cleanup.cond, cleanup_interval * WT_MILLION,
          __checkpoint_cleanup_run_chk, &cv_signalled);

        /* Check if we're quitting. */
        if (!__checkpoint_cleanup_run_chk(session))
            break;

        __wt_seconds(session, &now);

        /*
         * See if it is time to checkpoint cleanup. Checkpoint cleanup is an operation that
         * typically involves many IO operations so skipping some should have little impact.
         */
        if (!cv_signalled && (now - last < conn->cc_cleanup.interval))
            continue;

        WT_ERR(__checkpoint_cleanup_int(session));
        WT_STAT_CONN_INCR(session, checkpoint_cleanup_success);
        last = now;
    }

err:
    if (ret != 0)
        WT_IGNORE_RET(__wt_panic(session, ret, "checkpoint cleanup error"));
    return (WT_THREAD_RET_VALUE);
}

/*
 * __wt_checkpoint_cleanup_create --
 *     Start the checkpoint cleanup thread.
 */
int
__wt_checkpoint_cleanup_create(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    uint32_t session_flags;

    conn = S2C(session);

    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* Set first, the thread might run before we finish up. */
    FLD_SET(conn->server_flags, WT_CONN_SERVER_CHECKPOINT_CLEANUP);

    WT_RET(__wt_config_gets(session, cfg, "checkpoint_cleanup.method", &cval));
    if (WT_CONFIG_LIT_MATCH("reclaim_space", cval))
        F_SET(conn, WT_CONN_CKPT_CLEANUP_RECLAIM_SPACE);

    WT_RET(__wt_config_gets(session, cfg, "checkpoint_cleanup.wait", &cval));
    conn->cc_cleanup.interval = (uint64_t)cval.val;

    WT_RET(__wt_config_gets(session, cfg, "checkpoint_cleanup.file_wait_ms", &cval));
    conn->cc_cleanup.file_wait_ms = (uint64_t)cval.val;

    /*
     * Checkpoint cleanup does enough I/O it may be called upon to perform slow operations for the
     * block manager.
     */
    session_flags = WT_SESSION_CAN_WAIT;
    WT_RET(__wt_open_internal_session(
      conn, "checkpoint-cleanup", true, session_flags, 0, &conn->cc_cleanup.session));
    session = conn->cc_cleanup.session;

    WT_RET(__wt_cond_alloc(session, "checkpoint cleanup", &conn->cc_cleanup.cond));

    WT_RET(__wt_thread_create(session, &conn->cc_cleanup.tid, __checkpoint_cleanup, session));
    conn->cc_cleanup.tid_set = true;

    return (0);
}

/*
 * __wt_checkpoint_cleanup_destroy --
 *     Destroy the checkpoint cleanup thread.
 */
int
__wt_checkpoint_cleanup_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CHECKPOINT_CLEANUP);
    if (conn->cc_cleanup.tid_set) {
        __wt_cond_signal(session, conn->cc_cleanup.cond);
        WT_TRET(__wt_thread_join(session, &conn->cc_cleanup.tid));
        conn->cc_cleanup.tid_set = false;
    }
    __wt_cond_destroy(session, &conn->cc_cleanup.cond);

    /* Close the server thread's session. */
    if (conn->cc_cleanup.session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->cc_cleanup.session));
        conn->cc_cleanup.session = NULL;
    }

    return (ret);
}

/*
 * __wt_checkpoint_cleanup_trigger --
 *     Trigger the checkpoint cleanup thread.
 */
void
__wt_checkpoint_cleanup_trigger(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (conn->cc_cleanup.tid_set)
        __wt_cond_signal(session, conn->cc_cleanup.cond);
}
