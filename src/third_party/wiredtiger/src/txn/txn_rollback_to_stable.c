/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __txn_rollback_to_stable_lookaside_fixup --
 *     Remove any updates that need to be rolled back from the lookaside file.
 */
static int
__txn_rollback_to_stable_lookaside_fixup(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_ITEM las_key, las_value;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t las_timestamp, rollback_timestamp;
    uint64_t las_counter, las_pageid, las_total, las_txnid;
    uint32_t las_id, session_flags;
    uint8_t prepare_state, upd_type;

    conn = S2C(session);
    cursor = NULL;
    las_total = 0;
    session_flags = 0; /* [-Werror=maybe-uninitialized] */

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    txn_global = &conn->txn_global;
    WT_ORDERED_READ(rollback_timestamp, txn_global->stable_timestamp);

    __wt_las_cursor(session, &cursor, &session_flags);

    /* Discard pages we read as soon as we're done with them. */
    F_SET(session, WT_SESSION_READ_WONT_NEED);

    /* Walk the file. */
    __wt_writelock(session, &conn->cache->las_sweepwalk_lock);
    while ((ret = cursor->next(cursor)) == 0) {
        ++las_total;
        WT_ERR(cursor->get_key(cursor, &las_pageid, &las_id, &las_counter, &las_key));

        /* Check the file ID so we can skip durable tables */
        if (las_id >= conn->stable_rollback_maxfile)
            WT_PANIC_RET(session, EINVAL,
              "file ID %" PRIu32 " in lookaside table larger than max %" PRIu32, las_id,
              conn->stable_rollback_maxfile);
        if (__bit_test(conn->stable_rollback_bitstring, las_id))
            continue;

        WT_ERR(cursor->get_value(
          cursor, &las_txnid, &las_timestamp, &prepare_state, &upd_type, &las_value));

        /*
         * Entries with no timestamp will have a timestamp of zero, which will fail the following
         * check and cause them to never be removed.
         */
        if (rollback_timestamp < las_timestamp) {
            WT_ERR(cursor->remove(cursor));
            WT_STAT_CONN_INCR(session, txn_rollback_las_removed);
            --las_total;
        }
    }
    WT_ERR_NOTFOUND_OK(ret);
err:
    if (ret == 0) {
        conn->cache->las_insert_count = las_total;
        conn->cache->las_remove_count = 0;
    }
    __wt_writeunlock(session, &conn->cache->las_sweepwalk_lock);
    WT_TRET(__wt_las_cursor_close(session, &cursor, session_flags));

    F_CLR(session, WT_SESSION_READ_WONT_NEED);

    return (ret);
}

/*
 * __txn_abort_newer_update --
 *     Abort updates in an update change with timestamps newer than the rollback timestamp.
 */
static void
__txn_abort_newer_update(
  WT_SESSION_IMPL *session, WT_UPDATE *first_upd, wt_timestamp_t rollback_timestamp)
{
    WT_UPDATE *upd;

    for (upd = first_upd; upd != NULL; upd = upd->next) {
        /*
         * Updates with no timestamp will have a timestamp of zero and will never be rolled back. If
         * the table is configured for strict timestamp checking, assert that all more recent
         * updates were also rolled back.
         */
        if (upd->txnid == WT_TXN_ABORTED || upd->timestamp == 0) {
            if (upd == first_upd)
                first_upd = upd->next;
        } else if (rollback_timestamp < upd->timestamp) {
            /*
             * If any updates are aborted, all newer updates
             * better be aborted as well.
             *
             * Timestamp ordering relies on the validations at
             * the time of commit. Thus if the table is not
             * configured for key consistency check, the
             * the timestamps could be out of order here.
             */
            WT_ASSERT(session, !FLD_ISSET(S2BT(session)->assert_flags, WT_ASSERT_COMMIT_TS_KEYS) ||
                upd == first_upd);
            first_upd = upd->next;

            upd->txnid = WT_TXN_ABORTED;
            WT_STAT_CONN_INCR(session, txn_rollback_upd_aborted);
            upd->timestamp = 0;
        }
    }
}

/*
 * __txn_abort_newer_insert --
 *     Apply the update abort check to each entry in an insert skip list
 */
static void
__txn_abort_newer_insert(
  WT_SESSION_IMPL *session, WT_INSERT_HEAD *head, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT *ins;

    WT_SKIP_FOREACH (ins, head)
        __txn_abort_newer_update(session, ins->upd, rollback_timestamp);
}

/*
 * __txn_abort_newer_col_var --
 *     Abort updates on a variable length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static void
__txn_abort_newer_col_var(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_COL *cip;
    WT_INSERT_HEAD *ins;
    uint32_t i;

    /* Review the changes to the original on-page data items */
    WT_COL_FOREACH (page, cip, i)
        if ((ins = WT_COL_UPDATE(page, cip)) != NULL)
            __txn_abort_newer_insert(session, ins, rollback_timestamp);

    /* Review the append list */
    if ((ins = WT_COL_APPEND(page)) != NULL)
        __txn_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __txn_abort_newer_col_fix --
 *     Abort updates on a fixed length col leaf page with timestamps newer than the rollback
 *     timestamp.
 */
static void
__txn_abort_newer_col_fix(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT_HEAD *ins;

    /* Review the changes to the original on-page data items */
    if ((ins = WT_COL_UPDATE_SINGLE(page)) != NULL)
        __txn_abort_newer_insert(session, ins, rollback_timestamp);

    /* Review the append list */
    if ((ins = WT_COL_APPEND(page)) != NULL)
        __txn_abort_newer_insert(session, ins, rollback_timestamp);
}

/*
 * __txn_abort_newer_row_leaf --
 *     Abort updates on a row leaf page with timestamps newer than the rollback timestamp.
 */
static void
__txn_abort_newer_row_leaf(
  WT_SESSION_IMPL *session, WT_PAGE *page, wt_timestamp_t rollback_timestamp)
{
    WT_INSERT_HEAD *insert;
    WT_ROW *rip;
    WT_UPDATE *upd;
    uint32_t i;

    /*
     * Review the insert list for keys before the first entry on the disk page.
     */
    if ((insert = WT_ROW_INSERT_SMALLEST(page)) != NULL)
        __txn_abort_newer_insert(session, insert, rollback_timestamp);

    /*
     * Review updates that belong to keys that are on the disk image, as well as for keys inserted
     * since the page was read from disk.
     */
    WT_ROW_FOREACH (page, rip, i) {
        if ((upd = WT_ROW_UPDATE(page, rip)) != NULL)
            __txn_abort_newer_update(session, upd, rollback_timestamp);

        if ((insert = WT_ROW_INSERT(page, rip)) != NULL)
            __txn_abort_newer_insert(session, insert, rollback_timestamp);
    }
}

/*
 * __txn_abort_newer_updates --
 *     Abort updates on this page newer than the timestamp.
 */
static int
__txn_abort_newer_updates(WT_SESSION_IMPL *session, WT_REF *ref, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_PAGE *page;
    WT_PAGE_LOOKASIDE *page_las;
    uint32_t read_flags;
    bool local_read;

    /*
     * If we created a page image with updates that need to be rolled back,
     * read the history into cache now and make sure the page is marked
     * dirty.  Otherwise, the history we need could be swept from the
     * lookaside table before the page is read because the lookaside sweep
     * code has no way to tell that the page image is invalid.
     *
     * So, if there is lookaside history for a page, first check if the
     * history needs to be rolled back then ensure the history is loaded
     * into cache.
     *
     * Also, we have separately discarded any lookaside history more recent
     * than the rollback timestamp.  For page_las structures in cache,
     * reset any future timestamps back to the rollback timestamp.  This
     * allows those structures to be discarded once the rollback timestamp
     * is stable (crucially for tests, they can be discarded if the
     * connection is closed right after a rollback_to_stable call).
     */
    local_read = false;
    read_flags = WT_READ_WONT_NEED;
    if ((page_las = ref->page_las) != NULL) {
        if (rollback_timestamp < page_las->max_ondisk_ts) {
            /*
             * Make sure we get back a page with history, not a limbo page.
             */
            WT_ASSERT(session, !F_ISSET(&session->txn, WT_TXN_HAS_SNAPSHOT));
            WT_RET(__wt_page_in(session, ref, read_flags));
            WT_ASSERT(session,
              ref->state != WT_REF_LIMBO && ref->page != NULL && __wt_page_is_modified(ref->page));
            local_read = true;
            page_las->max_ondisk_ts = rollback_timestamp;
        }
        if (rollback_timestamp < page_las->min_skipped_ts)
            page_las->min_skipped_ts = rollback_timestamp;
    }

    /* Review deleted page saved to the ref */
    if (ref->page_del != NULL && rollback_timestamp < ref->page_del->timestamp)
        WT_ERR(__wt_delete_page_rollback(session, ref));

    /*
     * If we have a ref with no page, or the page is clean, there is
     * nothing to roll back.
     *
     * This check for a clean page is partly an optimization (checkpoint
     * only marks pages clean when they have no unwritten updates so
     * there's no point visiting them again), but also covers a corner case
     * of a checkpoint with use_timestamp=false.  Such a checkpoint
     * effectively moves the stable timestamp forward, because changes that
     * are written in the checkpoint cannot be reliably rolled back.  The
     * actual stable timestamp doesn't change, though, so if we try to roll
     * back clean pages the in-memory tree can get out of sync with the
     * on-disk tree.
     */
    if ((page = ref->page) == NULL || !__wt_page_is_modified(page))
        goto err;

    switch (page->type) {
    case WT_PAGE_COL_FIX:
        __txn_abort_newer_col_fix(session, page, rollback_timestamp);
        break;
    case WT_PAGE_COL_VAR:
        __txn_abort_newer_col_var(session, page, rollback_timestamp);
        break;
    case WT_PAGE_COL_INT:
    case WT_PAGE_ROW_INT:
        /*
         * There is nothing to do for internal pages, since we aren't rolling back far enough to
         * potentially include reconciled changes - and thus won't need to roll back structure
         * changes on internal pages.
         */
        break;
    case WT_PAGE_ROW_LEAF:
        __txn_abort_newer_row_leaf(session, page, rollback_timestamp);
        break;
    default:
        WT_ERR(__wt_illegal_value(session, page->type));
    }

err:
    if (local_read)
        WT_TRET(__wt_page_release(session, ref, read_flags));
    return (ret);
}

/*
 * __txn_rollback_to_stable_btree_walk --
 *     Called for each open handle - choose to either skip or wipe the commits
 */
static int
__txn_rollback_to_stable_btree_walk(WT_SESSION_IMPL *session, wt_timestamp_t rollback_timestamp)
{
    WT_DECL_RET;
    WT_REF *child_ref, *ref;

    /* Walk the tree, marking commits aborted where appropriate. */
    ref = NULL;
    while ((ret = __wt_tree_walk(
              session, &ref, WT_READ_CACHE | WT_READ_NO_EVICT | WT_READ_WONT_NEED)) == 0 &&
      ref != NULL) {
        if (WT_PAGE_IS_INTERNAL(ref->page)) {
            WT_INTL_FOREACH_BEGIN (session, ref->page, child_ref) {
                WT_RET(__txn_abort_newer_updates(session, child_ref, rollback_timestamp));
            }
            WT_INTL_FOREACH_END;
        } else
            WT_RET(__txn_abort_newer_updates(session, ref, rollback_timestamp));
    }
    return (ret);
}

/*
 * __txn_rollback_eviction_drain --
 *     Wait for eviction to drain from a tree.
 */
static int
__txn_rollback_eviction_drain(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_UNUSED(cfg);

    WT_RET(__wt_evict_file_exclusive_on(session));
    __wt_evict_file_exclusive_off(session);
    return (0);
}

/*
 * __txn_rollback_to_stable_btree --
 *     Called for each open handle - choose to either skip or wipe the commits
 */
static int
__txn_rollback_to_stable_btree(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    wt_timestamp_t rollback_timestamp;

    WT_UNUSED(cfg);

    btree = S2BT(session);
    conn = S2C(session);
    txn_global = &conn->txn_global;

    /*
     * Immediately durable files don't get their commits wiped. This case mostly exists to support
     * the semantic required for the oplog in MongoDB - updates that have been made to the oplog
     * should not be aborted. It also wouldn't be safe to roll back updates for any table that had
     * it's records logged, since those updates would be recovered after a crash making them
     * inconsistent.
     */
    if (__wt_btree_immediately_durable(session)) {
        /*
         * Add the btree ID to the bitstring, so we can exclude any lookaside entries for this
         * btree.
         */
        if (btree->id >= conn->stable_rollback_maxfile)
            WT_PANIC_RET(session, EINVAL, "btree file ID %" PRIu32 " larger than max %" PRIu32,
              btree->id, conn->stable_rollback_maxfile);
        __bit_set(conn->stable_rollback_bitstring, btree->id);
        return (0);
    }

    /* There is never anything to do for checkpoint handles */
    if (session->dhandle->checkpoint != NULL)
        return (0);

    /* There is nothing to do on an empty tree. */
    if (btree->root.page == NULL)
        return (0);

    /*
     * Copy the stable timestamp, otherwise we'd need to lock it each time it's accessed. Even
     * though the stable timestamp isn't supposed to be updated while rolling back, accessing it
     * without a lock would violate protocol.
     */
    WT_ORDERED_READ(rollback_timestamp, txn_global->stable_timestamp);

    /*
     * Ensure the eviction server is out of the file - we don't want it messing with us. This step
     * shouldn't be required, but it simplifies some of the reasoning about what state trees can be
     * in.
     */
    WT_RET(__wt_evict_file_exclusive_on(session));
    WT_WITH_PAGE_INDEX(
      session, ret = __txn_rollback_to_stable_btree_walk(session, rollback_timestamp));
    __wt_evict_file_exclusive_off(session);

    return (ret);
}

/*
 * __txn_rollback_to_stable_check --
 *     Ensure the rollback request is reasonable.
 */
static int
__txn_rollback_to_stable_check(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_TXN_GLOBAL *txn_global;
    bool txn_active;

    conn = S2C(session);
    txn_global = &conn->txn_global;

    if (!txn_global->has_stable_timestamp)
        WT_RET_MSG(session, EINVAL, "rollback_to_stable requires a stable timestamp");

    /*
     * Help the user comply with the requirement that there are no concurrent operations. Protect
     * against spurious conflicts with the sweep server: we exclude it from running concurrent with
     * rolling back the lookaside contents.
     */
    __wt_writelock(session, &conn->cache->las_sweepwalk_lock);
    ret = __wt_txn_activity_check(session, &txn_active);
    __wt_writeunlock(session, &conn->cache->las_sweepwalk_lock);

    if (ret == 0 && txn_active)
        WT_RET_MSG(session, EINVAL, "rollback_to_stable illegal with active transactions");

    return (ret);
}

/*
 * __txn_rollback_to_stable --
 *     Rollback all in-memory state related to timestamps more recent than the passed in timestamp.
 */
static int
__txn_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);

    WT_STAT_CONN_INCR(session, txn_rollback_to_stable);
    /*
     * Mark that a rollback operation is in progress and wait for eviction
     * to drain.  This is necessary because lookaside eviction uses
     * transactions and causes the check for a quiescent system to fail.
     *
     * Configuring lookaside eviction off isn't atomic, safe because the
     * flag is only otherwise set when closing down the database. Assert
     * to avoid confusion in the future.
     */
    WT_ASSERT(session, !F_ISSET(conn, WT_CONN_EVICTION_NO_LOOKASIDE));
    F_SET(conn, WT_CONN_EVICTION_NO_LOOKASIDE);

    WT_ERR(__wt_conn_btree_apply(session, NULL, __txn_rollback_eviction_drain, NULL, cfg));

    WT_ERR(__txn_rollback_to_stable_check(session));

    F_CLR(conn, WT_CONN_EVICTION_NO_LOOKASIDE);

    /*
     * Allocate a non-durable btree bitstring. We increment the global value before using it, so the
     * current value is already in use, and hence we need to add one here.
     */
    conn->stable_rollback_maxfile = conn->next_file_id + 1;
    WT_ERR(__bit_alloc(session, conn->stable_rollback_maxfile, &conn->stable_rollback_bitstring));
    WT_ERR(__wt_conn_btree_apply(session, NULL, __txn_rollback_to_stable_btree, NULL, cfg));

    /*
     * Clear any offending content from the lookaside file. This must be done after the in-memory
     * application, since the process of walking trees in cache populates a list that is used to
     * check which lookaside records should be removed.
     */
    if (!F_ISSET(conn, WT_CONN_IN_MEMORY))
        WT_ERR(__txn_rollback_to_stable_lookaside_fixup(session));

err:
    F_CLR(conn, WT_CONN_EVICTION_NO_LOOKASIDE);
    __wt_free(session, conn->stable_rollback_bitstring);
    return (ret);
}

/*
 * __wt_txn_rollback_to_stable --
 *     Rollback all in-memory state related to timestamps more recent than the passed in timestamp.
 */
int
__wt_txn_rollback_to_stable(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_DECL_RET;

    /*
     * Don't use the connection's default session: we are working on data handles and (a) don't want
     * to cache all of them forever, plus (b) can't guarantee that no other method will be called
     * concurrently.
     */
    WT_RET(__wt_open_internal_session(S2C(session), "txn rollback_to_stable", true, 0, &session));
    ret = __txn_rollback_to_stable(session, cfg);
    WT_TRET(session->iface.close(&session->iface, NULL));

    return (ret);
}
