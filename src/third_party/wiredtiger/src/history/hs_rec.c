/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __hs_delete_reinsert_from_pos(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor,
  uint32_t btree_id, const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool no_ts_tombstone,
  bool error_on_ts_ordering, uint64_t *hs_counter, WT_TIME_WINDOW *upd_tw);

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
  const uint8_t type, const WT_ITEM *hs_value, WT_TIME_WINDOW *tw, bool error_on_ts_ordering)
{
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_ITEM(existing_val);
    WT_DECL_ITEM(hs_key);
    WT_DECL_RET;
    wt_timestamp_t durable_timestamp_diag;
    wt_timestamp_t hs_start_ts;
    wt_timestamp_t hs_stop_durable_ts_diag;
    uint64_t counter, hs_counter;
    uint64_t upd_type_full_diag;
    uint32_t hs_btree_id;
    int cmp;
    bool hs_read_all_flag;

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

        if (EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_HS_VALIDATE)) {
            /* Allocate buffer for the existing history store value for the same key. */
            WT_ERR(__wt_scr_alloc(session, 0, &existing_val));

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
                 * 4. The prepared rollback left the history store entry when checkpoint is in
                 * progress.
                 */
                if (cmp == 0) {
                    if (!__wt_txn_tw_stop_visible_all(session, &hs_cbt->upd_value->tw) &&
                      tw->start_txn != WT_TXN_NONE &&
                      tw->start_txn == hs_cbt->upd_value->tw.start_txn &&
                      tw->start_ts == hs_cbt->upd_value->tw.start_ts &&
                      tw->start_ts != tw->stop_ts) {
                        /*
                         * If we have issues with duplicate history store records, we want to be
                         * able to distinguish between modifies and full updates. Since modifies are
                         * not idempotent, having them inserted multiple times can cause invalid
                         * values to be read.
                         */
                        WT_ASSERT_ALWAYS(session,
                          type != WT_UPDATE_MODIFY &&
                            (uint8_t)upd_type_full_diag != WT_UPDATE_MODIFY,
                          "Duplicate modifies inserted into the history store can result in "
                          "invalid reads");
                    }
                }
                counter = hs_counter + 1;
            }
        } else {
            if (tw->start_ts == hs_start_ts)
                counter = hs_counter + 1;
        }
    }

    /*
     * Look ahead for any higher timestamps. If we find updates, we should remove them and reinsert
     * them at the current timestamp. If there were no keys equal to or less than our target key, we
     * would have received WT_NOTFOUND. In that case we need to search again with a higher
     * timestamp.
     */
    if (ret == 0) {
        /* Check if the current history store update's stop timestamp is less than the update. */
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
     * timestamps ahead of it. An example would be an update without a timestamp getting moved to
     * the history store. This scenario can avoid detection earlier in reconciliation and result in
     * an EBUSY being returned as it detects updates without a timestamp. To prevent this we allow
     * globally visible updates to fix history store content even if eviction is running
     * concurrently with a checkpoint.
     *
     * This is safe because global visibility considers the checkpoint transaction id and timestamp
     * while it is running, i.e. if the update is globally visible to eviction it will be globally
     * visible to checkpoint and the modifications it makes to the history store will be the same as
     * what checkpoint would've done.
     */
    if (error_on_ts_ordering && __wt_txn_tw_start_visible_all(session, tw)) {
        error_on_ts_ordering = false;
    }

    if (ret == 0)
        WT_ERR(__hs_delete_reinsert_from_pos(session, cursor, btree->id, key, tw->start_ts + 1,
          true, false, error_on_ts_ordering, &counter, tw));

#ifdef HAVE_DIAGNOSTIC
    /*
     * We may have fixed the timestamps. Make sure that we haven't accidentally added a duplicate of
     * the key we are about to insert.
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
 * __hs_pack_key --
 *     Pack the history store key
 */
static inline int
__hs_pack_key(WT_SESSION_IMPL *session, WT_BTREE *btree, WT_RECONCILE *r, WT_INSERT *ins,
  WT_ROW *rip, WT_ITEM *key)
{
    WT_DECL_RET;
    uint8_t *p;

    switch (r->page->type) {
    case WT_PAGE_COL_FIX:
    case WT_PAGE_COL_VAR:
        p = key->mem;
        WT_RET(__wt_vpack_uint(&p, 0, WT_INSERT_RECNO(ins)));
        key->size = WT_PTRDIFF(p, key->data);
        break;
    case WT_PAGE_ROW_LEAF:
        if (ins == NULL) {
            WT_WITH_BTREE(
              session, btree, ret = __wt_row_leaf_key(session, r->page, rip, key, false));
            WT_RET(ret);
        } else {
            key->data = WT_INSERT_KEY(ins);
            key->size = WT_INSERT_KEY_SIZE(ins);
        }
        break;
    default:
        WT_RET(__wt_illegal_value(session, r->page->type));
    }

    return (ret);
}

/*
 * __wt_hs_insert_updates --
 *     Copy one set of saved updates into the database's history store table if they haven't been
 *     moved there. Whether the function fails or succeeds, if there is a successful write to
 *     history, cache_write_hs is set to true.
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
    WT_SAVE_UPD *list;
    WT_UPDATE *newest_hs, *no_ts_upd, *oldest_upd, *prev_upd, *ref_upd, *tombstone, *upd;
    WT_TIME_WINDOW tw;
    wt_off_t hs_size;
    uint64_t insert_cnt, max_hs_size, modify_cnt;
    uint64_t cache_hs_insert_full_update, cache_hs_insert_reverse_modify, cache_hs_write_squash;
    uint32_t i;
    int nentries;
    bool enable_reverse_modify, error_on_ts_ordering, hs_inserted, squashed;

    r->cache_write_hs = false;
    btree = S2BT(session);
    prev_upd = NULL;
    WT_TIME_WINDOW_INIT(&tw);
    insert_cnt = 0;
    error_on_ts_ordering = F_ISSET(r, WT_REC_CHECKPOINT_RUNNING);
    cache_hs_insert_full_update = cache_hs_insert_reverse_modify = cache_hs_write_squash = 0;

    WT_RET(__wt_curhs_open(session, NULL, &hs_cursor));
    F_SET(hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    __wt_update_vector_init(session, &updates);

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
        WT_ERR(__hs_pack_key(session, btree, r, list->ins, list->rip, key));

        no_ts_upd = newest_hs = NULL;
        ref_upd = list->onpage_upd;

        __wt_update_vector_clear(&updates);

        /*
         * Reverse deltas are only supported on 'S' and 'u' value formats.
         */
        enable_reverse_modify =
          (WT_STREQ(btree->value_format, "S") || WT_STREQ(btree->value_format, "u"));

        /*
         * If there exists an on page tombstone without a timestamp, consider it as a no timestamp
         * update to clear the timestamps of all the updates that are inserted into the history
         * store.
         */
        if (list->onpage_tombstone != NULL && list->onpage_tombstone->start_ts == WT_TS_NONE)
            no_ts_upd = list->onpage_tombstone;

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
        for (upd = list->onpage_upd, prev_upd = NULL; upd != NULL; upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;

            /* We must have deleted any update left in the history store. */
            WT_ASSERT(session, !F_ISSET(upd, WT_UPDATE_TO_DELETE_FROM_HS));

            /* Detect any update without a timestamp. */
            if (prev_upd != NULL && prev_upd->start_ts < upd->start_ts) {
                WT_ASSERT_ALWAYS(session, prev_upd->start_ts == WT_TS_NONE,
                  "out-of-order timestamp update detected");
                /*
                 * Fail the eviction if we detect any timestamp ordering issue and the error flag is
                 * set. We cannot modify the history store to fix the updates' timestamps as it may
                 * make the history store checkpoint inconsistent.
                 */
                if (error_on_ts_ordering) {
                    ret = EBUSY;
                    WT_STAT_CONN_INCR(session, cache_eviction_fail_checkpoint_no_ts);
                    goto err;
                }

                /*
                 * Always insert full update to the history store if we detect update without a
                 * timestamp.
                 */
                enable_reverse_modify = false;
            }

            WT_ERR(__wt_update_vector_push(&updates, upd));

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

            prev_upd = upd;

            /*
             * No need to continue if we found a first self contained value that is globally
             * visible.
             */
            if (__wt_txn_upd_visible_all(session, upd) && WT_UPDATE_DATA_VALUE(upd))
                break;

            /*
             * If we've reached a full update and it's in the history store we don't need to
             * continue as anything beyond this point won't help with calculating deltas.
             */
            if (upd->type == WT_UPDATE_STANDARD && F_ISSET(upd, WT_UPDATE_HS))
                break;

            /*
             * Save the first update without a timestamp in the update chain. This is used to remove
             * all the following updates' timestamps in the chain.
             */
            if (no_ts_upd == NULL && upd->start_ts == WT_TS_NONE) {
                WT_ASSERT(session, upd->durable_ts == WT_TS_NONE);
                no_ts_upd = upd;
            }
        }

        prev_upd = upd = NULL;

        WT_ASSERT(session, updates.size > 0);
        __wt_update_vector_peek(&updates, &oldest_upd);

        WT_ASSERT(session,
          oldest_upd->type == WT_UPDATE_STANDARD || oldest_upd->type == WT_UPDATE_TOMBSTONE);

        /*
         * Fix the history store record here if the oldest update is a tombstone without a
         * timestamp. This situation is possible only when the tombstone is globally visible. Delete
         * all the updates of the key in the history store with timestamps.
         */
        if (oldest_upd->type == WT_UPDATE_TOMBSTONE && oldest_upd->start_ts == WT_TS_NONE) {
            WT_ERR(
              __wt_hs_delete_key(session, hs_cursor, btree->id, key, false, error_on_ts_ordering));

            WT_STAT_CONN_DATA_INCR(session, cache_hs_key_truncate);

            /* Reset the update without a timestamp if it is the last update in the chain. */
            if (oldest_upd == no_ts_upd)
                no_ts_upd = NULL;
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

            /*
             * Reset the update without a timestamp pointer once all the previous updates are
             * inserted into the history store.
             */
            if (upd == no_ts_upd)
                no_ts_upd = NULL;

            if (no_ts_upd != NULL) {
                tw.durable_start_ts = WT_TS_NONE;
                tw.start_ts = WT_TS_NONE;
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

                if (no_ts_upd != NULL) {
                    tw.durable_stop_ts = WT_TS_NONE;
                    tw.stop_ts = WT_TS_NONE;
                } else {
                    tw.durable_stop_ts = prev_upd->durable_ts;
                    tw.stop_ts = prev_upd->start_ts;
                }
                tw.stop_txn = prev_upd->txnid;

                if (prev_upd->type == WT_UPDATE_TOMBSTONE)
                    tombstone = prev_upd;
            }

            /*
             * Reset the non timestamped update pointer once all the previous updates are inserted
             * into the history store.
             */
            if (prev_upd == no_ts_upd)
                no_ts_upd = NULL;

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
                  modify_value, &tw, error_on_ts_ordering));
                ++cache_hs_insert_reverse_modify;
                __wt_scr_free(session, &modify_value);
                ++modify_cnt;
            } else {
                modify_cnt = 0;
                WT_ERR(__hs_insert_record(session, hs_cursor, btree, key, WT_UPDATE_STANDARD,
                  full_value, &tw, error_on_ts_ordering));
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
 * __wt_hs_delete_key --
 *     Delete history store content of a given key and optionally reinsert them with 0 timestamp.
 */
int
__wt_hs_delete_key(WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id,
  const WT_ITEM *key, bool reinsert, bool error_on_ts_ordering)
{
    WT_DECL_RET;
    WT_ITEM hs_key;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    bool hs_read_all_flag;

    hs_read_all_flag = F_ISSET(hs_cursor, WT_CURSTD_HS_READ_ALL);

    hs_cursor->set_key(hs_cursor, 3, btree_id, key, WT_TS_NONE);
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
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
        ++hs_counter;
    }

    WT_ERR(__hs_delete_reinsert_from_pos(session, hs_cursor, btree_id, key, WT_TS_NONE, reinsert,
      true, error_on_ts_ordering, &hs_counter, NULL));

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
  const WT_ITEM *key, wt_timestamp_t ts, bool reinsert, bool no_ts_tombstone,
  bool error_on_ts_ordering, uint64_t *counter, WT_TIME_WINDOW *upd_tw)
{
    WT_CURSOR *hs_insert_cursor;
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_RET;
    WT_ITEM hs_key, hs_value;
    WT_TIME_WINDOW hs_insert_tw, *twp;
    wt_timestamp_t hs_durable_start_ts, hs_durable_stop_ts, hs_start_ts;
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

    /*
     * If we delete all the updates of the key from the history store, we should not reinsert any
     * update except when a tombstone without a timestamp is not globally visible yet.
     */
    WT_ASSERT(session, no_ts_tombstone || ts > WT_TS_NONE || !reinsert);

    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* Ignore records that are obsolete. */
        __wt_hs_upd_time_window(hs_cursor, &twp);
        if (__wt_txn_tw_stop_visible_all(session, twp))
            continue;

        /*
         * The below example illustrates a case that the data store and the history
         * store may contain the same value. In this case, skip inserting the same
         * value to the history store again.
         *
         * Suppose there is one table table1 and the below operations are performed.
         *
         * 1. Insert a=1 in table1 at timestamp 10
         * 2. Delete a from table1 at timestamp 20
         * 3. Set stable timestamp = 20, oldest timestamp=1
         * 4. Checkpoint table1
         * 5. Insert a=2 in table1 at timestamp 30
         * 6. Evict a=2 from table1 and move the content to history store.
         * 7. Checkpoint is still running and before it finishes checkpointing the history store the
         * above steps 5 and 6 will happen.
         *
         * After all this operations the checkpoint content will be
         * Data store --
         * table1 --> a=1 at start_ts=10, stop_ts=20
         *
         * History store --
         * table1 --> a=1 at start_ts=10, stop_ts=20
         *
         * WiredTiger takes a backup of the checkpoint and use this backup to restore.
         * Note: In table1 of both data store and history store has the same content.
         *
         * Now the backup is used to restore.
         *
         * 1. Insert a=3 in table1
         * 2. Checkpoint started, eviction started and sees the same content in data store and
         * history store while reconciling.
         *
         * The start timestamp and transaction ids are checked to ensure for the global
         * visibility because globally visible timestamps and transaction ids may be cleared to 0.
         * The time window of the inserting record and the history store record are
         * compared to make sure that the same record are not being inserted again.
         */

        if (upd_tw != NULL &&
          (__wt_txn_tw_start_visible_all(session, upd_tw) &&
                __wt_txn_tw_start_visible_all(session, twp) ?
              WT_TIME_WINDOWS_STOP_EQUAL(upd_tw, twp) :
              WT_TIME_WINDOWS_EQUAL(upd_tw, twp)))
            continue;

        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
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
        if (hs_start_ts >= ts || twp->stop_ts >= ts)
            break;
    }
    if (ret == WT_NOTFOUND)
        return (0);
    WT_ERR(ret);

    /*
     * If we find a key with a timestamp larger than or equal to the specified timestamp then the
     * specified timestamp must be mixed mode.
     *
     * FIXME-WT-10017: Change this back to WT_ASSERT_ALWAYS once WT-10017 is resolved. WT-10017 will
     * resolve a known issue where this assert fires for column store configurations.
     */
    WT_ASSERT(session, ts == 1 || ts == WT_TS_NONE);

    /*
     * Fail the eviction if we detect any timestamp ordering issue and the error flag is set. We
     * cannot modify the history store to fix the update's timestamps as it may make the history
     * store checkpoint inconsistent.
     */
    if (error_on_ts_ordering) {
        ret = EBUSY;
        WT_STAT_CONN_INCR(session, cache_eviction_fail_checkpoint_no_ts);
        goto err;
    }

    /*
     * The goal of this function is to move no timestamp content to maintain ordering in the
     * history store. We do this by removing content with higher timestamps and reinserting it
     * without a timestamp (from search's point of view) the newly inserted update. Even though
     * these updates will all have no timestamp, they cannot be discarded since older readers
     * may need to see them after they've been moved due to their transaction id.
     *
     * For example, if we're inserting an update without a timestamp with value ddd:
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
     * Another example, if we're inserting an update without a timestamp with value ddd:
     * btree key ts counter value stop_ts
     * 2     foo 0  0       aaa     6
     * 2     foo 6  0       bbb     7
     * 2     foo 7  0       ccc     8
     *
     * We want to end up with this:
     * btree key ts counter value stop_ts
     * 2     foo 0  1       aaa    0
     * 2     foo 0  2       bbb    0
     * 2     foo 0  3       ccc    0
     * 2     foo 0  4       ddd    0
     */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        /* We shouldn't have crossed the btree and user key search space. */
        WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
        WT_ASSERT(session, hs_btree_id == btree_id);
#ifdef HAVE_DIAGNOSTIC
        WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
        WT_ASSERT(session, cmp == 0);
#endif
        /*
         * If we got here, we've got updates need to be fixed in the history store.
         *
         * Our strategy to rectify this is to remove all records for the same key with a timestamp
         * higher or equal than the specified timestamp and reinsert them at the zero timestamp,
         * which is the timestamp of the update we are about to insert to the history store.
         *
         * It is possible that the cursor next call can find an update that was reinserted when it
         * had a tombstone without a timestamp with respect to the new update. Continue the search
         * by ignoring them.
         */
        __wt_hs_upd_time_window(hs_cursor, &twp);
        if (hs_start_ts < ts && twp->stop_ts < ts)
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
              "fixing existing updates by moving them; start_ts=%s, "
              "durable_start_ts=%s, "
              "stop_ts=%s, durable_stop_ts=%s, new_ts=%s",
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.start_ts, ts_string[0]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_start_ts, ts_string[1]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.stop_ts, ts_string[2]),
              __wt_timestamp_to_string(hs_cbt->upd_value->tw.durable_stop_ts, ts_string[3]),
              __wt_timestamp_to_string(ts, ts_string[4]));

            /*
             * Use the original start time window's timestamps if its timestamp is less than the new
             * update.
             */
            if (hs_cbt->upd_value->tw.start_ts >= ts ||
              hs_cbt->upd_value->tw.durable_start_ts >= ts)
                hs_insert_tw.start_ts = hs_insert_tw.durable_start_ts =
                  no_ts_tombstone ? ts : ts - 1;
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
            hs_insert_tw.stop_ts = hs_insert_tw.durable_stop_ts = no_ts_tombstone ? ts : ts - 1;
            hs_insert_tw.stop_txn = hs_cbt->upd_value->tw.stop_txn;

            /* Extract the underlying value for reinsertion. */
            WT_ERR(hs_cursor->get_value(
              hs_cursor, &hs_durable_stop_ts, &hs_durable_start_ts, &hs_upd_type, &hs_value));

            /* Reinsert the update with corrected timestamps. */
            if (no_ts_tombstone && hs_start_ts == ts)
                *counter = hs_counter;

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

        /* Delete the entry that needs to fix. */
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

/*
 * __hs_delete_record --
 *     Delete an update from the history store if it is not obsolete
 */
static int
__hs_delete_record(
  WT_SESSION_IMPL *session, WT_RECONCILE *r, WT_ITEM *key, WT_UPDATE *upd, WT_UPDATE *tombstone)
{
    WT_DECL_RET;
    bool hs_read_committed;

    WT_TIME_WINDOW *hs_tw;

    if (r->hs_cursor == NULL)
        WT_RET(__wt_curhs_open(session, NULL, &r->hs_cursor));
    hs_read_committed = F_ISSET(r->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    /* Ensure we can see all the content in the history store. */
    F_SET(r->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);

    /* No need to delete from the history store if it is already obsolete. */
    if (tombstone != NULL && __wt_txn_upd_visible_all(session, tombstone))
        goto done;

    r->hs_cursor->set_key(r->hs_cursor, 4, S2BT(session)->id, key, WT_TS_MAX, UINT64_MAX);
    WT_ERR_NOTFOUND_OK(__wt_curhs_search_near_before(session, r->hs_cursor), true);
    /* It's possible the value in the history store becomes obsolete concurrently. */
    if (ret == WT_NOTFOUND) {
        WT_ASSERT(session, tombstone != NULL && __wt_txn_upd_visible_all(session, tombstone));
        ret = 0;
    } else {
        /*
         * If we're deleting a record that is already in the history store this implies we're
         * rolling back a prepared transaction and need to pull the history store update back into
         * the update chain, then delete it from the history store. These checks ensure we've
         * retrieved the correct update from the history store.
         */
        if (EXTRA_DIAGNOSTICS_ENABLED(session, WT_DIAGNOSTIC_HS_VALIDATE)) {
            __wt_hs_upd_time_window(r->hs_cursor, &hs_tw);
            WT_ASSERT_ALWAYS(session,
              hs_tw->start_txn == WT_TXN_NONE || hs_tw->start_txn == upd->txnid,
              "Retrieved wrong update from history store: start txn id mismatch");
            WT_ASSERT_ALWAYS(session,
              hs_tw->start_ts == WT_TS_NONE || hs_tw->start_ts == upd->start_ts,
              "Retrieved wrong update from history store: start timestamp mismatch");
            WT_ASSERT_ALWAYS(session,
              hs_tw->durable_start_ts == WT_TS_NONE || hs_tw->durable_start_ts == upd->durable_ts,
              "Retrieved wrong update from history store: durable start timestamp mismatch");
            if (tombstone != NULL) {
                WT_ASSERT_ALWAYS(session, hs_tw->stop_txn == tombstone->txnid,
                  "Retrieved wrong update from history store: stop txn id mismatch");
                WT_ASSERT_ALWAYS(session, hs_tw->stop_ts == tombstone->start_ts,
                  "Retrieved wrong update from history store: stop timestamp mismatch");
                WT_ASSERT_ALWAYS(session, hs_tw->durable_stop_ts == tombstone->durable_ts,
                  "Retrieved wrong update from history store: durable stop timestamp mismatch");
            } else
                WT_ASSERT_ALWAYS(session, !WT_TIME_WINDOW_HAS_STOP(hs_tw),
                  "Retrieved wrong update from history store: empty tombstone with stop timestamp");
        }
        WT_ERR(r->hs_cursor->remove(r->hs_cursor));
    }
done:
    if (tombstone != NULL)
        F_CLR(tombstone, WT_UPDATE_TO_DELETE_FROM_HS | WT_UPDATE_HS);
    F_CLR(upd, WT_UPDATE_TO_DELETE_FROM_HS | WT_UPDATE_HS);

err:
    if (!hs_read_committed)
        F_CLR(r->hs_cursor, WT_CURSTD_HS_READ_COMMITTED);
    return (ret);
}

/*
 * __wt_hs_delete_updates --
 *     Delete the updates from the history store
 */
int
__wt_hs_delete_updates(WT_SESSION_IMPL *session, WT_RECONCILE *r)
{
    WT_BTREE *btree;
    WT_DECL_ITEM(key);
    WT_DECL_RET;
    WT_DELETE_HS_UPD *delete_hs_upd;
    uint32_t i;

    /* Nothing to delete from the history store. */
    if (r->delete_hs_upd == NULL)
        return (0);

    btree = S2BT(session);

    WT_RET(__wt_scr_alloc(session, WT_INTPACK64_MAXSIZE, &key));

    for (delete_hs_upd = r->delete_hs_upd, i = 0; i < r->delete_hs_upd_next; ++delete_hs_upd, ++i) {
        WT_ERR(__hs_pack_key(session, btree, r, delete_hs_upd->ins, delete_hs_upd->rip, key));
        WT_ERR(__hs_delete_record(session, r, key, delete_hs_upd->upd, delete_hs_upd->tombstone));
    }

err:
    __wt_scr_free(session, &key);
    return (ret);
}
