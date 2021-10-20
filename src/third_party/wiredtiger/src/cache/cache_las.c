/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * When an operation is accessing the lookaside table, it should ignore the cache size (since the
 * cache is already full), any pages it reads should be evicted before application data, and the
 * operation can't reenter reconciliation.
 */
#define WT_LAS_SESSION_FLAGS \
    (WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_READ_WONT_NEED | WT_SESSION_NO_RECONCILE)

/*
 * __las_set_isolation --
 *     Switch to read-uncommitted.
 */
static void
__las_set_isolation(WT_SESSION_IMPL *session, WT_TXN_ISOLATION *saved_isolationp)
{
    *saved_isolationp = session->txn.isolation;
    session->txn.isolation = WT_ISO_READ_UNCOMMITTED;
}

/*
 * __las_restore_isolation --
 *     Restore isolation.
 */
static void
__las_restore_isolation(WT_SESSION_IMPL *session, WT_TXN_ISOLATION saved_isolation)
{
    session->txn.isolation = saved_isolation;
}

/*
 * __las_entry_count --
 *     Return when there are entries in the lookaside table.
 */
static uint64_t
__las_entry_count(WT_CACHE *cache)
{
    uint64_t insert_cnt, remove_cnt;

    insert_cnt = cache->las_insert_count;
    WT_ORDERED_READ(remove_cnt, cache->las_remove_count);

    return (insert_cnt > remove_cnt ? insert_cnt - remove_cnt : 0);
}

/*
 * __wt_las_config --
 *     Configure the lookaside table.
 */
int
__wt_las_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR_BTREE *las_cursor;
    WT_SESSION_IMPL *las_session;

    WT_RET(__wt_config_gets(session, cfg, "cache_overflow.file_max", &cval));

    if (cval.val != 0 && cval.val < WT_LAS_FILE_MIN)
        WT_RET_MSG(session, EINVAL, "max cache overflow size %" PRId64 " below minimum %d",
          cval.val, WT_LAS_FILE_MIN);

    /* This is expected for in-memory configurations. */
    las_session = S2C(session)->cache->las_session[0];
    WT_ASSERT(session, las_session != NULL || F_ISSET(S2C(session), WT_CONN_IN_MEMORY));

    if (las_session == NULL)
        return (0);

    /*
     * We need to set file_max on the btree associated with one of the lookaside sessions.
     */
    las_cursor = (WT_CURSOR_BTREE *)las_session->las_cursor;
    las_cursor->btree->file_max = (uint64_t)cval.val;

    WT_STAT_CONN_SET(session, cache_lookaside_ondisk_max, las_cursor->btree->file_max);

    return (0);
}

/*
 * __wt_las_empty --
 *     Return when there are entries in the lookaside table.
 */
bool
__wt_las_empty(WT_SESSION_IMPL *session)
{
    return (__las_entry_count(S2C(session)->cache) == 0);
}

/*
 * __wt_las_stats_update --
 *     Update the lookaside table statistics for return to the application.
 */
void
__wt_las_stats_update(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_CONNECTION_STATS **cstats;
    WT_DSRC_STATS **dstats;
    int64_t v;

    conn = S2C(session);
    cache = conn->cache;

    /*
     * Lookaside table statistics are copied from the underlying lookaside table data-source
     * statistics. If there's no lookaside table, values remain 0.
     */
    if (!F_ISSET(conn, WT_CONN_LOOKASIDE_OPEN))
        return;

    /* Set the connection-wide statistics. */
    cstats = conn->stats;

    WT_STAT_SET(session, cstats, cache_lookaside_entries, __las_entry_count(cache));

    /*
     * We have a cursor, and we need the underlying data handle; we can get to it by way of the
     * underlying btree handle, but it's a little ugly.
     */
    dstats = ((WT_CURSOR_BTREE *)cache->las_session[0]->las_cursor)->btree->dhandle->stats;

    v = WT_STAT_READ(dstats, cursor_update);
    WT_STAT_SET(session, cstats, cache_lookaside_insert, v);
    v = WT_STAT_READ(dstats, cursor_remove);
    WT_STAT_SET(session, cstats, cache_lookaside_remove, v);

    /*
     * If we're clearing stats we need to clear the cursor values we just read. This does not clear
     * the rest of the statistics in the lookaside data source stat cursor, but we own that
     * namespace so we don't have to worry about users seeing inconsistent data source information.
     */
    if (FLD_ISSET(conn->stat_flags, WT_STAT_CLEAR)) {
        WT_STAT_SET(session, dstats, cursor_insert, 0);
        WT_STAT_SET(session, dstats, cursor_remove, 0);
    }
}

/*
 * __wt_las_create --
 *     Initialize the database's lookaside store.
 */
int
__wt_las_create(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    int i;
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL};

    conn = S2C(session);
    cache = conn->cache;

    /* Read-only and in-memory configurations don't need the LAS table. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /*
     * Done at startup: we cannot do it on demand because we require the
     * schema lock to create and drop the table, and it may not always be
     * available.
     *
     * Discard any previous incarnation of the table.
     */
    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_drop(session, WT_LAS_URI, drop_cfg));
    WT_RET(ret);

    /* Re-create the table. */
    WT_RET(__wt_session_create(session, WT_LAS_URI, WT_LAS_CONFIG));

    /*
     * Open a shared internal session and cursor used for the lookaside table. This session should
     * never perform reconciliation.
     */
    for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
        WT_RET(__wt_open_internal_session(
          conn, "lookaside table", true, WT_LAS_SESSION_FLAGS, &cache->las_session[i]));
        WT_RET(__wt_las_cursor_open(cache->las_session[i]));
    }

    WT_RET(__wt_las_config(session, cfg));

    /* The statistics server is already running, make sure we don't race. */
    WT_WRITE_BARRIER();
    F_SET(conn, WT_CONN_LOOKASIDE_OPEN);

    return (0);
}

/*
 * __wt_las_destroy --
 *     Destroy the database's lookaside store.
 */
int
__wt_las_destroy(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION *wt_session;
    int i;

    conn = S2C(session);
    cache = conn->cache;

    F_CLR(conn, WT_CONN_LOOKASIDE_OPEN);
    if (cache == NULL)
        return (0);

    for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
        if (cache->las_session[i] == NULL)
            continue;

        wt_session = &cache->las_session[i]->iface;
        WT_TRET(wt_session->close(wt_session, NULL));
        cache->las_session[i] = NULL;
    }

    __wt_buf_free(session, &cache->las_sweep_key);
    __wt_free(session, cache->las_dropped);
    __wt_free(session, cache->las_sweep_dropmap);

    return (ret);
}

/*
 * __wt_las_cursor_open --
 *     Open a new lookaside table cursor.
 */
int
__wt_las_cursor_open(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_LAS_URI, NULL, open_cursor_cfg, &cursor));
    WT_RET(ret);

    /*
     * Retrieve the btree from the cursor, rather than the session because we don't always switch
     * the LAS handle in to the session before entering this function.
     */
    btree = ((WT_CURSOR_BTREE *)cursor)->btree;

    /* Track the lookaside file ID. */
    if (S2C(session)->cache->las_fileid == 0)
        S2C(session)->cache->las_fileid = btree->id;

    /*
     * Set special flags for the lookaside table: the lookaside flag (used,
     * for example, to avoid writing records during reconciliation), also
     * turn off checkpoints and logging.
     *
     * Test flags before setting them so updates can't race in subsequent
     * opens (the first update is safe because it's single-threaded from
     * wiredtiger_open).
     */
    if (!F_ISSET(btree, WT_BTREE_LOOKASIDE))
        F_SET(btree, WT_BTREE_LOOKASIDE);
    if (!F_ISSET(btree, WT_BTREE_NO_CHECKPOINT))
        F_SET(btree, WT_BTREE_NO_CHECKPOINT);
    if (!F_ISSET(btree, WT_BTREE_NO_LOGGING))
        F_SET(btree, WT_BTREE_NO_LOGGING);

    session->las_cursor = cursor;
    F_SET(session, WT_SESSION_LOOKASIDE_CURSOR);

    return (0);
}

/*
 * __wt_las_cursor --
 *     Return a lookaside cursor.
 */
void
__wt_las_cursor(WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t *session_flags)
{
    WT_CACHE *cache;
    int i;

    *cursorp = NULL;

    /*
     * We don't want to get tapped for eviction after we start using the
     * lookaside cursor; save a copy of the current eviction state, we'll
     * turn eviction off before we return.
     *
     * Don't cache lookaside table pages, we're here because of eviction
     * problems and there's no reason to believe lookaside pages will be
     * useful more than once.
     */
    *session_flags = F_MASK(session, WT_LAS_SESSION_FLAGS);

    cache = S2C(session)->cache;

    /*
     * Some threads have their own lookaside table cursors, else lock the shared lookaside cursor.
     */
    if (F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR))
        *cursorp = session->las_cursor;
    else {
        for (;;) {
            __wt_spin_lock(session, &cache->las_lock);
            for (i = 0; i < WT_LAS_NUM_SESSIONS; i++) {
                if (!cache->las_session_inuse[i]) {
                    *cursorp = cache->las_session[i]->las_cursor;
                    cache->las_session_inuse[i] = true;
                    break;
                }
            }
            __wt_spin_unlock(session, &cache->las_lock);
            if (*cursorp != NULL)
                break;
            /*
             * If all the lookaside sessions are busy, stall.
             *
             * XXX better as a condition variable.
             */
            __wt_sleep(0, WT_THOUSAND);
            if (F_ISSET(session, WT_SESSION_INTERNAL))
                WT_STAT_CONN_INCRV(session, cache_lookaside_cursor_wait_internal, WT_THOUSAND);
            else
                WT_STAT_CONN_INCRV(session, cache_lookaside_cursor_wait_application, WT_THOUSAND);
        }
    }

    /* Configure session to access the lookaside table. */
    F_SET(session, WT_LAS_SESSION_FLAGS);
}

/*
 * __wt_las_cursor_close --
 *     Discard a lookaside cursor.
 */
int
__wt_las_cursor_close(WT_SESSION_IMPL *session, WT_CURSOR **cursorp, uint32_t session_flags)
{
    WT_CACHE *cache;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    int i;

    cache = S2C(session)->cache;

    if ((cursor = *cursorp) == NULL)
        return (0);
    *cursorp = NULL;

    /* Reset the cursor. */
    ret = cursor->reset(cursor);

    /*
     * We turned off caching and eviction while the lookaside cursor was in use, restore the
     * session's flags.
     */
    F_CLR(session, WT_LAS_SESSION_FLAGS);
    F_SET(session, session_flags);

    /*
     * Some threads have their own lookaside table cursors, else unlock the shared lookaside cursor.
     */
    if (!F_ISSET(session, WT_SESSION_LOOKASIDE_CURSOR)) {
        __wt_spin_lock(session, &cache->las_lock);
        for (i = 0; i < WT_LAS_NUM_SESSIONS; i++)
            if (cursor->session == &cache->las_session[i]->iface) {
                cache->las_session_inuse[i] = false;
                break;
            }
        __wt_spin_unlock(session, &cache->las_lock);
        WT_ASSERT(session, i != WT_LAS_NUM_SESSIONS);
    }

    return (ret);
}

/*
 * __wt_las_page_skip_locked --
 *     Check if we can skip reading a page with lookaside entries, where the page is already locked.
 */
bool
__wt_las_page_skip_locked(WT_SESSION_IMPL *session, WT_REF *ref)
{
    WT_TXN *txn;

    txn = &session->txn;

    /*
     * Skip lookaside pages if reading without a timestamp and all the
     * updates in lookaside are in the past.
     *
     * Lookaside eviction preferentially chooses the newest updates when
     * creating page images with no stable timestamp. If a stable timestamp
     * has been set, we have to visit the page because eviction chooses old
     * version of records in that case.
     *
     * One case where we may need to visit the page is if lookaside eviction
     * is active in tree 2 when a checkpoint has started and is working its
     * way through tree 1. In that case, lookaside may have created a page
     * image with updates in the future of the checkpoint.
     *
     * We also need to instantiate a lookaside page if this is an update
     * operation in progress or transaction is in prepared state.
     */
    if (F_ISSET(txn, WT_TXN_PREPARE | WT_TXN_UPDATE))
        return (false);

    if (!F_ISSET(txn, WT_TXN_HAS_SNAPSHOT))
        return (false);

    /*
     * If some of the page's history overlaps with the reader's snapshot then we have to read it.
     */
    if (WT_TXNID_LE(txn->snap_min, ref->page_las->max_txn))
        return (false);

    /*
     * Otherwise, if not reading at a timestamp, the page's history is in the past, so the page
     * image is correct if it contains the most recent versions of everything and nothing was
     * prepared.
     */
    if (!F_ISSET(txn, WT_TXN_HAS_TS_READ))
        return (!ref->page_las->has_prepares && ref->page_las->min_skipped_ts == WT_TS_MAX);

    /*
     * Skip lookaside history if reading as of a timestamp, we evicted new
     * versions of data and all the updates are in the past.  This is not
     * possible for prepared updates, because the commit timestamp was not
     * known when the page was evicted.
     *
     * Otherwise, skip reading lookaside history if everything on the page
     * is older than the read timestamp, and the oldest update in lookaside
     * newer than the page is in the future of the reader.  This seems
     * unlikely, but is exactly what eviction tries to do when a checkpoint
     * is running.
     */
    if (!ref->page_las->has_prepares && ref->page_las->min_skipped_ts == WT_TS_MAX &&
      txn->read_timestamp >= ref->page_las->max_ondisk_ts)
        return (true);

    if (txn->read_timestamp >= ref->page_las->max_ondisk_ts &&
      txn->read_timestamp < ref->page_las->min_skipped_ts)
        return (true);

    return (false);
}

/*
 * __wt_las_page_skip --
 *     Check if we can skip reading a page with lookaside entries, where the page needs to be locked
 *     before checking.
 */
bool
__wt_las_page_skip(WT_SESSION_IMPL *session, WT_REF *ref)
{
    uint32_t previous_state;
    bool skip;

    if ((previous_state = ref->state) != WT_REF_LIMBO && previous_state != WT_REF_LOOKASIDE)
        return (false);

    if (!WT_REF_CAS_STATE(session, ref, previous_state, WT_REF_LOCKED))
        return (false);

    skip = __wt_las_page_skip_locked(session, ref);

    /* Restore the state and push the change. */
    WT_REF_SET_STATE(ref, previous_state);
    WT_FULL_BARRIER();

    return (skip);
}

/*
 * __las_remove_block --
 *     Remove all records for a given page from the lookaside store.
 */
static int
__las_remove_block(WT_CURSOR *cursor, uint64_t pageid, bool lock_wait, uint64_t *remove_cntp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM las_key;
    WT_SESSION_IMPL *session;
    WT_TXN_ISOLATION saved_isolation;
    uint64_t las_counter, las_pageid;
    uint32_t las_id;
    bool local_txn;

    *remove_cntp = 0;

    session = (WT_SESSION_IMPL *)cursor->session;
    conn = S2C(session);
    local_txn = false;

    /* Prevent the sweep thread from removing the block. */
    if (lock_wait)
        __wt_writelock(session, &conn->cache->las_sweepwalk_lock);
    else
        WT_RET(__wt_try_writelock(session, &conn->cache->las_sweepwalk_lock));

    __las_set_isolation(session, &saved_isolation);
    WT_ERR(__wt_txn_begin(session, NULL));
    local_txn = true;

    /*
     * Search for the block's unique btree ID and page ID prefix and step through all matching
     * records, removing them.
     */
    for (ret = __wt_las_cursor_position(cursor, pageid); ret == 0; ret = cursor->next(cursor)) {
        WT_ERR(cursor->get_key(cursor, &las_pageid, &las_id, &las_counter, &las_key));

        /* Confirm that we have a matching record. */
        if (las_pageid != pageid)
            break;

        WT_ERR(cursor->remove(cursor));
        ++*remove_cntp;
    }
    WT_ERR_NOTFOUND_OK(ret);

err:
    if (local_txn) {
        if (ret == 0)
            ret = __wt_txn_commit(session, NULL);
        else
            WT_TRET(__wt_txn_rollback(session, NULL));
    }

    __las_restore_isolation(session, saved_isolation);
    __wt_writeunlock(session, &conn->cache->las_sweepwalk_lock);
    return (ret);
}

/*
 * __las_insert_block_verbose --
 *     Display a verbose message once per checkpoint with details about the cache state when
 *     performing a lookaside table write.
 */
static void
__las_insert_block_verbose(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_MULTI *multi)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    double pct_dirty, pct_full;
    uint64_t ckpt_gen_current, ckpt_gen_last;
    uint32_t btree_id;
    char max_ondisk_ts[WT_TS_HEX_SIZE], min_skipped_ts[WT_TS_HEX_SIZE];

    btree_id = btree->id;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY))
        return;

    conn = S2C(session);
    cache = conn->cache;
    ckpt_gen_current = __wt_gen(session, WT_GEN_CHECKPOINT);
    ckpt_gen_last = cache->las_verb_gen_write;

    /*
     * Print a message if verbose lookaside, or once per checkpoint if only reporting activity.
     * Avoid an expensive atomic operation as often as possible when the message rate is limited.
     */
    if (WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE) ||
      (ckpt_gen_current > ckpt_gen_last &&
          __wt_atomic_casv64(&cache->las_verb_gen_write, ckpt_gen_last, ckpt_gen_current))) {
        (void)__wt_eviction_clean_needed(session, &pct_full);
        (void)__wt_eviction_dirty_needed(session, &pct_dirty);

        __wt_timestamp_to_hex_string(max_ondisk_ts, multi->page_las.max_ondisk_ts);
        __wt_timestamp_to_hex_string(min_skipped_ts, multi->page_las.min_skipped_ts);
        __wt_verbose(session, WT_VERB_LOOKASIDE | WT_VERB_LOOKASIDE_ACTIVITY,
          "Page reconciliation triggered lookaside write "
          "file ID %" PRIu32 ", page ID %" PRIu64
          ". "
          "Max txn ID %" PRIu64
          ", max ondisk timestamp %s, "
          "first skipped ts %s. "
          "Entries now in lookaside file: %" PRId64
          ", "
          "cache dirty: %2.3f%% , "
          "cache use: %2.3f%%",
          btree_id, multi->page_las.las_pageid, multi->page_las.max_txn, max_ondisk_ts,
          min_skipped_ts, WT_STAT_READ(conn->stats, cache_lookaside_entries), pct_dirty, pct_full);
    }

    /* Never skip updating the tracked generation */
    if (WT_VERBOSE_ISSET(session, WT_VERB_LOOKASIDE))
        cache->las_verb_gen_write = ckpt_gen_current;
}

/*
 * __wt_las_insert_block --
 *     Copy one set of saved updates into the database's lookaside table.
 */
int
__wt_las_insert_block(
  WT_CURSOR *cursor, WT_BTREE *btree, WT_PAGE *page, WT_MULTI *multi, WT_ITEM *key)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM las_value;
    WT_SAVE_UPD *list;
    WT_SESSION_IMPL *session;
    WT_TXN_ISOLATION saved_isolation;
    WT_UPDATE *first_upd, *upd;
    wt_off_t las_size;
    uint64_t insert_cnt, las_counter, las_pageid, max_las_size;
    uint64_t prepared_insert_cnt;
    uint32_t btree_id, i, slot;
    uint8_t *p;
    bool local_txn;

    session = (WT_SESSION_IMPL *)cursor->session;
    conn = S2C(session);
    WT_CLEAR(las_value);
    insert_cnt = prepared_insert_cnt = 0;
    btree_id = btree->id;
    local_txn = false;

    las_pageid = __wt_atomic_add64(&conn->cache->las_pageid, 1);

    if (!btree->lookaside_entries)
        btree->lookaside_entries = true;

#ifdef HAVE_DIAGNOSTIC
    {
        uint64_t remove_cnt;
        /*
         * There should never be any entries with the page ID we are about to use.
         */
        WT_RET_BUSY_OK(__las_remove_block(cursor, las_pageid, false, &remove_cnt));
        WT_ASSERT(session, remove_cnt == 0);
    }
#endif

    /* Wrap all the updates in a transaction. */
    __las_set_isolation(session, &saved_isolation);
    WT_ERR(__wt_txn_begin(session, NULL));
    local_txn = true;

    /* Inserts should be on the same page absent a split, search any pinned leaf page. */
    F_SET(cursor, WT_CURSTD_UPDATE_LOCAL);

    /* Enter each update in the boundary's list into the lookaside store. */
    for (las_counter = 0, i = 0, list = multi->supd; i < multi->supd_entries; ++i, ++list) {
        /* Lookaside table key component: source key. */
        switch (page->type) {
        case WT_PAGE_COL_FIX:
        case WT_PAGE_COL_VAR:
            p = key->mem;
            WT_ERR(__wt_vpack_uint(&p, 0, WT_INSERT_RECNO(list->ins)));
            key->size = WT_PTRDIFF(p, key->data);
            break;
        case WT_PAGE_ROW_LEAF:
            if (list->ins == NULL) {
                WT_WITH_BTREE(
                  session, btree, ret = __wt_row_leaf_key(session, page, list->ripcip, key, false));
                WT_ERR(ret);
            } else {
                key->data = WT_INSERT_KEY(list->ins);
                key->size = WT_INSERT_KEY_SIZE(list->ins);
            }
            break;
        default:
            WT_ERR(__wt_illegal_value(session, page->type));
        }

        /*
         * Lookaside table value component: update reference. Updates come from the row-store insert
         * list (an inserted item), or update array (an update to an original on-page item), or from
         * a column-store insert list (column-store format has no update array, the insert list
         * contains both inserted items and updates to original on-page items). When rolling forward
         * a modify update from an original on-page item, we need an on-page slot so we can find the
         * original on-page item. When rolling forward from an inserted item, no on-page slot is
         * possible.
         */
        slot = UINT32_MAX; /* Impossible slot */
        if (list->ripcip != NULL)
            slot = page->type == WT_PAGE_ROW_LEAF ? WT_ROW_SLOT(page, list->ripcip) :
                                                    WT_COL_SLOT(page, list->ripcip);
        first_upd = list->ins == NULL ? page->modify->mod_row_update[slot] : list->ins->upd;

        /*
         * Trim any updates before writing to lookaside. This saves wasted work, but is also
         * necessary because the reconciliation only resolves existing birthmarks if they aren't
         * obsolete.
         */
        WT_WITH_BTREE(
          session, btree, upd = __wt_update_obsolete_check(session, page, first_upd, true));
        if (upd != NULL)
            __wt_free_update_list(session, upd);
        upd = first_upd;

        /*
         * It's not OK for the update list to contain a birthmark on entry - we will generate one
         * below if necessary.
         */
        WT_ASSERT(session, __wt_count_birthmarks(first_upd) == 0);

        /*
         * Walk the list of updates, storing each key/value pair into the lookaside table. Skip
         * aborted items (there's no point to restoring them), and assert we never see a reserved
         * item.
         */
        do {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;

            switch (upd->type) {
            case WT_UPDATE_MODIFY:
            case WT_UPDATE_STANDARD:
                las_value.data = upd->data;
                las_value.size = upd->size;
                break;
            case WT_UPDATE_TOMBSTONE:
                las_value.size = 0;
                break;
            default:
                /*
                 * It is never OK to see a birthmark here - it would be referring to the wrong page
                 * image.
                 */
                WT_ERR(__wt_illegal_value(session, upd->type));
            }

            cursor->set_key(cursor, las_pageid, btree_id, ++las_counter, key);

            /*
             * If saving a non-zero length value on the page, save a birthmark instead of
             * duplicating it in the lookaside table. (We check the length because row-store doesn't
             * write zero-length data items.)
             */
            if (upd == list->onpage_upd && upd->size > 0 &&
              (upd->type == WT_UPDATE_STANDARD || upd->type == WT_UPDATE_MODIFY)) {
                las_value.size = 0;
                cursor->set_value(cursor, upd->txnid, upd->timestamp, upd->prepare_state,
                  WT_UPDATE_BIRTHMARK, &las_value);
            } else
                cursor->set_value(
                  cursor, upd->txnid, upd->timestamp, upd->prepare_state, upd->type, &las_value);

            /*
             * Using update instead of insert so the page stays pinned and can be searched before
             * the tree.
             */
            WT_ERR(cursor->update(cursor));
            ++insert_cnt;
            if (upd->prepare_state == WT_PREPARE_INPROGRESS)
                ++prepared_insert_cnt;
        } while ((upd = upd->next) != NULL);
    }

    WT_ERR(__wt_block_manager_named_size(session, WT_LAS_FILE, &las_size));
    WT_STAT_CONN_SET(session, cache_lookaside_ondisk, las_size);
    max_las_size = ((WT_CURSOR_BTREE *)cursor)->btree->file_max;
    if (max_las_size != 0 && (uint64_t)las_size > max_las_size)
        WT_PANIC_MSG(session, WT_PANIC, "WiredTigerLAS: file size of %" PRIu64
                                        " exceeds maximum "
                                        "size %" PRIu64,
          (uint64_t)las_size, max_las_size);

err:
    /* Resolve the transaction. */
    if (local_txn) {
        if (ret == 0)
            ret = __wt_txn_commit(session, NULL);
        else
            WT_TRET(__wt_txn_rollback(session, NULL));
        F_CLR(cursor, WT_CURSTD_UPDATE_LOCAL);

        /* Adjust the entry count. */
        if (ret == 0) {
            (void)__wt_atomic_add64(&conn->cache->las_insert_count, insert_cnt);
            WT_STAT_CONN_INCRV(
              session, txn_prepared_updates_lookaside_inserts, prepared_insert_cnt);
        }
    }

    __las_restore_isolation(session, saved_isolation);

    if (ret == 0 && insert_cnt > 0) {
        multi->page_las.las_pageid = las_pageid;
        multi->page_las.has_prepares = prepared_insert_cnt > 0;
        __las_insert_block_verbose(session, btree, multi);
    }

    WT_UNUSED(first_upd);
    return (ret);
}

/*
 * __wt_las_cursor_position --
 *     Position a lookaside cursor at the beginning of a block. There may be no block of lookaside
 *     entries if they have been removed by WT_CONNECTION::rollback_to_stable.
 */
int
__wt_las_cursor_position(WT_CURSOR *cursor, uint64_t pageid)
{
    WT_ITEM las_key;
    uint64_t las_counter, las_pageid;
    uint32_t las_id;
    int exact;

    /*
     * When scanning for all pages, start at the beginning of the lookaside table.
     */
    if (pageid == 0) {
        WT_RET(cursor->reset(cursor));
        return (cursor->next(cursor));
    }

    /*
     * Because of the special visibility rules for lookaside, a new block can appear in between our
     * search and the block of interest. Keep trying until we find it.
     */
    for (;;) {
        WT_CLEAR(las_key);
        cursor->set_key(cursor, pageid, (uint32_t)0, (uint64_t)0, &las_key);
        WT_RET(cursor->search_near(cursor, &exact));
        if (exact < 0)
            WT_RET(cursor->next(cursor));

        /*
         * Because of the special visibility rules for lookaside, a new
         * block can appear in between our search and the block of
         * interest. Keep trying while we have a key lower than we
         * expect.
         *
         * There may be no block of lookaside entries if they have been
         * removed by WT_CONNECTION::rollback_to_stable.
         */
        WT_RET(cursor->get_key(cursor, &las_pageid, &las_id, &las_counter, &las_key));
        if (las_pageid >= pageid)
            return (0);
    }

    /* NOTREACHED */
}

/*
 * __wt_las_remove_block --
 *     Remove all records for a given page from the lookaside table.
 */
int
__wt_las_remove_block(WT_SESSION_IMPL *session, uint64_t pageid)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    uint64_t remove_cnt;
    uint32_t session_flags;

    conn = S2C(session);
    session_flags = 0; /* [-Wconditional-uninitialized] */

    /*
     * This is an external API for removing records from the lookaside table, first acquiring a
     * lookaside table cursor and enclosing transaction, then calling an underlying function to do
     * the work.
     */
    __wt_las_cursor(session, &cursor, &session_flags);

    if ((ret = __las_remove_block(cursor, pageid, true, &remove_cnt)) == 0)
        (void)__wt_atomic_add64(&conn->cache->las_remove_count, remove_cnt);

    WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));
    return (ret);
}

/*
 * __wt_las_remove_dropped --
 *     Remove an opened btree ID if it is in the dropped table.
 */
void
__wt_las_remove_dropped(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    u_int i, j;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    __wt_spin_lock(session, &cache->las_sweep_lock);
    for (i = 0; i < cache->las_dropped_next && cache->las_dropped[i] != btree->id; i++)
        ;

    if (i < cache->las_dropped_next) {
        cache->las_dropped_next--;
        for (j = i; j < cache->las_dropped_next; j++)
            cache->las_dropped[j] = cache->las_dropped[j + 1];
    }
    __wt_spin_unlock(session, &cache->las_sweep_lock);
}

/*
 * __wt_las_save_dropped --
 *     Save a dropped btree ID to be swept from the lookaside table.
 */
int
__wt_las_save_dropped(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CACHE *cache;
    WT_DECL_RET;

    btree = S2BT(session);
    cache = S2C(session)->cache;

    __wt_spin_lock(session, &cache->las_sweep_lock);
    WT_ERR(__wt_realloc_def(
      session, &cache->las_dropped_alloc, cache->las_dropped_next + 1, &cache->las_dropped));
    cache->las_dropped[cache->las_dropped_next++] = btree->id;
err:
    __wt_spin_unlock(session, &cache->las_sweep_lock);
    return (ret);
}

/*
 * __las_sweep_count --
 *     Calculate how many records to examine per sweep step.
 */
static inline uint64_t
__las_sweep_count(WT_CACHE *cache)
{
    uint64_t las_entry_count;

    /*
     * The sweep server is a slow moving thread. Try to review the entire
     * lookaside table once every 5 minutes.
     *
     * The reason is because the lookaside table exists because we're seeing
     * cache/eviction pressure (it allows us to trade performance and disk
     * space for cache space), and it's likely lookaside blocks are being
     * evicted, and reading them back in doesn't help things. A trickier,
     * but possibly better, alternative might be to review all lookaside
     * blocks in the cache in order to get rid of them, and slowly review
     * lookaside blocks that have already been evicted.
     *
     * Put upper and lower bounds on the calculation: since reads of pages
     * with lookaside entries are blocked during sweep, make sure we do
     * some work but don't block reads for too long.
     */
    las_entry_count = __las_entry_count(cache);
    return (
      (uint64_t)WT_MAX(WT_LAS_SWEEP_ENTRIES, las_entry_count / (5 * WT_MINUTE / WT_LAS_SWEEP_SEC)));
}

/*
 * __las_sweep_init --
 *     Prepare to start a lookaside sweep.
 */
static int
__las_sweep_init(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_DECL_RET;
    u_int i;

    cache = S2C(session)->cache;

    __wt_spin_lock(session, &cache->las_sweep_lock);

    /*
     * If no files have been dropped and the lookaside file is empty, there's nothing to do.
     */
    if (cache->las_dropped_next == 0 && __wt_las_empty(session))
        WT_ERR(WT_NOTFOUND);

    /*
     * Record the current page ID: sweep will stop after this point.
     *
     * Since the btree IDs we're scanning are closed, any eviction must
     * have already completed, so we won't miss anything with this
     * approach.
     *
     * Also, if a tree is reopened and there is lookaside activity before
     * this sweep completes, it will have a higher page ID and should not
     * be removed.
     */
    cache->las_sweep_max_pageid = cache->las_pageid;

    /* Scan the btree IDs to find min/max. */
    cache->las_sweep_dropmin = UINT32_MAX;
    cache->las_sweep_dropmax = 0;
    for (i = 0; i < cache->las_dropped_next; i++) {
        cache->las_sweep_dropmin = WT_MIN(cache->las_sweep_dropmin, cache->las_dropped[i]);
        cache->las_sweep_dropmax = WT_MAX(cache->las_sweep_dropmax, cache->las_dropped[i]);
    }

    /* Initialize the bitmap. */
    __wt_free(session, cache->las_sweep_dropmap);
    WT_ERR(__bit_alloc(
      session, 1 + cache->las_sweep_dropmax - cache->las_sweep_dropmin, &cache->las_sweep_dropmap));
    for (i = 0; i < cache->las_dropped_next; i++)
        __bit_set(cache->las_sweep_dropmap, cache->las_dropped[i] - cache->las_sweep_dropmin);

    /* Clear the list of btree IDs. */
    cache->las_dropped_next = 0;

err:
    __wt_spin_unlock(session, &cache->las_sweep_lock);
    return (ret);
}

/*
 * __wt_las_sweep --
 *     Sweep the lookaside table.
 */
int
__wt_las_sweep(WT_SESSION_IMPL *session)
{
    WT_CACHE *cache;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(saved_key);
    WT_DECL_RET;
    WT_ITEM las_key, las_value;
    WT_ITEM *sweep_key;
    wt_timestamp_t las_timestamp;
    uint64_t cnt, remove_cnt, las_pageid, saved_pageid, visit_cnt;
    uint64_t las_counter, las_txnid;
    uint32_t las_id, session_flags;
    uint8_t prepare_state, upd_type;
    int notused;
    bool local_txn, locked, removing_key_block;

    cache = S2C(session)->cache;
    cursor = NULL;
    sweep_key = &cache->las_sweep_key;
    remove_cnt = 0;
    session_flags = 0; /* [-Werror=maybe-uninitialized] */
    local_txn = locked = removing_key_block = false;

    WT_RET(__wt_scr_alloc(session, 0, &saved_key));
    saved_pageid = 0;

    /*
     * Prevent other threads removing entries from underneath the sweep.
     */
    __wt_writelock(session, &cache->las_sweepwalk_lock);
    locked = true;

    /*
     * Allocate a cursor and wrap all the updates in a transaction. We should have our own lookaside
     * cursor.
     */
    __wt_las_cursor(session, &cursor, &session_flags);
    WT_ASSERT(session, cursor->session == &session->iface);
    WT_ERR(__wt_txn_begin(session, NULL));
    local_txn = true;

    /* Encourage a race */
    __wt_timing_stress(session, WT_TIMING_STRESS_LOOKASIDE_SWEEP);

    /*
     * When continuing a sweep, position the cursor using the key from the
     * last call (we don't care if we're before or after the key, either
     * side is fine).
     *
     * Otherwise, we're starting a new sweep, gather the list of trees to
     * sweep.
     */
    if (sweep_key->size != 0) {
        __wt_cursor_set_raw_key(cursor, sweep_key);
        ret = cursor->search_near(cursor, &notused);

        /*
         * Don't search for the same key twice; if we don't set a new key below, it's because we've
         * reached the end of the table and we want the next pass to start at the beginning of the
         * table. Searching for the same key could leave us stuck at the end of the table,
         * repeatedly checking the same rows.
         */
        __wt_buf_free(session, sweep_key);
    } else
        ret = __las_sweep_init(session);
    if (ret != 0)
        goto srch_notfound;

    cnt = __las_sweep_count(cache);
    visit_cnt = 0;

    /* Walk the file. */
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &las_pageid, &las_id, &las_counter, &las_key));

        __wt_verbose(session, WT_VERB_LOOKASIDE_ACTIVITY,
          "Sweep reviewing lookaside entry with lookaside "
          "page ID %" PRIu64 " btree ID %" PRIu32 " saved key size: %" WT_SIZET_FMT,
          las_pageid, las_id, saved_key->size);

        /*
         * Signal to stop if the cache is stuck: we are ignoring the cache size while scanning the
         * lookaside table, so we're making things worse.
         */
        if (__wt_cache_stuck(session))
            cnt = 0;

        /*
         * Don't go past the end of lookaside from when sweep started. If a file is reopened, its ID
         * may be reused past this point so the bitmap we're using is not valid.
         */
        if (las_pageid > cache->las_sweep_max_pageid) {
            __wt_buf_free(session, sweep_key);
            ret = WT_NOTFOUND;
            break;
        }

        /*
         * We only want to break between key blocks. Stop if we've processed enough entries either
         * all we wanted or enough and there is a reader waiting and we're on a key boundary.
         */
        ++visit_cnt;
        if (!removing_key_block &&
          (cnt == 0 || (visit_cnt > WT_LAS_SWEEP_ENTRIES && cache->las_reader)))
            break;
        if (cnt > 0)
            --cnt;

        /*
         * If the entry belongs to a dropped tree, discard it.
         *
         * Cursor opened overwrite=true: won't return WT_NOTFOUND
         * should another thread remove the record before we do (not
         * expected for dropped trees), and the cursor remains
         * positioned in that case.
         */
        if (las_id >= cache->las_sweep_dropmin && las_id <= cache->las_sweep_dropmax &&
          __bit_test(cache->las_sweep_dropmap, las_id - cache->las_sweep_dropmin)) {
            WT_ERR(cursor->remove(cursor));
            ++remove_cnt;
            saved_key->size = 0;
            /*
             * Allow sweep to break while removing entries from a dead file.
             */
            removing_key_block = false;
            continue;
        }

        /*
         * Remove all entries for a key once they have aged out and are no longer needed.
         */
        WT_ERR(cursor->get_value(
          cursor, &las_txnid, &las_timestamp, &prepare_state, &upd_type, &las_value));

        /*
         * Check to see if the page or key has changed this iteration,
         * and if they have, setup context for safely removing obsolete
         * updates.
         *
         * It's important to check for page boundaries explicitly
         * because it is possible for the same key to be at the start
         * of the next block. See WT-3982 for details.
         */
        if (las_pageid != saved_pageid || saved_key->size != las_key.size ||
          memcmp(saved_key->data, las_key.data, las_key.size) != 0) {
            /* If we've examined enough entries, give up. */
            if (cnt == 0)
                break;

            saved_pageid = las_pageid;
            WT_ERR(__wt_buf_set(session, saved_key, las_key.data, las_key.size));

            /*
             * There are several conditions that need to be met
             * before we choose to remove a key block:
             *  * The entries were written with skew newest.
             *    Indicated by the first entry being a birthmark.
             *  * The first entry is globally visible.
             *  * The entry wasn't from a prepared transaction.
             */
            if (upd_type == WT_UPDATE_BIRTHMARK &&
              __wt_txn_visible_all(session, las_txnid, las_timestamp) &&
              prepare_state != WT_PREPARE_INPROGRESS)
                removing_key_block = true;
            else
                removing_key_block = false;
        }

        if (!removing_key_block)
            continue;

        __wt_verbose(session, WT_VERB_LOOKASIDE_ACTIVITY,
          "Sweep removing lookaside entry with "
          "page ID: %" PRIu64 " btree ID: %" PRIu32 " saved key size: %" WT_SIZET_FMT
          ", record type: %" PRIu8 " transaction ID: %" PRIu64,
          las_pageid, las_id, saved_key->size, upd_type, las_txnid);
        WT_ERR(cursor->remove(cursor));
        ++remove_cnt;
    }

    /*
     * If the loop terminates after completing a work unit, we will continue the table sweep next
     * time. Get a local copy of the sweep key, we're going to reset the cursor; do so before
     * calling cursor.remove, cursor.remove can discard our hazard pointer and the page could be
     * evicted from underneath us.
     */
    if (ret == 0) {
        WT_ERR(__wt_cursor_get_raw_key(cursor, sweep_key));
        if (!WT_DATA_IN_ITEM(sweep_key))
            WT_ERR(__wt_buf_set(session, sweep_key, sweep_key->data, sweep_key->size));
    }

srch_notfound:
    WT_ERR_NOTFOUND_OK(ret);

    if (0) {
err:
        __wt_buf_free(session, sweep_key);
    }
    if (local_txn) {
        if (ret == 0)
            ret = __wt_txn_commit(session, NULL);
        else
            WT_TRET(__wt_txn_rollback(session, NULL));
        if (ret == 0)
            (void)__wt_atomic_add64(&cache->las_remove_count, remove_cnt);
    }

    WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));

    if (locked)
        __wt_writeunlock(session, &cache->las_sweepwalk_lock);

    __wt_scr_free(session, &saved_key);

    return (ret);
}
