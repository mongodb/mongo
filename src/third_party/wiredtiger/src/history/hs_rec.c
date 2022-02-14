/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __hs_delete_reinsert_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  uint32_t btree_id, const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool error_on_ooo_ts,
  uint64_t *hs_counter);

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

    if (!WT_VERBOSE_ISSET(session, WT_VERB_HS) && !WT_VERBOSE_ISSET(session, WT_VERB_HS_ACTIVITY))
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

        __wt_verbose_multi(session,
          WT_DECL_VERBOSE_MULTI_CATEGORY(
            ((WT_VERBOSE_CATEGORY[]){WT_VERB_HS, WT_VERB_HS_ACTIVITY})),
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
  const uint8_t type, const WT_ITEM *hs_value, WT_TIME_WINDOW *tw, bool error_on_ooo_ts)
{
    WT_CURSOR_BTREE *hs_cbt;
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
    bool hs_read_all_flag;
    uint64_t counter, hs_counter;
    uint32_t hs_btree_id;

    counter = hs_counter = 0;

    /* Verify that the timestamps are in increasing order. */
    WT_ASSERT(session, tw->stop_ts >= tw->start_ts && tw->durable_stop_ts >= tw->durable_start_ts);

    /*
     * We might be entering this code from application thread's context. We should make sure that we
     * are not using snapshot associated with application session to perform visibility checks on
     * history store records. Note that the history store cursor performs visibility checks based on
     * snapshot if none of WT_CURSTD_HS_READ_ALL or WT_CURSTD_HS_READ_COMMITTED flags are set.
     */
    WT_ASSERT(session,
      F_ISSET(session, WT_SESSION_INTERNAL) ||
        F_ISSET(cursor, WT_CURSTD_HS_READ_ALL | WT_CURSTD_HS_READ_COMMITTED));

    /*
     * Keep track if the caller had set WT_CURSTD_HS_READ_ALL flag on the history store cursor. We
     * want to preserve the flags set by the caller when we exit from this function. Also, we want
     * to explicitly set the flag WT_CURSTD_HS_READ_ALL only for the search_near operations on the
     * history store cursor and perform all other cursor operations using the flags set by the
     * caller of this function.
     */
    hs_read_all_flag = F_ISSET(cursor, WT_CURSTD_HS_READ_ALL);

    /* Allocate buffers for the history store and search key. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));

#ifdef HAVE_DIAGNOSTIC
    /* Allocate buffer for the existing history store value for the same key. */
    WT_ERR(__wt_scr_alloc(session, 0, &existing_val));
#endif

    hs_cbt = __wt_curhs_get_cbt(cursor);

    /* Sanity check that the btree is not a history store btree. */
    WT_ASSERT(session, !WT_IS_HS(btree));

    /*
     * Only deltas or full updates should be written to the history store. More specifically, we
     * should NOT be writing tombstone records in the history store table.
     */
    WT_ASSERT(session, type == WT_UPDATE_STANDARD || type == WT_UPDATE_MODIFY);

    /*
     * Setting the flag WT_CURSTD_HS_READ_ALL before searching the history store optimizes the
     * search routine as we do not skip globally visible tombstones during the search.
     */
    F_SET(cursor, WT_CURSTD_HS_READ_ALL);

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
             * The same value should not be inserted again unless:
             * 1. The previous entry is already deleted (i.e. the stop timestamp is globally
             * visible)
             * 2. It came from a different transaction
             * 3. It came from the same transaction but with a different timestamp
             * 4. The prepared rollback left the history store entry when checkpoint is in progress.
             */
            if (cmp == 0) {
                if (!__wt_txn_tw_stop_visible_all(session, &hs_cbt->upd_value->tw) &&
                  tw->start_txn != WT_TXN_NONE &&
                  tw->start_txn == hs_cbt->upd_value->tw.start_txn &&
                  tw->start_ts == hs_cbt->upd_value->tw.start_ts && tw->start_ts != tw->stop_ts) {
                    /*
                     * If we have issues with duplicate history store records, we want to be able to
                     * distinguish between modifies and full updates. Since modifies are not
                     * idempotent, having them inserted multiple times can cause invalid values to
                     * be read.
                     */
                    WT_ASSERT(session,
                      type != WT_UPDATE_MODIFY && (uint8_t)upd_type_full_diag != WT_UPDATE_MODIFY);
                }
            }
            counter = hs_counter + 1;
        }
#else
        if (tw->start_ts == hs_start_ts)
            counter = hs_counter + 1;
#endif
    }

    /*
     * Look ahead for any higher timestamps. If we find updates, we should remove them and reinsert
     * them at the current timestamp. If there were no keys equal to or less than our target key, we
     * would have received WT_NOT_FOUND. In that case we need to search again with a higher
     * timestamp.
     */
    if (ret == 0) {
        /*
         * Check if the current history store update's stop timestamp is out of order with respect
         * to the update to be inserted before before moving onto the next record.
         */
        if (hs_cbt->upd_value->tw.stop_ts <= tw->start_ts)
            WT_ERR_NOTFOUND_OK(cursor->next(cursor), true);
        else
            counter = hs_counter + 1;
    } else {
        cursor->set_key(cursor, 3, btree->id, key, tw->start_ts + 1);
        WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, cursor), true);
    }

    /*
     * It is possible to insert a globally visible update into the history store with larger
     * timestamps ahead of it. An example would be a mixed-mode update getting moved to the history
     * store. This scenario can avoid detection earlier in reconciliation and result in an EBUSY
     * being returned as it detects out-of-order timestamps. To prevent this we allow globally
     * visible updates to fix history store content even if eviction is running concurrently with a
     * checkpoint.
     *
     * This is safe because global visibility considers the checkpoint transaction id and timestamp
     * while it is running, i.e. if the update is globally visible to eviction it will be globally
     * visible to checkpoint and the modifications it makes to the history store will be the same as
     * what checkpoint would've done.
     */
    if (error_on_ooo_ts && __wt_txn_tw_start_visible_all(session, tw)) {
        error_on_ooo_ts = false;
    }

    if (ret == 0)
        WT_ERR(__hs_delete_reinsert_from_pos(
          session, cursor, btree->id, key, tw->start_ts + 1, true, error_on_ooo_ts, &counter));

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

err:
    if (!hs_read_all_flag)
        F_CLR(cursor, WT_CURSTD_HS_READ_ALL);
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
__hs_next_upd_full_value(WT_SESSION_IMPL *session, WT_UPDATE_VECTOR *updates,
  WT_ITEM *older_full_value, WT_ITEM *full_value, WT_UPDATE **updp)
{
    WT_UPDATE *upd;
    *updp = NULL;

    __wt_update_vector_pop(updates, &upd);
    if (upd->type == WT_UPDATE_TOMBSTONE) {
        if (updates->size == 0) {
            WT_ASSERT(session, older_full_value == NULL);
            *updp = upd;
            return (0);
        }

        __wt_update_vector_pop(updates, &upd);
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
 *     Copy one set of saved updates into the database's history store table. Whether the function
 *     fails or succeeds, if there is a successful write to history, cache_write_hs is set to true.
 */
int
__wt_hs_insert_updates(WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_MULTI *multi)
{
    WT_BTREE *btree, *hs_btree;
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(full_value);
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(modify_value);
    WT_DECL_ITEM(prev_full_value);
    WT_DECL_ITEM(tmp);
    WT_DECL_RET;
/* Limit the number of consecutive reverse modifies. */
#define WT_MAX_CONSECUTIVE_REVERSE_MODIFY 10
/* If the limit is exceeded, we will insert a full update to the history store */
#define MAX_REVERSE_MODIFY_NUM 16
    WT_MODIFY entries[MAX_REVERSE_MODIFY_NUM];
    WT_UPDATE_VECTOR updates;
    WT_UPDATE_VECTOR out_of_order_ts_updates;
    WT_SAVE_UPD *list;
    WT_UPDATE *first_globally_visible_upd, *fix_ts_upd, *min_ts_upd, *out_of_order_ts_upd;
    WT_UPDATE *newest_hs, *non_aborted_upd, *oldest_upd, *prev_upd, *ref_upd, *tombstone, *upd;
    WT_TIME_WINDOW tw;
    wt_off_t hs_size;
    uint64_t insert_cnt, max_hs_size, modify_cnt;
    uint64_t cache_hs_insert_full_update, cache_hs_insert_reverse_modify, cache_hs_write_squash;
    uint32_t i;
    uint8_t *p;
    int nentries;
    bool enable_reverse_modify, error_on_ooo_ts, hs_inserted, squashed;

    r->cache_write_hs = false;
    btree = S2BT(session);
    prev_upd = NULL;
    WT_TIME_WINDOW_INIT(&tw);
    insert_cnt = 0;
    error_on_ooo_ts = F_ISSET(r, WT_REC_CHECKPOINT_RUNNING);
    cache_hs_insert_full_update = cache_hs_insert_reverse_modify = cache_hs_write_squash = 0;

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    __wt_update_vector_init(session, &updates);
    /*
     * We use another stack to store the out-of-order timestamp updates (including updates without a
     * timestamp). We walk the update chain from the newest to the oldest. Once an out-of-order
     * timestamp update is detected, and it has a lower timestamp than the head of the stack, it is
     * pushed to the stack. When we are inserting updates to the history store, we compare the
     * update's timestamp with the head of the stack. If it is larger than the out-of-order
     * timestamp, we fix the timestamp by inserting with the out-of-order timestamp. If the update
     * we are inserting is the head of the stack, we pop it from the stack.
     */
    __wt_update_vector_init(session, &out_of_order_ts_updates);

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
        switch (r->page->type) {
        case WT_PAGE_COL_FIX:
        case WT_PAGE_COL_VAR:
            p = key->mem;
            WT_ERR(__wt_vpack_uint(&p, 0, WT_INSERT_RECNO(list->ins)));
            key->size = WT_PTRDIFF(p, key->data);
            break;
        case WT_PAGE_ROW_LEAF:
            if (list->ins == NULL) {
                WT_WITH_BTREE(
                  session, btree, ret = __wt_row_leaf_key(session, r->page, list->rip, key, false));
                WT_ERR(ret);
            } else {
                key->data = WT_INSERT_KEY(list->ins);
                key->size = WT_INSERT_KEY_SIZE(list->ins);
            }
            break;
        default:
            WT_ERR(__wt_illegal_value(session, r->page->type));
        }

        newest_hs = first_globally_visible_upd = min_ts_upd = out_of_order_ts_upd = NULL;
        ref_upd = list->onpage_upd;

        __wt_update_vector_clear(&out_of_order_ts_updates);
        __wt_update_vector_clear(&updates);

        /*
         * Reverse deltas are only supported on 'S' and 'u' value formats.
         */
        enable_reverse_modify =
          (WT_STREQ(btree->value_format, "S") || WT_STREQ(btree->value_format, "u"));

        /*
         * The algorithm assumes the oldest update on the update chain in memory is either a full
         * update or a tombstone.
         *
         * This is guaranteed by __wt_rec_upd_select appends the original onpage value at the end of
         * the chain. It also assumes the onpage_upd selected cannot be a TOMBSTONE and the update
         * newer than a TOMBSTONE must be a full update.
         *
         * The algorithm walks from the oldest update, or the most recently inserted into history
         * store update, to the newest update and builds full updates along the way. It sets the
         * stop time point of the update to the start time point of the next update, squashes the
         * updates that are from the same transaction and of the same start timestamp, checks if the
         * update can be written as reverse modification, and inserts the update to the history
         * store either as a full update or a reverse modification.
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
        squashed = false;
        for (upd = list->onpage_upd, non_aborted_upd = prev_upd = NULL; upd != NULL;
             prev_upd = non_aborted_upd, upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;

            non_aborted_upd = upd;

            /* Detect out of order timestamp update. */
            if (min_ts_upd != NULL && min_ts_upd->start_ts < upd->start_ts &&
              out_of_order_ts_upd != min_ts_upd) {
                /*
                 * Fail the eviction if we detect out of order timestamps and the error flag is set.
                 * We cannot modify the history store to fix the out of order timestamp updates as
                 * it may make the history store checkpoint inconsistent.
                 */
                if (error_on_ooo_ts) {
                    ret = EBUSY;
                    WT_STAT_CONN_INCR(session, cache_eviction_fail_checkpoint_out_of_order_ts);
                    goto err;
                }

                /*
                 * Always insert full update to the history store if we detect out of order
                 * timestamp update.
                 */
                enable_reverse_modify = false;
                WT_ERR(__wt_update_vector_push(&out_of_order_ts_updates, min_ts_upd));
                out_of_order_ts_upd = min_ts_upd;
            } else if (upd->prepare_state != WT_PREPARE_INPROGRESS &&
              (min_ts_upd == NULL || upd->start_ts <= min_ts_upd->start_ts))
                min_ts_upd = upd;

            WT_ERR(__wt_update_vector_push(&updates, upd));

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

            /*
             * Find the first update to insert to the history store. (The value that is just older
             * than the on-page value)
             */
            if (newest_hs == NULL) {
                if (upd->txnid != ref_upd->txnid || upd->start_ts != ref_upd->start_ts) {
                    if (upd->type == WT_UPDATE_TOMBSTONE)
                        ref_upd = upd;
                    else
                        newest_hs = upd;
                    if (squashed) {
                        ++cache_hs_write_squash;
                        squashed = false;
                    }
                } else if (upd != ref_upd)
                    squashed = true;
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

        WT_ASSERT(session, updates.size > 0);

        __wt_update_vector_peek(&updates, &oldest_upd);

        WT_ASSERT(session,
          oldest_upd->type == WT_UPDATE_STANDARD || oldest_upd->type == WT_UPDATE_TOMBSTONE);

        /*
         * Fix the history store record here if the oldest update is a tombstone because we don't
         * have the cursor placed at the correct place to fix the history store records when
         * inserting the first update and it may be skipped if there is nothing to insert to the
         * history store.
         */
        if (oldest_upd->type == WT_UPDATE_TOMBSTONE) {
            if (out_of_order_ts_upd != NULL && out_of_order_ts_upd->start_ts < oldest_upd->start_ts)
                fix_ts_upd = out_of_order_ts_upd;
            else
                fix_ts_upd = oldest_upd;

            if (!F_ISSET(fix_ts_upd, WT_UPDATE_FIXED_HS)) {
                /* Delete and reinsert any update of the key with a higher timestamp. */
                WT_ERR(__wt_hs_delete_key_from_ts(session, hs_cursor, btree->id, key,
                  fix_ts_upd->start_ts + 1, true, error_on_ooo_ts));
                F_SET(fix_ts_upd, WT_UPDATE_FIXED_HS);
            }
        }

        /* Skip if we have nothing to insert to the history store. */
        if (newest_hs == NULL || F_ISSET(newest_hs, WT_UPDATE_HS)) {
            /* The onpage value is squashed. */
            if (newest_hs == NULL && squashed)
                ++cache_hs_write_squash;
            continue;
        }

        /* Construct the oldest full update. */
        WT_ERR(__hs_next_upd_full_value(session, &updates, NULL, full_value, &upd));

        hs_inserted = false;

        /*
         * Flush the updates on stack. Stopping once we finish inserting the newest history store
         * value.
         */
        modify_cnt = 0;
        for (;; tmp = full_value, full_value = prev_full_value, prev_full_value = tmp,
                upd = prev_upd) {
            /* We should never insert the onpage value to the history store. */
            WT_ASSERT(session, upd != list->onpage_upd);
            WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD || upd->type == WT_UPDATE_MODIFY);
            /* We should never insert prepared updates to the history store. */
            WT_ASSERT(session, upd->prepare_state != WT_PREPARE_INPROGRESS);

            tombstone = NULL;
            __wt_update_vector_peek(&updates, &prev_upd);

            if (out_of_order_ts_updates.size > 0) {
                __wt_update_vector_peek(&out_of_order_ts_updates, &out_of_order_ts_upd);
            } else
                out_of_order_ts_upd = NULL;

            if (out_of_order_ts_upd != NULL && out_of_order_ts_upd->start_ts < upd->start_ts) {
                tw.durable_start_ts = out_of_order_ts_upd->durable_ts;
                tw.start_ts = out_of_order_ts_upd->start_ts;
            } else {
                tw.durable_start_ts = upd->durable_ts;
                tw.start_ts = upd->start_ts;
            }
            tw.start_txn = upd->txnid;

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

                /*
                 * Pop from the out of order timestamp updates stack if the previous update or the
                 * current update is at the head of the stack. We need to check both cases because
                 * if there is a tombstone older than the out of order timestamp, we would not pop
                 * it because we skip the tombstone. Pop it when we are inserting it instead.
                 *
                 * Here it is assumed that the out of order update is equal to the oldest update
                 * among the multiple out of order consecutive updates that have same timestamps.
                 * For instance, U1@10 -> U2@10 -> U3@10 -> U4@20, U3 which is the oldest update
                 * will be the out of order update.
                 */
                if (out_of_order_ts_upd != NULL &&
                  (out_of_order_ts_upd == prev_upd || out_of_order_ts_upd == upd)) {
                    __wt_update_vector_pop(&out_of_order_ts_updates, &out_of_order_ts_upd);
                    out_of_order_ts_upd = NULL;
                }

                if (out_of_order_ts_upd != NULL &&
                  out_of_order_ts_upd->start_ts < prev_upd->start_ts) {
                    tw.durable_stop_ts = out_of_order_ts_upd->durable_ts;
                    tw.stop_ts = out_of_order_ts_upd->start_ts;
                } else {
                    tw.durable_stop_ts = prev_upd->durable_ts;
                    tw.stop_ts = prev_upd->start_ts;
                }
                tw.stop_txn = prev_upd->txnid;

                if (prev_upd->type == WT_UPDATE_TOMBSTONE)
                    tombstone = prev_upd;
            }

            WT_ERR(
              __hs_next_upd_full_value(session, &updates, full_value, prev_full_value, &prev_upd));

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
             * inserting the first update. Always write the newest update in the history store as a
             * full update. We don't want to handle the edge cases that the reverse modifies be
             * applied to the wrong on-disk base value. This also limits the number of consecutive
             * reverse modifies for standard updates. We want to ensure we do not store a large
             * chain of reverse modifies as to impact read performance.
             *
             * Due to concurrent operation of checkpoint and eviction, it is possible that history
             * store may have more recent versions of a key than the on-disk version. Without a
             * proper base value in the history store, it can lead to wrong value being restored by
             * the RTS.
             */
            nentries = MAX_REVERSE_MODIFY_NUM;
            if (upd != newest_hs && enable_reverse_modify &&
              modify_cnt < WT_MAX_CONSECUTIVE_REVERSE_MODIFY &&
              __wt_calc_modify(session, prev_full_value, full_value, prev_full_value->size / 10,
                entries, &nentries) == 0) {
                WT_ERR(__wt_modify_pack(hs_cursor, entries, nentries, &modify_value));
                WT_ERR(__hs_insert_record(session, hs_cursor, btree, key, WT_UPDATE_MODIFY,
                  modify_value, &tw, error_on_ooo_ts));
                ++cache_hs_insert_reverse_modify;
                __wt_scr_free(session, &modify_value);
                ++modify_cnt;
            } else {
                modify_cnt = 0;
                WT_ERR(__hs_insert_record(session, hs_cursor, btree, key, WT_UPDATE_STANDARD,
                  full_value, &tw, error_on_ooo_ts));
                ++cache_hs_insert_full_update;
            }

            /* Flag the update as now in the history store. */
            F_SET(upd, WT_UPDATE_HS);
            if (tombstone != NULL)
                F_SET(tombstone, WT_UPDATE_HS);

            hs_inserted = true;
            ++insert_cnt;
            if (squashed) {
                ++cache_hs_write_squash;
                squashed = false;
            }

            if (upd == newest_hs)
                break;
        }

        /*
         * In the case that the onpage value is an out of order timestamp update and the update
         * older than it is a tombstone, it remains in the stack.
         */
        WT_ASSERT(session, out_of_order_ts_updates.size <= 1);
#ifdef HAVE_DIAGNOSTIC
        if (out_of_order_ts_updates.size == 1) {
            __wt_update_vector_peek(&out_of_order_ts_updates, &upd);
            WT_ASSERT(session,
              upd->txnid == list->onpage_upd->txnid && upd->start_ts == list->onpage_upd->start_ts);
        }
#endif
    }

    WT_ERR(__wt_block_manager_named_size(session, WT_HS_FILE, &hs_size));
    hs_btree = __wt_curhs_get_btree(hs_cursor);
    max_hs_size = hs_btree->file_max;
    if (max_hs_size != 0 && (uint64_t)hs_size > max_hs_size)
        WT_ERR_PANIC(session, WT_PANIC,
          "WiredTigerHS: file size of %" PRIu64 " exceeds maximum size %" PRIu64, (uint64_t)hs_size,
          max_hs_size);

err:
    if (ret == 0 && insert_cnt > 0)
        __hs_verbose_cache_stats(session, btree);

    /* cache_write_hs is set to true as there was at least one successful write to history. */
    if (insert_cnt > 0)
        r->cache_write_hs = true;

    __wt_scr_free(session, &key);
    /* modify_value is allocated in __wt_modify_pack. Free it if it is allocated. */
    if (modify_value != NULL)
        __wt_scr_free(session, &modify_value);
    __wt_update_vector_free(&updates);
    __wt_update_vector_free(&out_of_order_ts_updates);
    __wt_scr_free(session, &full_value);
    __wt_scr_free(session, &prev_full_value);

    WT_TRET(hs_cursor->close(hs_cursor));

    /* Update the statistics. */
    WT_STAT_CONN_DATA_INCRV(session, cache_hs_insert, insert_cnt);
    WT_STAT_CONN_DATA_INCRV(session, cache_hs_insert_full_update, cache_hs_insert_full_update);
    WT_STAT_CONN_DATA_INCRV(
      session, cache_hs_insert_reverse_modify, cache_hs_insert_reverse_modify);
    WT_STAT_CONN_DATA_INCRV(session, cache_hs_write_squash, cache_hs_write_squash);

    return (ret);
}

/*
 * __wt_hs_delete_key_from_ts --
 *     Delete history store content of a given key from a timestamp and optionally reinsert them
 *     with ts-1 timestamp.
 */
int
__wt_hs_delete_key_from_ts(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id,
  const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool error_on_ooo_ts)
{
    WT_DECL_RET;
    WT_ITEM hs_key;
    wt_timestamp_t hs_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    bool hs_read_all_flag;

    /*
     * If we will delete all the updates of the key from the history store, we should not reinsert
     * any update.
     */
    WT_ASSERT(session, ts > WT_TS_NONE || !reinsert);

    hs_read_all_flag = F_ISSET(hs_cursor, WT_CURSTD_HS_READ_ALL);

    hs_cursor->set_key(hs_cursor, 3, btree_id, key, ts);
    /*
     * Setting the flag WT_CURSTD_HS_READ_ALL before searching the history store optimizes the
     * search routine as we do not skip globally visible tombstones during the search.
     */
    F_SET(hs_cursor, WT_CURSTD_HS_READ_ALL);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_after(session, hs_cursor), true);
    /* Empty history store is fine. */
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    } else {
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_ts, &hs_counter));
        ++hs_counter;
    }

    WT_ERR(__hs_delete_reinsert_from_pos(
      session, hs_cursor, btree_id, key, ts, reinsert, error_on_ooo_ts, &hs_counter));

done:
err:
    if (!hs_read_all_flag)
        F_CLR(hs_cursor, WT_CURSTD_HS_READ_ALL);
    return (ret);
}

/*
 * __hs_delete_reinsert_from_pos --
 *     Delete updates in the history store if the start timestamp of the update is larger or equal
 *     to the specified timestamp and optionally reinsert them with ts-1 timestamp. This function
 *     works by looking ahead of the current cursor position for entries for the same key, removing
 *     them.
 */
static int
__hs_delete_reinsert_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id,
  const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool error_on_ooo_ts, uint64_t *counter)
{
    WT_CURSOR *hs_insert_cursor;
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_RET;
    WT_ITEM hs_key, hs_value;
    WT_TIME_WINDOW hs_insert_tw, tw, *twp;
    wt_timestamp_t hs_ts;
    uint64_t cache_hs_order_lose_durable_timestamp, cache_hs_order_reinsert, cache_hs_order_remove;
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
    cache_hs_order_lose_durable_timestamp = cache_hs_order_reinsert = cache_hs_order_remove = 0;

#ifndef HAVE_DIAGNOSTIC
    WT_UNUSED(key);
#endif

    /* If we will delete all the updates of the key from the history store, we should not reinsert
     * any update. */
    WT_ASSERT(session, ts > WT_TS_NONE || !reinsert);

    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* Ignore records that are obsolete. */
        __wt_hs_upd_time_window(hs_cursor, &twp);
        if (__wt_txn_tw_stop_visible_all(session, twp))
            continue;

        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_ts, &hs_counter));
        WT_ASSERT(session, hs_btree_id == btree_id);
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        WT_ASSERT(session, cmp == 0);
#endif
        /*
         * We have found a key with a timestamp larger than or equal to the specified timestamp.
         * Always use the start timestamp retrieved from the key instead of the start timestamp from
         * the cell. The cell's start timestamp can be cleared during reconciliation if it is
         * globally visible.
         */
        if (hs_ts >= ts || twp->stop_ts >= ts)
            break;
    }
    if (ret == WT_NOTFOUND)
        return (0);
    WT_ERR(ret);

    /*
     * Fail the eviction if we detect out of order timestamps when we've passed the error return
     * flag. We cannot modify the history store to fix the out of order timestamp updates as it may
     * make the history store checkpoint inconsistent.
     */
    if (error_on_ooo_ts) {
        ret = EBUSY;
        WT_STAT_CONN_INCR(session, cache_eviction_fail_checkpoint_out_of_order_ts);
        goto err;
    }

    /*
     * The goal of this function is to move out-of-order content to maintain ordering in the
     * history store. We do this by removing content with higher timestamps and reinserting it
     * behind (from search's point of view) the newly inserted update. Even though these updates
     * will all have the same timestamp, they cannot be discarded since older readers may need to
     * see them after they've been moved due to their transaction id.
     *
     * For example, if we're inserting an update at timestamp 3 with value ddd:
     * btree key ts counter value stop_ts
     * 2     foo 5  0       aaa     6
     * 2     foo 6  0       bbb     7
     * 2     foo 7  0       ccc     8
     *
     * We want to end up with this:
     * btree key ts counter value stop_ts
     * 2     foo 3  0       aaa    3
     * 2     foo 3  1       bbb    3
     * 2     foo 3  2       ccc    3
     * 2     foo 3  3       ddd    3
     *
     * Another example, if we're inserting an update at timestamp 0 with value ddd:
     * btree key ts counter value stop_ts
     * 2     foo 5  0       aaa    6
     * 2     foo 6  0       bbb    7
     * 2     foo 7  0       ccc    8
     *
     * We want to end up with this:
     * btree key ts counter value stop_ts
     * 2     foo 0  0       aaa    0
     * 2     foo 0  1       bbb    0
     * 2     foo 0  2       ccc    0
     * 2     foo 0  3       ddd    0
     *
     * Another example, if we're inserting an update at timestamp 3 with value ddd
     * that is an out of order with a stop timestamp of 6:
     * btree key ts counter value stop_ts
     * 2     foo 1  0       aaa     6
     * 2     foo 6  0       bbb     7
     * 2     foo 7  0       ccc     8
     *
     * We want to end up with this:
     * btree key ts counter value stop_ts
     * 2     foo 1  1       aaa    3
     * 2     foo 3  2       bbb    3
     * 2     foo 3  3       ccc    3
     * 2     foo 3  4       ddd    3
     */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_ts, &hs_counter));
        WT_ASSERT(session, hs_btree_id == btree_id);
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        WT_ASSERT(session, cmp == 0);
#endif
        /*
         * If we got here, we've got out-of-order updates in the history store.
         *
         * Our strategy to rectify this is to remove all records for the same key with a timestamp
         * higher or equal than the specified timestamp and reinsert them at the smaller timestamp,
         * which is the timestamp of the update we are about to insert to the history store.
         *
         * It is possible that the cursor next call can find an update that was reinserted when it
         * had an out of order tombstone with respect to the new update. Continue the search by
         * ignoring them.
         */
        __wt_hs_upd_time_window(hs_cursor, &twp);
        if (hs_ts < ts && twp->stop_ts < ts)
            continue;

        if (reinsert) {
            /*
             * Don't incur the overhead of opening this new cursor unless we need it. In the regular
             * case, we'll never get here.
             */
            if (hs_insert_cursor == NULL)
                WT_ERR(__wt_curhs_open(session, NULL, &hs_insert_cursor));

            /*
             * If these history store records are resolved prepared updates, their durable
             * timestamps will be clobbered by our fix-up process. Keep track of how often this is
             * happening.
             */
            if (hs_cbt->upd_value->tw.start_ts != hs_cbt->upd_value->tw.durable_start_ts ||
              hs_cbt->upd_value->tw.stop_ts != hs_cbt->upd_value->tw.durable_stop_ts)
                ++cache_hs_order_lose_durable_timestamp;

            __wt_verbose(session, WT_VERB_TIMESTAMP,
              "fixing existing out-of-order updates by moving them; start_ts=%s, "
              "durable_start_ts=%s, "
              "stop_ts=%s, durable_stop_ts=%s, new_ts=%s",
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.start_ts, ts_string[0]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_start_ts, ts_string[1]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.stop_ts, ts_string[2]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_stop_ts, ts_string[3]),
              __wt_timestamp_to_string(ts, ts_string[4]));

            /*
             * Use the original start time window's timestamps if it isn't out of order with respect
             * to the new update.
             */
            if (hs_cbt->upd_value->tw.start_ts >= ts)
                hs_insert_tw.start_ts = hs_insert_tw.durable_start_ts = ts - 1;
            else {
                hs_insert_tw.start_ts = hs_cbt->upd_value->tw.start_ts;
                hs_insert_tw.durable_start_ts = hs_cbt->upd_value->tw.durable_start_ts;
            }
            hs_insert_tw.start_txn = hs_cbt->upd_value->tw.start_txn;

            /*
             * We're going to insert something immediately after with the smaller timestamp. Either
             * another moved update OR the update itself triggered the correction. In either case,
             * we should preserve the stop transaction id.
             */
            hs_insert_tw.stop_ts = hs_insert_tw.durable_stop_ts = ts - 1;
            hs_insert_tw.stop_txn = hs_cbt->upd_value->tw.stop_txn;

            /* Extract the underlying value for reinsertion. */
            WT_ERR(hs_cursor->get_value(
              hs_cursor, &tw.durable_stop_ts, &tw.durable_start_ts, &hs_upd_type, &hs_value));

            /* Insert the value back with different timestamps. */
            hs_insert_cursor->set_key(
              hs_insert_cursor, 4, btree_id, &hs_key, hs_insert_tw.start_ts, *counter);
            hs_insert_cursor->set_value(hs_insert_cursor, &hs_insert_tw,
              hs_insert_tw.durable_stop_ts, hs_insert_tw.durable_start_ts, (uint64_t)hs_upd_type,
              &hs_value);
            WT_ERR(hs_insert_cursor->insert(hs_insert_cursor));
            ++(*counter);
            ++cache_hs_order_reinsert;
        }

        /* Delete the out-of-order entry. */
        WT_ERR(hs_cursor->remove(hs_cursor));
        ++cache_hs_order_remove;
    }
    if (ret == WT_NOTFOUND)
        ret = 0;
err:
    if (hs_insert_cursor != NULL)
        hs_insert_cursor->close(hs_insert_cursor);

    WT_STAT_CONN_DATA_INCRV(
      session, cache_hs_order_lose_durable_timestamp, cache_hs_order_lose_durable_timestamp);
    WT_STAT_CONN_DATA_INCRV(session, cache_hs_order_reinsert, cache_hs_order_reinsert);
    WT_STAT_CONN_DATA_INCRV(session, cache_hs_order_remove, cache_hs_order_remove);

    return (ret);
}
