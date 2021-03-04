/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __hs_delete_key_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  uint32_t btree_id, const WT_ITEM *key, bool reinsert);
static int __hs_fixup_out_of_order_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  WT_BTREE *btree, const WT_ITEM *key, wt_timestamp_t ts, uint64_t *hs_counter);

/*
 * __hs_verbose_cache_stats --
 *     Display a verbose message once per checkpoint with details about the cache state when
 *     performing a history store table write.
 */
static void
__hs_verbose_cache_stats(WT_SESSION_IMPL *session, WT_BTREE *btree)
{
    WT_CACHE *cache;
    WT_CONNECTION_IMPL *conn;
    double pct_dirty, pct_full;
    uint64_t ckpt_gen_current, ckpt_gen_last;
    uint32_t btree_id;

    btree_id = btree->id;

    if (!WT_VERBOSE_ISSET(session, WT_VERB_HS | WT_VERB_HS_ACTIVITY))
        return;

    conn = S2C(session);
    cache = conn->cache;
    ckpt_gen_current = __wt_gen(session, WT_GEN_CHECKPOINT);
    ckpt_gen_last = cache->hs_verb_gen_write;

    /*
     * Print a message if verbose history store, or once per checkpoint if only reporting activity.
     * Avoid an expensive atomic operation as often as possible when the message rate is limited.
     */
    if (WT_VERBOSE_ISSET(session, WT_VERB_HS) ||
      (ckpt_gen_current > ckpt_gen_last &&
        __wt_atomic_casv64(&cache->hs_verb_gen_write, ckpt_gen_last, ckpt_gen_current))) {
        WT_IGNORE_RET_BOOL(__wt_eviction_clean_needed(session, &pct_full));
        WT_IGNORE_RET_BOOL(__wt_eviction_dirty_needed(session, &pct_dirty));

        __wt_verbose(session, WT_VERB_HS | WT_VERB_HS_ACTIVITY,
          "Page reconciliation triggered history store write: file ID %" PRIu32
          ". Current history store file size: %" PRId64
          ", cache dirty: %2.3f%% , cache use: %2.3f%%",
          btree_id, WT_STAT_READ(conn->stats, cache_hs_ondisk), pct_dirty, pct_full);
    }

    /* Never skip updating the tracked generation */
    if (WT_VERBOSE_ISSET(session, WT_VERB_HS))
        cache->hs_verb_gen_write = ckpt_gen_current;
}

/*
 * __hs_insert_record --
 *     A helper function to insert the record into the history store including stop time point.
 */
static int
__hs_insert_record(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_BTREE *btree, const WT_ITEM *key,
  const uint8_t type, const WT_ITEM *hs_value, WT_TIME_WINDOW *tw)
{
#ifdef HAVE_DIAGNOSTIC
    WT_CURSOR_BTREE *hs_cbt;
#endif
    WT_DECL_ITEM(hs_key);
#ifdef HAVE_DIAGNOSTIC
    WT_DECL_ITEM(existing_val);
#endif
    WT_DECL_RET;
    wt_timestamp_t hs_start_ts;
#ifdef HAVE_DIAGNOSTIC
    wt_timestamp_t durable_timestamp_diag;
    wt_timestamp_t hs_stop_durable_ts_diag;
    uint64_t upd_type_full_diag;
    int cmp;
#endif
    uint64_t counter, hs_counter;
    uint32_t hs_btree_id;

    counter = 0;

    /* Allocate buffers for the history store and search key. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));

#ifdef HAVE_DIAGNOSTIC
    /* Allocate buffer for the existing history store value for the same key. */
    WT_ERR(__wt_scr_alloc(session, 0, &existing_val));
    hs_cbt = __wt_curhs_get_cbt(cursor);
#endif

    /* Sanity check that the btree is not a history store btree. */
    WT_ASSERT(session, !WT_IS_HS(btree));

    /*
     * Only deltas or full updates should be written to the history store. More specifically, we
     * should NOT be writing tombstone records in the history store table.
     */
    WT_ASSERT(session, type == WT_UPDATE_STANDARD || type == WT_UPDATE_MODIFY);

    /*
     * Adjust counter if there exists an update in the history store with same btree id, key and
     * timestamp. Otherwise the newly inserting history store record may fall behind the existing
     * one can lead to wrong order.
     */
    cursor->set_key(cursor, 4, btree->id, key, tw->start_ts, UINT64_MAX);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_before(session, cursor), true);

    if (ret == 0) {
        WT_ERR(cursor->get_key(cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

#ifdef HAVE_DIAGNOSTIC
        if (tw->start_ts == hs_start_ts) {
            WT_ERR(cursor->get_value(cursor, &hs_stop_durable_ts_diag, &durable_timestamp_diag,
              &upd_type_full_diag, existing_val));
            WT_ERR(__wt_compare(session, NULL, existing_val, hs_value, &cmp));
            /*
             * We shouldn't be inserting the same value again for the key unless coming from a
             * different transaction. If the updates are from the same transaction, the start
             * timestamp for each update should be different.
             */
            if (cmp == 0)
                WT_ASSERT(session,
                  tw->start_txn == WT_TXN_NONE ||
                    tw->start_txn != hs_cbt->upd_value->tw.start_txn ||
                    tw->start_ts != hs_cbt->upd_value->tw.start_ts);
            counter = hs_counter + 1;
        }
#else
        if (tw->start_ts == hs_start_ts)
            counter = hs_counter + 1;
#endif
    }

    /*
     * If we're inserting a non-zero timestamp, look ahead for any higher timestamps. If we find
     * updates, we should remove them and reinsert them at the current timestamp.
     */
    if (tw->start_ts != WT_TS_NONE) {
        /*
         * If there were no keys equal to or less than our target key, we would have received
         * WT_NOTFOUND. In that case we need to search again with a higher timestamp as the cursor
         * would not be positioned correctly.
         */
        if (ret == 0)
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);
        else {
            cursor->set_key(cursor, 3, btree->id, key, tw->start_ts + 1);
            WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, cursor), true);
        }
        if (ret == 0)
            WT_ERR(__hs_fixup_out_of_order_from_pos(
              session, cursor, btree, key, tw->start_ts, &counter));
    }

#ifdef HAVE_DIAGNOSTIC
    /*
     * We may have fixed out of order keys. Make sure that we haven't accidentally added a duplicate
     * of the key we are about to insert.
     */
    if (F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
        WT_ERR(cursor->get_key(cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));
        if (hs_btree_id == btree->id && tw->start_ts == hs_start_ts && hs_counter == counter) {
            WT_ERR(__wt_compare(session, NULL, hs_key, key, &cmp));
            WT_ASSERT(session, cmp != 0);
        }
    }
#endif

    /* Insert the new record now. */
    cursor->set_key(cursor, 4, btree->id, key, tw->start_ts, counter);
    cursor->set_value(
      cursor, tw, tw->durable_stop_ts, tw->durable_start_ts, (uint64_t)type, hs_value);
    WT_ERR(cursor->insert(cursor));
    WT_STAT_CONN_INCR(session, cache_hs_insert);
    WT_STAT_DATA_INCR(session, cache_hs_insert);

err:
#ifdef HAVE_DIAGNOSTIC
    __wt_scr_free(session, &existing_val);
#endif
    __wt_scr_free(session, &hs_key);
    return (ret);
}

/*
 * __hs_next_upd_full_value --
 *     Get the next update and its full value.
 */
static inline int
__hs_next_upd_full_value(WT_SESSION_IMPL *session, WT_MODIFY_VECTOR *modifies,
  WT_ITEM *older_full_value, WT_ITEM *full_value, WT_UPDATE **updp)
{
    WT_UPDATE *upd;
    *updp = NULL;

    __wt_modify_vector_pop(modifies, &upd);
    if (upd->type == WT_UPDATE_TOMBSTONE) {
        if (modifies->size == 0) {
            WT_ASSERT(session, older_full_value == NULL);
            *updp = upd;
            return (0);
        }

        __wt_modify_vector_pop(modifies, &upd);
        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
        full_value->data = upd->data;
        full_value->size = upd->size;
    } else if (upd->type == WT_UPDATE_MODIFY) {
        WT_RET(__wt_buf_set(session, full_value, older_full_value->data, older_full_value->size));
        WT_RET(__wt_modify_apply_item(session, S2BT(session)->value_format, full_value, upd->data));
    } else {
        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
        full_value->data = upd->data;
        full_value->size = upd->size;
    }

    *updp = upd;
    return (0);
}

/*
 * __wt_hs_insert_updates --
 *     Copy one set of saved updates into the database's history store table.
 */
int
__wt_hs_insert_updates(WT_SESSION_IMPL *session, WT_PAGE *page, WT_MULTI *multi)
{
    WT_BTREE *btree, *hs_btree;
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(full_value);
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(modify_value);
    WT_DECL_ITEM(prev_full_value);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
/* If the limit is exceeded, we will insert a full update to the history store */
#define MAX_REVERSE_MODIFY_NUM 16
    WT_MODIFY entries[MAX_REVERSE_MODIFY_NUM];
    WT_MODIFY_VECTOR modifies;
    WT_SAVE_UPD *list;
    WT_UPDATE *first_globally_visible_upd, *first_non_ts_upd;
    WT_UPDATE *non_aborted_upd, *oldest_upd, *prev_upd, *tombstone, *upd;
    WT_TIME_WINDOW tw;
    wt_off_t hs_size;
    wt_timestamp_t min_insert_ts;
    uint64_t insert_cnt, max_hs_size;
    uint32_t i;
    uint8_t *p;
    int nentries;
    char ts_string[3][WT_TS_INT_STRING_SIZE];
    bool enable_reverse_modify, hs_inserted, squashed, ts_updates_in_hs;

    btree = S2BT(session);
    prev_upd = NULL;
    insert_cnt = 0;
    WT_TIME_WINDOW_INIT(&tw);

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    __wt_modify_vector_init(session, &modifies);

    if (!btree->hs_entries)
        btree->hs_entries = true;

    /* Ensure enough room for a column-store key without checking. */
    WT_ERR(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));

    WT_ERR(__wt_scr_alloc(session, 0, &full_value));

    WT_ERR(__wt_scr_alloc(session, 0, &prev_full_value));

    /* Enter each update in the boundary's list into the history store. */
    for (i = 0, list = multi->supd; i < multi->supd_entries; ++i, ++list) {
        /* If no onpage_upd is selected, we don't need to insert anything into the history store. */
        if (list->onpage_upd == NULL)
            continue;

        /* Skip aborted updates. */
        for (upd = list->onpage_upd->next; upd != NULL && upd->txnid == WT_TXN_ABORTED;
             upd = upd->next)
            ;

        /* No update to insert to history store. */
        if (upd == NULL)
            continue;

        /* Updates have already been inserted to the history store. */
        if (F_ISSET(upd, WT_UPDATE_HS))
            continue;

        /* History store table key component: source key. */
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

        first_globally_visible_upd = first_non_ts_upd = NULL;
        ts_updates_in_hs = false;
        enable_reverse_modify = true;
        min_insert_ts = WT_TS_MAX;

        __wt_modify_vector_clear(&modifies);

        /*
         * The algorithm assumes the oldest update on the update chain in memory is either a full
         * update or a tombstone.
         *
         * This is guaranteed by __wt_rec_upd_select appends the original onpage value at the end of
         * the chain. It also assumes the onpage_upd selected cannot be a TOMBSTONE and the update
         * newer than a TOMBSTONE must be a full update.
         *
         * The algorithm walks from the oldest update, or the most recently inserted into history
         * store update, to the newest update and build full updates along the way. It sets the stop
         * time point of the update to the start time point of the next update, squashes the updates
         * that are from the same transaction and of the same start timestamp, calculates reverse
         * modification if prev_upd is a MODIFY, and inserts the update to the history store.
         *
         * It deals with the following scenarios:
         * 1) We only have full updates on the chain and we only insert full updates to
         * the history store.
         * 2) We have modifies on the chain, e.g., U (selected onpage value) -> M -> M ->U. We
         * reverse the modifies and insert the reversed modifies to the history store if it is not
         * the newest update written to the history store and the reverse operation is successful.
         * With regard to the example, we insert U -> RM -> U to the history store.
         * 3) We have tombstones in the middle of the chain, e.g.,
         * U (selected onpage value) -> U -> T -> M -> U.
         * We write the stop time point of M with the start time point of the tombstone and skip the
         * tombstone.
         * 4) We have a single tombstone on the chain, it is simply ignored.
         */
        for (upd = list->onpage_upd, non_aborted_upd = prev_upd = NULL; upd != NULL;
             prev_upd = non_aborted_upd, upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;

            non_aborted_upd = upd;

            /* If we've seen a smaller timestamp before, use that instead. */
            if (min_insert_ts < upd->start_ts) {
                /*
                 * Resolved prepared updates will lose their durable timestamp here. This is a
                 * wrinkle in our handling of out-of-order updates.
                 */
                if (upd->start_ts != upd->durable_ts) {
                    WT_ASSERT(session, min_insert_ts < upd->durable_ts);
                    WT_STAT_CONN_DATA_INCR(session, cache_hs_order_lose_durable_timestamp);
                }
                __wt_verbose(session, WT_VERB_TIMESTAMP,
                  "fixing out-of-order updates during insertion; start_ts=%s, durable_start_ts=%s, "
                  "min_insert_ts=%s",
                  __wt_timestamp_to_string(upd->start_ts, ts_string[0]),
                  __wt_timestamp_to_string(upd->durable_ts, ts_string[1]),
                  __wt_timestamp_to_string(min_insert_ts, ts_string[2]));
                upd->start_ts = upd->durable_ts = min_insert_ts;
                WT_STAT_CONN_DATA_INCR(session, cache_hs_order_fixup_insert);
            } else if (upd->start_ts != WT_TS_NONE)
                /*
                 * Don't reset to WT_TS_NONE as we don't want to clear the timestamps for updates
                 * older than the update without timestamp.
                 */
                min_insert_ts = upd->start_ts;

            WT_ERR(__wt_modify_vector_push(&modifies, upd));

            /* Track the first update that is globally visible. */
            if (first_globally_visible_upd == NULL && __wt_txn_upd_visible_all(session, upd))
                first_globally_visible_upd = upd;

            /*
             * Always insert full update to the history store if we write a prepared update to the
             * data store.
             */
            if (upd->prepare_state == WT_PREPARE_INPROGRESS)
                enable_reverse_modify = false;

            /* Always insert full update to the history store if we need to squash the updates. */
            if (prev_upd != NULL && prev_upd->txnid == upd->txnid &&
              prev_upd->start_ts == upd->start_ts)
                enable_reverse_modify = false;

            /* Always insert full update to the history store if the timestamps are not in order. */
            if (prev_upd != NULL && prev_upd->start_ts < upd->start_ts)
                enable_reverse_modify = false;

            /* Find the first update without timestamp. */
            if (first_non_ts_upd == NULL && upd->start_ts == WT_TS_NONE)
                first_non_ts_upd = upd;
            else if (first_non_ts_upd != NULL && upd->start_ts != WT_TS_NONE) {
                F_SET(upd, WT_UPDATE_BEHIND_MIXED_MODE);
                if (F_ISSET(upd, WT_UPDATE_HS))
                    ts_updates_in_hs = true;
            }

            /*
             * No need to continue if we see the first self contained value after the first globally
             * visible value.
             */
            if (first_globally_visible_upd != NULL && WT_UPDATE_DATA_VALUE(upd))
                break;

            /*
             * If we've reached a full update and it's in the history store we don't need to
             * continue as anything beyond this point won't help with calculating deltas.
             */
            if (upd->type == WT_UPDATE_STANDARD && F_ISSET(upd, WT_UPDATE_HS))
                break;
        }

        prev_upd = upd = NULL;

        /* Construct the oldest full update. */
        WT_ASSERT(session, modifies.size > 0);

        __wt_modify_vector_peek(&modifies, &oldest_upd);

        WT_ASSERT(session,
          oldest_upd->type == WT_UPDATE_STANDARD || oldest_upd->type == WT_UPDATE_TOMBSTONE);

        /*
         * Clear the history store here if the oldest update is a tombstone and it is the first
         * update without timestamp on the update chain because we don't have the cursor placed at
         * the correct place to delete the history store records when inserting the first update and
         * it may be skipped if there is nothing to insert to the history store.
         */
        if (oldest_upd->type == WT_UPDATE_TOMBSTONE && oldest_upd == first_non_ts_upd &&
          !F_ISSET(first_non_ts_upd, WT_UPDATE_CLEARED_HS)) {
            /* We can only delete history store entries that have timestamps. */
            WT_ERR(__wt_hs_delete_key_from_ts(session, hs_cursor, btree->id, key, 1, true));
            WT_STAT_CONN_INCR(session, cache_hs_key_truncate_non_ts);
            WT_STAT_DATA_INCR(session, cache_hs_key_truncate_non_ts);
            F_SET(first_non_ts_upd, WT_UPDATE_CLEARED_HS);
        } else if (first_non_ts_upd != NULL && !F_ISSET(first_non_ts_upd, WT_UPDATE_CLEARED_HS) &&
          (list->ins == NULL || ts_updates_in_hs)) {
            WT_ERR(__wt_hs_delete_key_from_ts(session, hs_cursor, btree->id, key, 1, true));
            WT_STAT_CONN_INCR(session, cache_hs_key_truncate_non_ts);
            WT_STAT_DATA_INCR(session, cache_hs_key_truncate_non_ts);
            F_SET(first_non_ts_upd, WT_UPDATE_CLEARED_HS);
        }

        WT_ERR(__hs_next_upd_full_value(session, &modifies, NULL, full_value, &upd));

        hs_inserted = squashed = false;

        /*
         * Flush the updates on stack. Stopping once we run out or we reach the onpage upd start
         * time point, we can squash modifies with the same start time point as the onpage upd away.
         */
        for (; modifies.size > 0 &&
             !(upd->txnid == list->onpage_upd->txnid &&
               upd->start_ts == list->onpage_upd->start_ts);
             tmp = full_value, full_value = prev_full_value, prev_full_value = tmp,
             upd = prev_upd) {
            WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD || upd->type == WT_UPDATE_MODIFY);

            tw.durable_start_ts = upd->durable_ts;
            tw.start_ts = upd->start_ts;
            tw.start_txn = upd->txnid;
            tombstone = NULL;
            __wt_modify_vector_peek(&modifies, &prev_upd);

            /*
             * For any uncommitted prepared updates written to disk, the stop timestamp of the last
             * update moved into the history store should be with max visibility to protect its
             * removal by checkpoint garbage collection until the data store update is committed.
             */
            if (prev_upd->prepare_state == WT_PREPARE_INPROGRESS) {
                WT_ASSERT(session,
                  list->onpage_upd->txnid == prev_upd->txnid &&
                    list->onpage_upd->start_ts == prev_upd->start_ts);
                tw.durable_stop_ts = tw.stop_ts = WT_TS_MAX;
                tw.stop_txn = WT_TXN_MAX;
            } else {
                /*
                 * Set the stop timestamp from durable timestamp instead of commit timestamp. The
                 * garbage collection of history store removes the history values once the stop
                 * timestamp is globally visible. i.e. durable timestamp of data store version.
                 */
                WT_ASSERT(session, prev_upd->start_ts <= prev_upd->durable_ts);
                tw.durable_stop_ts = prev_upd->durable_ts;
                tw.stop_ts = prev_upd->start_ts;
                tw.stop_txn = prev_upd->txnid;

                if (prev_upd->type == WT_UPDATE_TOMBSTONE)
                    tombstone = prev_upd;
            }

            WT_ERR(
              __hs_next_upd_full_value(session, &modifies, full_value, prev_full_value, &prev_upd));

            /* Squash the updates from the same transaction. */
            if (upd->start_ts == prev_upd->start_ts && upd->txnid == prev_upd->txnid) {
                squashed = true;
                continue;
            }

            /* Skip updates that are already in the history store. */
            if (F_ISSET(upd, WT_UPDATE_HS)) {
                if (hs_inserted)
                    WT_ERR_PANIC(session, WT_PANIC,
                      "Reinserting updates to the history store may corrupt the data as it may "
                      "clear the history store data newer than it.");
                continue;
            }

            /*
             * When we see an update older than a mixed mode update we need to insert it with a zero
             * start and stop timestamp. This means it'll still exist but only use txnid visibility
             * rules. As such older readers should still be able to see it.
             */
            if (F_ISSET(upd, WT_UPDATE_BEHIND_MIXED_MODE)) {
                tw.start_ts = tw.durable_start_ts = WT_TS_NONE;
                tw.stop_ts = tw.durable_stop_ts = WT_TS_NONE;
            }

            /*
             * If the time points are out of order (which can happen if the application performs
             * updates with out-of-order timestamps), so this value can never be seen, don't bother
             * inserting it. However if it was made obsolete by a mixed mode operation we still want
             * to insert it, it will be flagged as such.
             *
             * FIXME-WT-6443: We should be able to replace this with an assertion.
             */
            if (!F_ISSET(upd, WT_UPDATE_BEHIND_MIXED_MODE) &&
              (tw.stop_ts < upd->start_ts ||
                (tw.stop_ts == upd->start_ts && tw.stop_txn <= upd->txnid))) {
                __wt_verbose(session, WT_VERB_TIMESTAMP,
                  "Warning: fixing out-of-order timestamps %s earlier than previous update %s",
                  __wt_timestamp_to_string(tw.stop_ts, ts_string[0]),
                  __wt_timestamp_to_string(upd->start_ts, ts_string[1]));
                continue;
            }

            /* We should never write a prepared update to the history store. */
            WT_ASSERT(session,
              upd->prepare_state != WT_PREPARE_INPROGRESS &&
                upd->prepare_state != WT_PREPARE_LOCKED);

            /*
             * Ensure all the updates inserted to the history store are committed.
             *
             * Sometimes the application and the checkpoint threads will fall behind the eviction
             * threads, and they may choose an invisible update to write to the data store if the
             * update was previously selected by a failed eviction pass. Also the eviction may run
             * without a snapshot if the checkpoint is running concurrently. In those cases, check
             * whether the history transaction is committed or not against the global transaction
             * list. We expect the transaction is committed before the check. However, though very
             * rare, it is possible that the check may race with transaction commit and in this case
             * we may fail to catch the failure.
             */
#ifdef HAVE_DIAGNOSTIC
            if (!F_ISSET(session->txn, WT_TXN_HAS_SNAPSHOT) ||
              !__txn_visible_id(session, list->onpage_upd->txnid))
                WT_ASSERT(session, !__wt_txn_active(session, upd->txnid));
            else
                WT_ASSERT(session, __txn_visible_id(session, upd->txnid));
#endif

            /*
             * Calculate reverse modify and clear the history store records with timestamps when
             * inserting the first update. Always write on-disk data store updates to the history
             * store as a full update because the on-disk update will be the base update for all the
             * updates that are older than the on-disk update.
             *
             * Due to concurrent operation of checkpoint and eviction, it is possible that history
             * store may have more recent versions of a key than the on-disk version. Without a
             * proper base value in the history store, it can lead to wrong value being restored by
             * the RTS.
             */
            nentries = MAX_REVERSE_MODIFY_NUM;
            if (!F_ISSET(upd, WT_UPDATE_DS) && upd->type == WT_UPDATE_MODIFY &&
              enable_reverse_modify &&
              __wt_calc_modify(session, prev_full_value, full_value, prev_full_value->size / 10,
                entries, &nentries) == 0) {
                WT_ERR(__wt_modify_pack(hs_cursor, entries, nentries, &modify_value));
                WT_ERR(__hs_insert_record(
                  session, hs_cursor, btree, key, WT_UPDATE_MODIFY, modify_value, &tw));
                __wt_scr_free(session, &modify_value);
            } else
                WT_ERR(__hs_insert_record(
                  session, hs_cursor, btree, key, WT_UPDATE_STANDARD, full_value, &tw));

            /* Flag the update as now in the history store. */
            F_SET(upd, WT_UPDATE_HS);
            if (tombstone != NULL)
                F_SET(tombstone, WT_UPDATE_HS);
            hs_inserted = true;
            ++insert_cnt;
            if (squashed) {
                WT_STAT_CONN_DATA_INCR(session, cache_hs_write_squash);
                squashed = false;
            }
        }

        if (modifies.size > 0)
            WT_STAT_CONN_DATA_INCR(session, cache_hs_write_squash);
    }

    WT_ERR(__wt_block_manager_named_size(session, WT_HS_FILE, &hs_size));
    WT_STAT_CONN_SET(session, cache_hs_ondisk, hs_size);
    hs_btree = __wt_curhs_get_btree(hs_cursor);
    max_hs_size = hs_btree->file_max;
    if (max_hs_size != 0 && (uint64_t)hs_size > max_hs_size)
        WT_ERR_PANIC(session, WT_PANIC,
          "WiredTigerHS: file size of %" PRIu64 " exceeds maximum size %" PRIu64, (uint64_t)hs_size,
          max_hs_size);

err:
    if (ret == 0 && insert_cnt > 0)
        __hs_verbose_cache_stats(session, btree);

    __wt_scr_free(session, &key);
    /* modify_value is allocated in __wt_modify_pack. Free it if it is allocated. */
    if (modify_value != NULL)
        __wt_scr_free(session, &modify_value);
    __wt_modify_vector_free(&modifies);
    __wt_scr_free(session, &full_value);
    __wt_scr_free(session, &prev_full_value);

    WT_TRET(hs_cursor->close(hs_cursor));
    return (ret);
}

/*
 * __wt_hs_delete_key_from_ts --
 *     Delete history store content of a given key from a timestamp.
 */
int
__wt_hs_delete_key_from_ts(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id,
  const WT_ITEM *key, wt_timestamp_t ts, bool reinsert)
{
    WT_DECL_RET;
    bool hs_read_committed;

    hs_read_committed = F_ISSET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    if (!hs_read_committed)
        F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    hs_cursor->set_key(hs_cursor, 3, btree_id, key, ts);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor), true);
    /* Empty history store is fine. */
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }

    WT_ERR(__hs_delete_key_from_pos(session, hs_cursor, btree_id, key, reinsert));
done:
err:
    if (!hs_read_committed)
        F_CLR(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    return (ret);
}

/*
 * __hs_fixup_out_of_order_from_pos --
 *     Fixup existing out-of-order updates in the history store. This function works by looking
 *     ahead of the current cursor position for entries for the same key, removing them and
 *     reinserting them at the timestamp that is currently being inserted.
 */
static int
__hs_fixup_out_of_order_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, WT_BTREE *btree,
  const WT_ITEM *key, wt_timestamp_t ts, uint64_t *counter)
{
    WT_CURSOR *hs_insert_cursor;
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_RET;
    WT_ITEM hs_key, hs_value;
    WT_TIME_WINDOW tw, hs_insert_tw;
    wt_timestamp_t hs_ts;
    uint64_t hs_counter, hs_upd_type;
    uint32_t hs_btree_id;
#ifdef HAVE_DIAGNOSTIC
    int cmp;
#endif
    char ts_string[5][WT_TS_INT_STRING_SIZE];

    hs_insert_cursor = NULL;
    hs_cbt = __wt_curhs_get_cbt(hs_cursor);
    WT_CLEAR(hs_key);
    WT_CLEAR(hs_value);

#ifndef HAVE_DIAGNOSTIC
    WT_UNUSED(key);
#endif
    /*
     * Position ourselves at the beginning of the key range that we may have to fixup. Prior to
     * getting here, we've positioned our cursor at the end of a key/timestamp range and then done a
     * "next". Normally that would leave us pointing at higher timestamps for the same key (if any)
     * but in the case where our insertion timestamp is the lowest for that key, our cursor may be
     * pointing at the previous key and can potentially race with additional key insertions. We need
     * to keep doing "next" until we've got a key greater than the one we attempted to position
     * ourselves with.
     */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_ts, &hs_counter));
        WT_ASSERT(session, hs_btree_id == btree->id);
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        WT_ASSERT(session, cmp == 0);
#endif
        if (hs_ts > ts)
            break;
    }
    if (ret == WT_NOTFOUND)
        return (0);
    WT_ERR(ret);

    /*
     * The goal of this fixup function is to move out-of-order content to maintain ordering in the
     * history store. We do this by removing content with higher timestamps and reinserting it
     * behind (from search's point of view) the newly inserted update. Even though these updates
     * will all have the same timestamp, they cannot be discarded since older readers may need to
     * see them after they've been moved due to their transaction id.
     *
     * For example, if we're inserting an update at timestamp 3 with value ddd:
     * btree key ts counter value
     * 2     foo 5  0       aaa
     * 2     foo 6  0       bbb
     * 2     foo 7  0       ccc
     *
     * We want to end up with this:
     * btree key ts counter value
     * 2     foo 3  0       aaa
     * 2     foo 3  1       bbb
     * 2     foo 3  2       ccc
     * 2     foo 3  3       ddd
     */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_ts, &hs_counter));
        WT_ASSERT(session, hs_btree_id == btree->id);
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        WT_ASSERT(session, cmp == 0);
#endif
        /*
         * If we got here, we've got out-of-order updates in the history store.
         *
         * Our strategy to rectify this is to remove all records for the same key with a higher
         * timestamp than the one that we're inserting on and reinsert them at the same timestamp
         * that we're inserting with.
         */
        WT_ASSERT(session, hs_ts > ts);

        /*
         * Don't incur the overhead of opening this new cursor unless we need it. In the regular
         * case, we'll never get here.
         */
        if (hs_insert_cursor == NULL)
            WT_ERR(__wt_curhs_open(session, NULL, &hs_insert_cursor));

        /*
         * If these history store records are resolved prepared updates, their durable timestamps
         * will be clobbered by our fix-up process. Keep track of how often this is happening.
         */
        if (hs_cbt->upd_value->tw.start_ts != hs_cbt->upd_value->tw.durable_start_ts ||
          hs_cbt->upd_value->tw.stop_ts != hs_cbt->upd_value->tw.durable_stop_ts)
            WT_STAT_CONN_DATA_INCR(session, cache_hs_order_lose_durable_timestamp);

        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "fixing existing out-of-order updates by moving them; start_ts=%s, durable_start_ts=%s, "
          "stop_ts=%s, durable_stop_ts=%s, new_ts=%s",
          __wt_timestamp_to_string(hs_cbt->upd_value->tw.start_ts, ts_string[0]),
          __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_start_ts, ts_string[1]),
          __wt_timestamp_to_string(hs_cbt->upd_value->tw.stop_ts, ts_string[2]),
          __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_stop_ts, ts_string[3]),
          __wt_timestamp_to_string(ts, ts_string[4]));

        hs_insert_tw.start_ts = hs_insert_tw.durable_start_ts = ts;
        hs_insert_tw.start_txn = hs_cbt->upd_value->tw.start_txn;

        /*
         * We're going to be inserting something immediately after with the same timestamp. Either
         * another moved update OR the update itself that triggered the correction. In either case,
         * we should preserve the stop transaction id.
         */
        hs_insert_tw.stop_ts = hs_insert_tw.durable_stop_ts = ts;
        hs_insert_tw.stop_txn = hs_cbt->upd_value->tw.stop_txn;

        /* Extract the underlying value for reinsertion. */
        WT_ERR(hs_cursor->get_value(
          hs_cursor, &tw.durable_stop_ts, &tw.durable_start_ts, &hs_upd_type, &hs_value));

        /* Insert the value back with different timestamps. */
        hs_insert_cursor->set_key(hs_insert_cursor, 4, btree->id, &hs_key, ts, *counter);
        hs_insert_cursor->set_value(hs_insert_cursor, &hs_insert_tw, hs_insert_tw.durable_stop_ts,
          hs_insert_tw.durable_start_ts, (uint64_t)hs_upd_type, &hs_value);
        WT_ERR(hs_insert_cursor->insert(hs_insert_cursor));
        ++(*counter);

        /* Delete the entry with higher timestamp. */
        WT_ERR(hs_cursor->remove(hs_cursor));
        WT_STAT_CONN_INCR(session, cache_hs_order_fixup_move);
        WT_STAT_DATA_INCR(session, cache_hs_order_fixup_move);
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
err:
    if (hs_insert_cursor != NULL)
        hs_insert_cursor->close(hs_insert_cursor);
    return (ret);
}

/*
 * __hs_delete_key_from_pos --
 *     Delete an entire key's worth of data in the history store. If we chose to reinsert the values
 *     the reinserted values will have 0 start and stop timestamps to ensure that they only use
 *     txnid based visibility rules.
 */
static int
__hs_delete_key_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id,
  const WT_ITEM *key, bool reinsert)
{
    WT_CURSOR *hs_insert_cursor;
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_RET;
    WT_ITEM hs_key, hs_value;
    WT_TIME_WINDOW hs_insert_tw;
    wt_timestamp_t durable_timestamp, hs_start_ts, hs_stop_durable_ts;
    uint64_t hs_counter, hs_insert_counter, hs_upd_type;
    uint32_t hs_btree_id;

    hs_cbt = __wt_curhs_get_cbt(hs_cursor);
    hs_insert_counter = 0;
    WT_CLEAR(hs_key);
    WT_CLEAR(hs_value);

    hs_insert_cursor = NULL;
    if (reinsert) {
        /*
         * Determine the starting value of our counter, i.e. highest counter value of the timestamp
         * range for timestamp 0. We'll be inserting at timestamp 0 and don't want to overwrite a
         * currently existing counter.
         *
         * The cursor will also be positioned at the start of the range that we wish to start
         * inserting.
         */
        WT_WITHOUT_DHANDLE(session, ret = __wt_curhs_open(session, NULL, &hs_insert_cursor));
        WT_ERR(ret);
        F_SET(hs_insert_cursor, WT_CURSTD_HS_READ_COMMITTED);
        hs_insert_cursor->set_key(hs_insert_cursor, 4, btree_id, key, WT_TS_NONE, UINT64_MAX);
        WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_before(session, hs_insert_cursor), true);

        if (ret == WT_NOTFOUND) {
            hs_insert_counter = 0;
            ret = 0;
        } else {
            WT_ERR(hs_insert_cursor->get_key(
              hs_insert_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_insert_counter));
            WT_ASSERT(session, hs_start_ts == WT_TS_NONE);
            /*
             * Increment the history store counter that we'll be using to insert with to avoid
             * overwriting the record we just found.
             */
            hs_insert_counter++;
        }
    }

    /* Begin iterating over the range of entries we expect to replace. */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));

        if (reinsert) {
            WT_ERR(hs_cursor->get_value(
              hs_cursor, &hs_stop_durable_ts, &durable_timestamp, &hs_upd_type, &hs_value));

            /* Reinsert entry with zero timestamp. */
            hs_insert_tw.start_ts = hs_insert_tw.durable_start_ts = WT_TS_NONE;
            hs_insert_tw.start_txn = hs_cbt->upd_value->tw.start_txn;

            hs_insert_tw.stop_ts = hs_insert_tw.durable_stop_ts = WT_TS_NONE;
            hs_insert_tw.stop_txn = hs_cbt->upd_value->tw.stop_txn;

            hs_insert_cursor->set_key(
              hs_insert_cursor, 4, btree_id, key, WT_TS_NONE, hs_insert_counter);
            hs_insert_cursor->set_value(hs_insert_cursor, &hs_insert_tw, WT_TS_NONE, WT_TS_NONE,
              (uint64_t)hs_upd_type, &hs_value);
            WT_ERR(hs_insert_cursor->insert(hs_insert_cursor));
            WT_STAT_CONN_INCR(session, cache_hs_insert);
            WT_STAT_DATA_INCR(session, cache_hs_insert);

            hs_insert_counter++;
        }

        /*
         * Remove the key using history store cursor interface.
         *
         * If anything fails after this point and we're reinserting we need to panic as it will
         * leave our history store in an unexpected state with duplicate entries.
         */
        if ((ret = hs_cursor->remove(hs_cursor)) != 0) {
            if (reinsert)
                WT_ERR_PANIC(session, WT_PANIC,
                  "Failed to insert tombstone, history store now "
                  " contains duplicate values.");
            else
                WT_ERR(ret);
        }
        WT_STAT_CONN_INCR(session, cache_hs_key_truncate);
        WT_STAT_DATA_INCR(session, cache_hs_key_truncate);
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
err:
    if (hs_insert_cursor != NULL)
        hs_insert_cursor->close(hs_insert_cursor);
    return (ret);
}
