/*-
 * Copyright (c) 2014-2020 MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * When an operation is accessing the history store table, it should ignore the cache size (since
 * the cache is already full), and the operation can't reenter reconciliation.
 */
#define WT_HS_SESSION_FLAGS (WT_SESSION_IGNORE_CACHE_SIZE | WT_SESSION_NO_RECONCILE)

static int __hs_delete_key_from_pos(
  WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id, const WT_ITEM *key);

/*
 * __hs_start_internal_session --
 *     Create a temporary internal session to retrieve history store.
 */
static int
__hs_start_internal_session(WT_SESSION_IMPL *session, WT_SESSION_IMPL **int_sessionp)
{
    WT_ASSERT(session, !F_ISSET(session, WT_CONN_HS_OPEN));
    return (__wt_open_internal_session(S2C(session), "hs_access", true, 0, int_sessionp));
}

/*
 * __hs_release_internal_session --
 *     Release the temporary internal session started to retrieve history store.
 */
static int
__hs_release_internal_session(WT_SESSION_IMPL *int_session)
{
    WT_SESSION *wt_session;

    wt_session = &int_session->iface;
    return (wt_session->close(wt_session, NULL));
}

/*
 * __wt_hs_get_btree --
 *     Get the history store btree. Open a history store cursor if needed to get the btree.
 */
int
__wt_hs_get_btree(WT_SESSION_IMPL *session, WT_BTREE **hs_btreep)
{
    WT_DECL_RET;
    uint32_t session_flags;
    bool is_owner;

    *hs_btreep = NULL;
    session_flags = 0; /* [-Werror=maybe-uninitialized] */

    WT_RET(__wt_hs_cursor(session, &session_flags, &is_owner));

    *hs_btreep = ((WT_CURSOR_BTREE *)session->hs_cursor)->btree;
    WT_ASSERT(session, *hs_btreep != NULL);

    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));

    return (ret);
}

/*
 * __wt_hs_config --
 *     Configure the history store table.
 */
int
__wt_hs_config(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_BTREE *btree;
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *tmp_setup_session;

    conn = S2C(session);
    tmp_setup_session = NULL;

    WT_ERR(__wt_config_gets(session, cfg, "history_store.file_max", &cval));
    if (cval.val != 0 && cval.val < WT_HS_FILE_MIN)
        WT_ERR_MSG(session, EINVAL, "max history store size %" PRId64 " below minimum %d", cval.val,
          WT_HS_FILE_MIN);

    /* TODO: WT-5585 Remove after we switch to using history_store config in MongoDB. */
    if (cval.val == 0) {
        WT_ERR(__wt_config_gets(session, cfg, "cache_overflow.file_max", &cval));
        if (cval.val != 0 && cval.val < WT_HS_FILE_MIN)
            WT_ERR_MSG(session, EINVAL, "max history store size %" PRId64 " below minimum %d",
              cval.val, WT_HS_FILE_MIN);
    }

    /* in-memory or readonly configurations do not have a history store. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    WT_ERR(__hs_start_internal_session(session, &tmp_setup_session));

    /*
     * Retrieve the btree from the history store cursor.
     */
    WT_ERR(__wt_hs_get_btree(tmp_setup_session, &btree));

    /* Track the history store file ID. */
    if (conn->cache->hs_fileid == 0)
        conn->cache->hs_fileid = btree->id;

    /*
     * Set special flags for the history store table: the history store flag (used, for example, to
     * avoid writing records during reconciliation), also turn off checkpoints and logging.
     *
     * Test flags before setting them so updates can't race in subsequent opens (the first update is
     * safe because it's single-threaded from wiredtiger_open).
     */
    if (!F_ISSET(btree, WT_BTREE_HS))
        F_SET(btree, WT_BTREE_HS);
    if (!F_ISSET(btree, WT_BTREE_NO_LOGGING))
        F_SET(btree, WT_BTREE_NO_LOGGING);

    /*
     * We need to set file_max on the btree associated with one of the history store sessions.
     */
    btree->file_max = (uint64_t)cval.val;
    WT_STAT_CONN_SET(session, cache_hs_ondisk_max, btree->file_max);

err:
    if (tmp_setup_session != NULL)
        WT_TRET(__hs_release_internal_session(tmp_setup_session));
    return (ret);
}

/*
 * __wt_hs_create --
 *     Initialize the database's history store.
 */
int
__wt_hs_create(WT_SESSION_IMPL *session, const char **cfg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    const char *drop_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_drop), "force=true", NULL};

    conn = S2C(session);

    /* Read-only and in-memory configurations don't need the history store table. */
    if (F_ISSET(conn, WT_CONN_IN_MEMORY | WT_CONN_READONLY))
        return (0);

    /* The LAS table may exist on upgrade. Discard it. */
    WT_WITH_SCHEMA_LOCK(
      session, ret = __wt_schema_drop(session, "file:WiredTigerLAS.wt", drop_cfg));
    WT_RET(ret);

    /* Re-create the table. */
    WT_RET(__wt_session_create(session, WT_HS_URI, WT_HS_CONFIG));

    WT_RET(__wt_hs_config(session, cfg));

    /* The statistics server is already running, make sure we don't race. */
    WT_WRITE_BARRIER();
    F_SET(conn, WT_CONN_HS_OPEN);

    return (0);
}

/*
 * __wt_hs_destroy --
 *     Destroy the database's history store.
 */
void
__wt_hs_destroy(WT_SESSION_IMPL *session)
{
    F_CLR(S2C(session), WT_CONN_HS_OPEN);
}

/*
 * __wt_hs_cursor_open --
 *     Open a new history store table cursor.
 */
int
__wt_hs_cursor_open(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *open_cursor_cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

    WT_WITHOUT_DHANDLE(
      session, ret = __wt_open_cursor(session, WT_HS_URI, NULL, open_cursor_cfg, &cursor));
    WT_RET(ret);

    session->hs_cursor = cursor;
    F_SET(session, WT_SESSION_HS_CURSOR);

    return (0);
}

/*
 * __wt_hs_cursor --
 *     Return a history store cursor, open one if not already open.
 */
int
__wt_hs_cursor(WT_SESSION_IMPL *session, uint32_t *session_flags, bool *is_owner)
{
    /* We should never reach here if working in context of the default session. */
    WT_ASSERT(session, S2C(session)->default_session != session);

    /*
     * We don't want to get tapped for eviction after we start using the history store cursor; save
     * a copy of the current eviction state, we'll turn eviction off before we return.
     *
     * Don't cache history store table pages, we're here because of eviction problems and there's no
     * reason to believe history store pages will be useful more than once.
     */
    *session_flags = F_MASK(session, WT_HS_SESSION_FLAGS);
    *is_owner = false;

    /* Open a cursor if this session doesn't already have one. */
    if (!F_ISSET(session, WT_SESSION_HS_CURSOR)) {
        /* The caller is responsible for closing this cursor. */
        *is_owner = true;
        WT_RET(__wt_hs_cursor_open(session));
    }

    WT_ASSERT(session, session->hs_cursor != NULL);

    /* Configure session to access the history store table. */
    F_SET(session, WT_HS_SESSION_FLAGS);

    return (0);
}

/*
 * __wt_hs_cursor_close --
 *     Discard a history store cursor.
 */
int
__wt_hs_cursor_close(WT_SESSION_IMPL *session, uint32_t session_flags, bool is_owner)
{
    /* Nothing to do if the session doesn't have a HS cursor opened. */
    if (!F_ISSET(session, WT_SESSION_HS_CURSOR)) {
        WT_ASSERT(session, session->hs_cursor == NULL);
        return (0);
    }
    WT_ASSERT(session, session->hs_cursor != NULL);

    /*
     * If we're not the owner, we're not responsible for closing this cursor. Reset the cursor to
     * avoid pinning the page in cache.
     */
    if (!is_owner)
        return (session->hs_cursor->reset(session->hs_cursor));

    /*
     * We turned off caching and eviction while the history store cursor was in use, restore the
     * session's flags.
     */
    F_CLR(session, WT_HS_SESSION_FLAGS);
    F_SET(session, session_flags);

    WT_RET(session->hs_cursor->close(session->hs_cursor));
    session->hs_cursor = NULL;
    F_CLR(session, WT_SESSION_HS_CURSOR);

    return (0);
}

/*
 * __wt_hs_modify --
 *     Make an update to the history store.
 *
 * History store updates don't use transactions as those updates should be immediately visible and
 *     don't follow normal transaction semantics. For this reason, history store updates are
 *     directly modified using the low level api instead of the ordinary cursor api.
 */
int
__wt_hs_modify(WT_CURSOR_BTREE *hs_cbt, WT_UPDATE *hs_upd)
{
    WT_DECL_RET;
    WT_PAGE_MODIFY *mod;
    WT_SESSION_IMPL *session;
    WT_UPDATE *last_upd;

    session = (WT_SESSION_IMPL *)hs_cbt->iface.session;

    /* If there are existing updates, append them after the new updates. */
    if (hs_cbt->compare == 0) {
        for (last_upd = hs_upd; last_upd->next != NULL; last_upd = last_upd->next)
            ;
        if (hs_cbt->ins != NULL)
            last_upd->next = hs_cbt->ins->upd;
        else if ((mod = hs_cbt->ref->page->modify) != NULL && mod->mod_row_update != NULL)
            last_upd->next = mod->mod_row_update[hs_cbt->slot];
    }

    WT_WITH_BTREE(session, hs_cbt->btree,
      ret = __wt_row_modify(hs_cbt, &hs_cbt->iface.key, NULL, hs_upd, WT_UPDATE_INVALID, true));
    return (ret);
}

/*
 * __hs_insert_updates_verbose --
 *     Display a verbose message once per checkpoint with details about the cache state when
 *     performing a history store table write.
 */
static void
__hs_insert_updates_verbose(WT_SESSION_IMPL *session, WT_BTREE *btree)
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
          ". "
          "Current history store file size: %" PRId64
          ", "
          "cache dirty: %2.3f%% , "
          "cache use: %2.3f%%",
          btree_id, WT_STAT_READ(conn->stats, cache_hs_ondisk), pct_dirty, pct_full);
    }

    /* Never skip updating the tracked generation */
    if (WT_VERBOSE_ISSET(session, WT_VERB_HS))
        cache->hs_verb_gen_write = ckpt_gen_current;
}

/*
 * __hs_insert_record_with_btree_int --
 *     Internal helper for inserting history store records.
 */
static int
__hs_insert_record_with_btree_int(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_BTREE *btree,
  const WT_ITEM *key, const WT_UPDATE *upd, const uint8_t type, const WT_ITEM *hs_value,
  WT_TIME_PAIR stop_ts_pair)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;
    WT_UPDATE *hs_upd;
    size_t notused;
    uint32_t session_flags;

    cbt = (WT_CURSOR_BTREE *)cursor;
    hs_upd = NULL;

    /*
     * Use WT_CURSOR.set_key and WT_CURSOR.set_value to create key and value items, then use them to
     * create an update chain for a direct insertion onto the history store page.
     */
    cursor->set_key(
      cursor, btree->id, key, upd->start_ts, __wt_atomic_add64(&btree->hs_counter, 1));
    cursor->set_value(cursor, stop_ts_pair.timestamp, upd->durable_ts, (uint64_t)type, hs_value);

    /*
     * Insert a delete record to represent stop time pair for the actual record to be inserted. Set
     * the stop time pair as the commit time pair of the history store delete record.
     */
    WT_ERR(__wt_update_alloc(session, NULL, &hs_upd, &notused, WT_UPDATE_TOMBSTONE));
    hs_upd->start_ts = stop_ts_pair.timestamp;
    hs_upd->durable_ts = stop_ts_pair.timestamp;
    hs_upd->txnid = stop_ts_pair.txnid;

    /*
     * Append to the delete record, the actual record to be inserted into the history store. Set the
     * current update start time pair as the commit time pair to the history store record.
     */
    WT_ERR(__wt_update_alloc(session, &cursor->value, &hs_upd->next, &notused, WT_UPDATE_STANDARD));
    hs_upd->next->start_ts = upd->start_ts;
    hs_upd->next->durable_ts = upd->durable_ts;
    hs_upd->next->txnid = upd->txnid;

    /*
     * Search the page and insert the updates. We expect there will be no existing data: assert that
     * we don't find a matching key.
     */
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, &cursor->key, true, NULL, false, NULL));
    WT_ERR(ret);
    WT_ERR(__wt_hs_modify(cbt, hs_upd));

    /*
     * Since the two updates (tombstone and the standard) will reconcile into a single entry, we are
     * incrementing the history store insert statistic by one.
     */
    WT_STAT_CONN_INCR(session, cache_hs_insert);

err:
    if (ret != 0)
        __wt_free_update_list(session, &hs_upd);
    /*
     * If we inserted an update with no timestamp, we need to delete all history records for that
     * key that are further in the history table than us (the key is lexicographically greater). For
     * timestamped tables that are occasionally getting a non-timestamped update, that means that
     * all timestamped updates should get removed. In the case of non-timestamped tables, that means
     * that all updates with higher transaction ids will get removed (which could happen at some
     * more relaxed isolation levels).
     */
    if (ret == 0 && upd->start_ts == WT_TS_NONE) {
#ifdef HAVE_DIAGNOSTIC
        /*
         * We need to initialize the last searched key so that we can do key comparisons when we
         * begin iterating over the history store. This needs to be done otherwise the subsequent
         * "next" calls will blow up.
         */
        WT_TRET(__wt_cursor_key_order_init(cbt));
#endif
        session_flags = session->flags;
        F_SET(session, WT_SESSION_IGNORE_HS_TOMBSTONE);
        /* We're pointing at the newly inserted update. Iterate once more to avoid deleting it. */
        ret = cursor->next(cursor);
        if (ret == WT_NOTFOUND)
            ret = 0;
        else if (ret == 0) {
            WT_TRET(__hs_delete_key_from_pos(session, cursor, btree->id, key));
            WT_STAT_CONN_INCR(session, cache_hs_key_truncate_mix_ts);
        }
        if (!FLD_ISSET(session_flags, WT_SESSION_IGNORE_HS_TOMBSTONE))
            F_CLR(session, WT_SESSION_IGNORE_HS_TOMBSTONE);
    }
    /* We did a row search, release the cursor so that the page doesn't continue being held. */
    cursor->reset(cursor);

    return (ret);
}

/*
 * __hs_insert_record_with_btree --
 *     A helper function to insert the record into the history store including stop time pair.
 *     Should be called with session's btree switched to the history store.
 */
static int
__hs_insert_record_with_btree(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_BTREE *btree,
  const WT_ITEM *key, const WT_UPDATE *upd, const uint8_t type, const WT_ITEM *hs_value,
  WT_TIME_PAIR stop_ts_pair)
{
    WT_DECL_RET;

    /*
     * The session should be pointing at the history store btree since this is the one that we'll be
     * inserting into. The btree parameter that we're passing in should is the btree that the
     * history store content is associated with (this is where the btree id part of the history
     * store key comes from).
     */
    WT_ASSERT(session, WT_IS_HS(S2BT(session)));
    WT_ASSERT(session, !WT_IS_HS(btree));

    /*
     * Disable bulk loads into history store. This would normally occur when updating a record with
     * a cursor however the history store doesn't use cursor update, so we do it here.
     */
    __wt_cursor_disable_bulk(session);

    /*
     * Only deltas or full updates should be written to the history store. More specifically, we
     * should NOT be writing tombstone records in the history store table.
     */
    WT_ASSERT(session, type == WT_UPDATE_STANDARD || type == WT_UPDATE_MODIFY);

    /*
     * If the time pairs are out of order (which can happen if the application performs updates with
     * out-of-order timestamps), so this value can never be seen, don't bother inserting it.
     */
    if (stop_ts_pair.timestamp < upd->start_ts ||
      (stop_ts_pair.timestamp == upd->start_ts && stop_ts_pair.txnid <= upd->txnid)) {
        char ts_string[2][WT_TS_INT_STRING_SIZE];
        __wt_verbose(session, WT_VERB_TIMESTAMP,
          "Warning: fixing out-of-order timestamps %s earlier than previous update %s",
          __wt_timestamp_to_string(stop_ts_pair.timestamp, ts_string[0]),
          __wt_timestamp_to_string(upd->start_ts, ts_string[1]));
        return (0);
    }

    /* The tree structure can change while we try to insert the mod list, retry if that happens. */
    while ((ret = __hs_insert_record_with_btree_int(
              session, cursor, btree, key, upd, type, hs_value, stop_ts_pair)) == WT_RESTART)
        ;

    return (ret);
}

/*
 * __hs_insert_record --
 *     Temporarily switches to history store btree and calls the helper routine to insert records.
 */
static int
__hs_insert_record(WT_SESSION_IMPL *session, WT_CURSOR *cursor, WT_BTREE *btree, const WT_ITEM *key,
  const WT_UPDATE *upd, const uint8_t type, const WT_ITEM *hs_value, WT_TIME_PAIR stop_ts_pair)
{
    WT_CURSOR_BTREE *cbt;
    WT_DECL_RET;

    cbt = (WT_CURSOR_BTREE *)cursor;
    WT_WITH_BTREE(session, cbt->btree, ret = __hs_insert_record_with_btree(session, cursor, btree,
                                         key, upd, type, hs_value, stop_ts_pair));
    return (ret);
}

/*
 * __hs_calculate_full_value --
 *     Calculate the full value of an update.
 */
static inline int
__hs_calculate_full_value(WT_SESSION_IMPL *session, WT_ITEM *full_value, WT_UPDATE *upd,
  const void *base_full_value, size_t size)
{
    if (upd->type == WT_UPDATE_MODIFY) {
        WT_RET(__wt_buf_set(session, full_value, base_full_value, size));
        WT_RET(__wt_modify_apply_item(session, full_value, upd->data, false));
    } else {
        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
        full_value->data = upd->data;
        full_value->size = upd->size;
    }

    return (0);
}

/*
 * __wt_hs_insert_updates --
 *     Copy one set of saved updates into the database's history store table.
 */
int
__wt_hs_insert_updates(WT_CURSOR *cursor, WT_BTREE *btree, WT_PAGE *page, WT_MULTI *multi)
{
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
    WT_SESSION_IMPL *session;
    WT_UPDATE *prev_upd, *upd;
    WT_TIME_PAIR stop_ts_pair;
    wt_off_t hs_size;
    uint64_t insert_cnt, max_hs_size;
    uint32_t i;
    uint8_t *p;
    int nentries;
    bool squashed;

    prev_upd = NULL;
    session = (WT_SESSION_IMPL *)cursor->session;
    insert_cnt = 0;
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

        /* onpage_upd now is always from the update chain */
        WT_ASSERT(session, !F_ISSET(list->onpage_upd, WT_UPDATE_RESTORED_FROM_DISK));

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

        /*
         * Trim any updates before writing to history store. This saves wasted work.
         */
        WT_WITH_BTREE(
          session, btree, upd = __wt_update_obsolete_check(session, page, list->onpage_upd, true));
        __wt_free_update_list(session, &upd);
        upd = list->onpage_upd;

        /*
         * The algorithm assumes the oldest update on the update chain in memory is either a full
         * update or a tombstone.
         *
         * This is guaranteed by __wt_rec_upd_select appends the original onpage value at the end of
         * the chain. It also assumes the onpage_upd selected cannot be a TOMBSTONE and the update
         * newer than a TOMBSTONE must be a full update.
         *
         * The algorithm walks from the oldest update, or the most recently inserted into history
         * store update. To the newest update and build full updates along the way. It sets the stop
         * time pair of the update to the start time pair of the next update, squashes the updates
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
         * We write the stop time pair of M with the start time pair of the tombstone and skip the
         * tombstone.
         * 4) We have a single tombstone on the chain, it is simply ignored.
         */
        for (; upd != NULL; upd = upd->next) {
            if (upd->txnid == WT_TXN_ABORTED)
                continue;
            WT_ERR(__wt_modify_vector_push(&modifies, upd));
            /*
             * If we've reached a full update and its in the history store we don't need to continue
             * as anything beyond this point won't help with calculating deltas.
             */
            if (upd->type == WT_UPDATE_STANDARD && F_ISSET(upd, WT_UPDATE_HS))
                break;
        }

        upd = NULL;

        /* Construct the oldest full update. */
        WT_ASSERT(session, modifies.size > 0);
        __wt_modify_vector_pop(&modifies, &upd);

        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD || upd->type == WT_UPDATE_TOMBSTONE);
        /* Skip TOMBSTONE at the end of the update chain. */
        if (upd->type == WT_UPDATE_TOMBSTONE) {
            if (modifies.size > 0) {
                if (upd->start_ts == WT_TS_NONE) {
                    WT_ERR(__wt_hs_delete_key(session, btree->id, key));
                    WT_STAT_CONN_INCR(session, cache_hs_key_truncate_mix_ts);
                }
                __wt_modify_vector_pop(&modifies, &upd);
            } else
                continue;
        }

        WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD);
        full_value->data = upd->data;
        full_value->size = upd->size;

        squashed = false;

        /*
         * Flush the updates on stack. Stopping once we run out or we reach the onpage upd start
         * time pair, we can squash modifies with the same start time pair as the onpage upd away.
         */
        for (; modifies.size > 0 &&
             !(upd->txnid == list->onpage_upd->txnid &&
                 upd->start_ts == list->onpage_upd->start_ts);
             tmp = full_value, full_value = prev_full_value, prev_full_value = tmp,
             upd = prev_upd) {
            WT_ASSERT(session, upd->type == WT_UPDATE_STANDARD || upd->type == WT_UPDATE_MODIFY);

            __wt_modify_vector_pop(&modifies, &prev_upd);

            /*
             * Set the stop timestamp from durable timestamp instead of commit timestamp. The
             * Garbage collection of history store removes the history values once the stop
             * timestamp is globally visible. i.e. durable timestamp of data store version.
             */
            WT_ASSERT(session, prev_upd->start_ts <= prev_upd->durable_ts);
            stop_ts_pair.timestamp = prev_upd->durable_ts;
            stop_ts_pair.txnid = prev_upd->txnid;

            if (prev_upd->type == WT_UPDATE_TOMBSTONE) {
                WT_ASSERT(session, modifies.size > 0);
                if (prev_upd->start_ts == WT_TS_NONE) {
                    WT_ERR(__wt_hs_delete_key(session, btree->id, key));
                    WT_STAT_CONN_INCR(session, cache_hs_key_truncate_mix_ts);
                }
                __wt_modify_vector_pop(&modifies, &prev_upd);
                WT_ASSERT(session, prev_upd->type == WT_UPDATE_STANDARD);
                prev_full_value->data = prev_upd->data;
                prev_full_value->size = prev_upd->size;
            } else
                WT_ERR(__hs_calculate_full_value(
                  session, prev_full_value, prev_upd, full_value->data, full_value->size));

            /*
             * Skip the updates have the same start timestamp and transaction id
             *
             * Modifies that have the same start time pair as the onpage_upd can be squashed away.
             */
            if (upd->start_ts != prev_upd->start_ts || upd->txnid != prev_upd->txnid) {
                /*
                 * Calculate reverse delta. Insert full update for the newest historical record even
                 * it's a MODIFY.
                 *
                 * It is not correct to check prev_upd == list->onpage_upd as we may have aborted
                 * updates in the middle.
                 */
                nentries = MAX_REVERSE_MODIFY_NUM;
                if (!F_ISSET(upd, WT_UPDATE_HS)) {
                    if (upd->type == WT_UPDATE_MODIFY &&
                      __wt_calc_modify(session, prev_full_value, full_value,
                        prev_full_value->size / 10, entries, &nentries) == 0) {
                        WT_ERR(__wt_modify_pack(cursor, entries, nentries, &modify_value));
                        WT_ERR(__hs_insert_record(session, cursor, btree, key, upd,
                          WT_UPDATE_MODIFY, modify_value, stop_ts_pair));
                        __wt_scr_free(session, &modify_value);
                    } else
                        WT_ERR(__hs_insert_record(session, cursor, btree, key, upd,
                          WT_UPDATE_STANDARD, full_value, stop_ts_pair));

                    /* Flag the update as now in the history store. */
                    F_SET(upd, WT_UPDATE_HS);
                    ++insert_cnt;
                    if (squashed) {
                        WT_STAT_CONN_INCR(session, cache_hs_write_squash);
                        squashed = false;
                    }
                }
            } else
                squashed = true;
        }

        if (modifies.size > 0)
            WT_STAT_CONN_INCR(session, cache_hs_write_squash);
    }

    WT_ERR(__wt_block_manager_named_size(session, WT_HS_FILE, &hs_size));
    WT_STAT_CONN_SET(session, cache_hs_ondisk, hs_size);
    max_hs_size = ((WT_CURSOR_BTREE *)cursor)->btree->file_max;
    if (max_hs_size != 0 && (uint64_t)hs_size > max_hs_size)
        WT_PANIC_ERR(session, WT_PANIC, "WiredTigerHS: file size of %" PRIu64
                                        " exceeds maximum "
                                        "size %" PRIu64,
          (uint64_t)hs_size, max_hs_size);

err:
    if (ret == 0 && insert_cnt > 0)
        __hs_insert_updates_verbose(session, btree);

    __wt_scr_free(session, &key);
    /* modify_value is allocated in __wt_modify_pack. Free it if it is allocated. */
    if (modify_value != NULL)
        __wt_scr_free(session, &modify_value);
    __wt_modify_vector_free(&modifies);
    __wt_scr_free(session, &full_value);
    __wt_scr_free(session, &prev_full_value);
    return (ret);
}

/*
 * __wt_hs_cursor_position --
 *     Position a history store cursor at the end of a set of updates for a given btree id, record
 *     key and timestamp. There may be no history store entries for the given btree id and record
 *     key if they have been removed by WT_CONNECTION::rollback_to_stable.
 */
int
__wt_hs_cursor_position(WT_SESSION_IMPL *session, WT_CURSOR *cursor, uint32_t btree_id,
  WT_ITEM *key, wt_timestamp_t timestamp)
{
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    int cmp, exact;

    WT_RET(__wt_scr_alloc(session, 0, &srch_key));

    /*
     * Because of the special visibility rules for the history store, a new key can appear in
     * between our search and the set of updates that we're interested in. Keep trying until we find
     * it.
     *
     * There may be no history store entries for the given btree id and record key if they have been
     * removed by WT_CONNECTION::rollback_to_stable.
     *
     * Note that we need to compare the raw key off the cursor to determine where we are in the
     * history store as opposed to comparing the embedded data store key since the ordering is not
     * guaranteed to be the same.
     *
     * FIXME: We should be repeatedly moving the cursor backwards within the loop instead of doing a
     * search near operation each time as it is cheaper.
     */
    cursor->set_key(
      cursor, btree_id, key, timestamp != WT_TS_NONE ? timestamp : WT_TS_MAX, UINT64_MAX);
    /* Copy the raw key before searching as a basis for comparison. */
    WT_ERR(__wt_buf_set(session, srch_key, cursor->key.data, cursor->key.size));
    WT_ERR(cursor->search_near(cursor, &exact));
    if (exact > 0) {
        /*
         * It's possible that we may race with a history store insert for another key. So we may be
         * more than one record away the end of our target key/timestamp range. Keep iterating
         * backwards until we land on our key.
         */
        while ((ret = cursor->prev(cursor)) == 0) {
            WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
            if (cmp <= 0)
                break;
        }
    }
#ifdef HAVE_DIAGNOSTIC
    if (ret == 0) {
        WT_ERR(__wt_compare(session, NULL, &cursor->key, srch_key, &cmp));
        WT_ASSERT(session, cmp <= 0);
    }
#endif
err:
    __wt_scr_free(session, &srch_key);
    return (ret);
}

/*
 * __hs_restore_read_timestamp --
 *     Reset the currently running transaction's read timestamp with the original read timestamp.
 */
static void
__hs_restore_read_timestamp(WT_SESSION_IMPL *session)
{
    WT_TXN_SHARED *txn_shared;

    txn_shared = WT_SESSION_TXN_SHARED(session);
    session->txn->read_timestamp = txn_shared->pinned_read_timestamp;
}

/*
 * __wt_find_hs_upd --
 *     Scan the history store for a record the btree cursor wants to position on. Create an update
 *     for the record and return to the caller. The caller may choose to optionally allow prepared
 *     updates to be returned regardless of whether prepare is being ignored globally. Otherwise, a
 *     prepare conflict will be returned upon reading a prepared update.
 */
int
__wt_find_hs_upd(WT_SESSION_IMPL *session, WT_ITEM *key, uint64_t recno, WT_UPDATE **updp,
  bool allow_prepare, WT_ITEM *on_disk_buf)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(hs_key);
    WT_DECL_ITEM(hs_value);
    WT_DECL_ITEM(orig_hs_value_buf);
    WT_DECL_RET;
    WT_ITEM recno_key;
    WT_MODIFY_VECTOR modifies;
    WT_TXN *txn;
    WT_UPDATE *mod_upd, *upd;
    wt_timestamp_t durable_timestamp, durable_timestamp_tmp, hs_start_ts, hs_start_ts_tmp;
    wt_timestamp_t hs_stop_ts, hs_stop_ts_tmp, read_timestamp;
    size_t notused, size;
    uint64_t hs_counter, hs_counter_tmp, upd_type_full;
    uint32_t hs_btree_id, session_flags;
    uint8_t *p, recno_key_buf[WT_INTPACK64_MAXSIZE], upd_type;
    int cmp;
    bool is_owner, modify;

    *updp = NULL;

    hs_cursor = NULL;
    mod_upd = upd = NULL;
    orig_hs_value_buf = NULL;
    __wt_modify_vector_init(session, &modifies);
    txn = session->txn;
    notused = size = 0;
    hs_btree_id = S2BT(session)->id;
    session_flags = 0; /* [-Werror=maybe-uninitialized] */
    WT_NOT_READ(modify, false);
    is_owner = false;

    /*
     * We temporarily move the read timestamp forwards to read modify records in the history store.
     * Outside of that window, it should always be equal to the original read timestamp.
     */
    WT_ASSERT(
      session, txn->read_timestamp == WT_SESSION_TXN_SHARED(session)->pinned_read_timestamp);

    /* Row-store key is as passed to us, create the column-store key as needed. */
    WT_ASSERT(
      session, (key == NULL && recno != WT_RECNO_OOB) || (key != NULL && recno == WT_RECNO_OOB));
    if (key == NULL) {
        p = recno_key_buf;
        WT_RET(__wt_vpack_uint(&p, 0, recno));
        memset(&recno_key, 0, sizeof(recno_key));
        key = &recno_key;
        key->data = recno_key_buf;
        key->size = WT_PTRDIFF(p, recno_key_buf);
    }

    /* Allocate buffers for the history store key/value. */
    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));
    WT_ERR(__wt_scr_alloc(session, 0, &hs_value));

    /* Open a history store table cursor. */
    WT_ERR(__wt_hs_cursor(session, &session_flags, &is_owner));
    hs_cursor = session->hs_cursor;

    /*
     * After positioning our cursor, we're stepping backwards to find the correct update. Since the
     * timestamp is part of the key, our cursor needs to go from the newest record (further in the
     * las) to the oldest (earlier in the las) for a given key.
     */
    read_timestamp = allow_prepare ? txn->prepare_timestamp : txn->read_timestamp;
    WT_ERR_NOTFOUND_OK(
      __wt_hs_cursor_position(session, hs_cursor, hs_btree_id, key, read_timestamp), true);
    if (ret == WT_NOTFOUND) {
        ret = 0;
        goto done;
    }
    WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, hs_key, &hs_start_ts, &hs_counter));

    /* Stop before crossing over to the next btree */
    if (hs_btree_id != S2BT(session)->id)
        goto done;

    /*
     * Keys are sorted in an order, skip the ones before the desired key, and bail out if we have
     * crossed over the desired key and not found the record we are looking for.
     */
    WT_ERR(__wt_compare(session, NULL, hs_key, key, &cmp));
    if (cmp != 0)
        goto done;

    WT_ERR(
      hs_cursor->get_value(hs_cursor, &hs_stop_ts, &durable_timestamp, &upd_type_full, hs_value));
    upd_type = (uint8_t)upd_type_full;

    /* We do not have tombstones in the history store anymore. */
    WT_ASSERT(session, upd_type != WT_UPDATE_TOMBSTONE);

    /*
     * Keep walking until we get a non-modify update. Once we get to that point, squash the updates
     * together.
     */
    if (upd_type == WT_UPDATE_MODIFY) {
        WT_NOT_READ(modify, true);
        /* Store this so that we don't have to make a special case for the first modify. */
        hs_stop_ts_tmp = hs_stop_ts;
        while (upd_type == WT_UPDATE_MODIFY) {
            WT_ERR(__wt_update_alloc(session, hs_value, &mod_upd, &notused, upd_type));
            WT_ERR(__wt_modify_vector_push(&modifies, mod_upd));
            mod_upd = NULL;

            /*
             * Each entry in the lookaside is written with the actual start and stop time pair
             * embedded in the key. In order to traverse a sequence of modifies, we're going to have
             * to manipulate our read timestamp to see records we wouldn't otherwise be able to see.
             *
             * In this case, we want to read the next update in the chain meaning that its start
             * timestamp should be equivalent to the stop timestamp of the record that we're
             * currently on.
             */
            session->txn->read_timestamp = hs_stop_ts_tmp;

            /*
             * Find the base update to apply the reverse deltas. If our cursor next fails to find an
             * update here we fall back to the datastore version. If its timestamp doesn't match our
             * timestamp then we return not found.
             */
            if ((ret = hs_cursor->next(hs_cursor)) == WT_NOTFOUND) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }
            hs_start_ts_tmp = WT_TS_NONE;
            /*
             * Make sure we use the temporary variants of these variables. We need to retain the
             * timestamps of the original modify we saw.
             *
             * We keep looking back into history store until we find a base update to apply the
             * reverse deltas on top of.
             */
            WT_ERR(hs_cursor->get_key(
              hs_cursor, &hs_btree_id, hs_key, &hs_start_ts_tmp, &hs_counter_tmp));

            WT_ERR(__wt_compare(session, NULL, hs_key, key, &cmp));

            if (cmp != 0) {
                /* Fallback to the onpage value as the base value. */
                orig_hs_value_buf = hs_value;
                hs_value = on_disk_buf;
                upd_type = WT_UPDATE_STANDARD;
                break;
            }

            WT_ERR(hs_cursor->get_value(
              hs_cursor, &hs_stop_ts_tmp, &durable_timestamp_tmp, &upd_type_full, hs_value));
            upd_type = (uint8_t)upd_type_full;
        }

        WT_ASSERT(session, upd_type == WT_UPDATE_STANDARD);
        while (modifies.size > 0) {
            __wt_modify_vector_pop(&modifies, &mod_upd);
            WT_ERR(__wt_modify_apply_item(session, hs_value, mod_upd->data, false));
            __wt_free_update_list(session, &mod_upd);
            mod_upd = NULL;
        }
        /* After we're done looping over modifies, reset the read timestamp. */
        __hs_restore_read_timestamp(session);
        WT_STAT_CONN_INCR(session, cache_hs_read_squash);
    }

    /* Allocate an update structure for the record found. */
    WT_ERR(__wt_update_alloc(session, hs_value, &upd, &size, upd_type));
    upd->txnid = WT_TXN_NONE;
    upd->durable_ts = durable_timestamp;
    upd->start_ts = hs_start_ts;
    upd->prepare_state = upd->start_ts == upd->durable_ts ? WT_PREPARE_INIT : WT_PREPARE_RESOLVED;

    /*
     * We're not keeping this in our update list as we want to get rid of it after the read has been
     * dealt with. Mark this update as external and to be discarded when not needed.
     */
    F_SET(upd, WT_UPDATE_RESTORED_FROM_DISK);
    *updp = upd;

done:
err:
    if (orig_hs_value_buf != NULL)
        __wt_scr_free(session, &orig_hs_value_buf);
    else
        __wt_scr_free(session, &hs_value);
    __wt_scr_free(session, &hs_key);

    /*
     * Restore the read timestamp if we encountered an error while processing a modify. There's no
     * harm in doing this multiple times.
     */
    __hs_restore_read_timestamp(session);
    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));

    __wt_free_update_list(session, &mod_upd);
    while (modifies.size > 0) {
        __wt_modify_vector_pop(&modifies, &upd);
        __wt_free_update_list(session, &upd);
    }
    __wt_modify_vector_free(&modifies);

    if (ret == 0) {
        /* Couldn't find a record. */
        if (upd == NULL) {
            ret = WT_NOTFOUND;
            WT_STAT_CONN_INCR(session, cache_hs_read_miss);
        } else {
            WT_STAT_CONN_INCR(session, cache_hs_read);
            WT_STAT_DATA_INCR(session, cache_hs_read);
        }
    }

    WT_ASSERT(session, upd != NULL || ret != 0);

    return (ret);
}

/*
 * __hs_delete_key_int --
 *     Internal helper for deleting history store content for a given key.
 */
static int
__hs_delete_key_int(WT_SESSION_IMPL *session, uint32_t btree_id, const WT_ITEM *key)
{
    WT_CURSOR *hs_cursor;
    WT_DECL_ITEM(srch_key);
    WT_DECL_RET;
    WT_ITEM hs_key;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    int cmp, exact;

    hs_cursor = session->hs_cursor;
    WT_RET(__wt_scr_alloc(session, 0, &srch_key));

    hs_cursor->set_key(hs_cursor, btree_id, key, WT_TS_NONE, (uint64_t)0);
    WT_ERR(__wt_buf_set(session, srch_key, hs_cursor->key.data, hs_cursor->key.size));
    WT_ERR_NOTFOUND_OK(hs_cursor->search_near(hs_cursor, &exact), true);
    /* Empty history store is fine. */
    if (ret == WT_NOTFOUND)
        goto done;
    /*
     * If we raced with a history store insert, we may be two or more records away from our target.
     * Keep iterating forwards until we are on or past our target key.
     *
     * We can't use the cursor positioning helper that we use for regular reads since that will
     * place us at the end of a particular key/timestamp range whereas we want to be placed at the
     * beginning.
     */
    if (exact < 0) {
        while ((ret = hs_cursor->next(hs_cursor)) == 0) {
            WT_ERR(__wt_compare(session, NULL, &hs_cursor->key, srch_key, &cmp));
            if (cmp >= 0)
                break;
        }
        /* No entries greater than or equal to the key we searched for. */
        WT_ERR_NOTFOUND_OK(ret, true);
        if (ret == WT_NOTFOUND)
            goto done;
    }
    /* Bailing out here also means we have no history store records for our key. */
    WT_ERR(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
    if (hs_btree_id != btree_id)
        goto done;
    WT_ERR(__wt_compare(session, NULL, &hs_key, key, &cmp));
    if (cmp != 0)
        goto done;
    WT_ERR(__hs_delete_key_from_pos(session, hs_cursor, btree_id, key));
done:
    ret = 0;
err:
    __wt_scr_free(session, &srch_key);
    return (ret);
}

/*
 * __wt_hs_delete_key --
 *     Delete an entire key's worth of data in the history store.
 */
int
__wt_hs_delete_key(WT_SESSION_IMPL *session, uint32_t btree_id, const WT_ITEM *key)
{
    WT_DECL_RET;
    uint32_t session_flags;
    bool is_owner;

    session_flags = session->flags;

    /*
     * Some code paths such as schema removal involve deleting keys in metadata and assert that we
     * shouldn't be opening new dhandles. We won't ever need to blow away history store content in
     * these cases so let's just return early here.
     */
    if (F_ISSET(session, WT_SESSION_NO_DATA_HANDLES))
        return (0);

    WT_RET(__wt_hs_cursor(session, &session_flags, &is_owner));
    /*
     * In order to delete a key range, we need to be able to inspect all history store records
     * regardless of their stop time pairs.
     */
    F_SET(session, WT_SESSION_IGNORE_HS_TOMBSTONE);
    /* The tree structure can change while we try to insert the mod list, retry if that happens. */
    while ((ret = __hs_delete_key_int(session, btree_id, key)) == WT_RESTART)
        ;

    if (!FLD_ISSET(session_flags, WT_SESSION_IGNORE_HS_TOMBSTONE))
        F_CLR(session, WT_SESSION_IGNORE_HS_TOMBSTONE);
    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));
    return (ret);
}

/*
 * __hs_delete_key_from_pos --
 *     Delete an entire key's worth of data in the history store assuming that the input cursor is
 *     positioned at the beginning of the key range.
 */
static int
__hs_delete_key_from_pos(
  WT_SESSION_IMPL *session, WT_CURSOR *hs_cursor, uint32_t btree_id, const WT_ITEM *key)
{
    WT_CURSOR_BTREE *hs_cbt;
    WT_DECL_RET;
    WT_ITEM hs_key;
    WT_UPDATE *upd;
    wt_timestamp_t hs_start_ts;
    size_t size;
    uint64_t hs_counter;
    uint32_t hs_btree_id;
    int cmp;

    hs_cbt = (WT_CURSOR_BTREE *)hs_cursor;
    upd = NULL;

    /* If there is nothing else in history store, we're done here. */
    for (; ret == 0; ret = hs_cursor->next(hs_cursor)) {
        WT_RET(hs_cursor->get_key(hs_cursor, &hs_btree_id, &hs_key, &hs_start_ts, &hs_counter));
        /*
         * If the btree id or key isn't ours, that means that we've hit the end of the key range and
         * that there is no more history store content for this key.
         */
        if (hs_btree_id != btree_id)
            break;
        WT_RET(__wt_compare(session, NULL, &hs_key, key, &cmp));
        if (cmp != 0)
            break;
        /*
         * Since we're using internal functions to modify the row structure, we need to manually set
         * the comparison to an exact match.
         */
        hs_cbt->compare = 0;
        /*
         * Append a globally visible tombstone to the update list. This will effectively make the
         * value invisible and the key itself will eventually get removed during reconciliation.
         */
        WT_RET(__wt_update_alloc(session, NULL, &upd, &size, WT_UPDATE_TOMBSTONE));
        upd->txnid = WT_TXN_NONE;
        upd->start_ts = upd->durable_ts = WT_TS_NONE;
        WT_ERR(__wt_hs_modify(hs_cbt, upd));
        upd = NULL;
        WT_STAT_CONN_INCR(session, cache_hs_remove_key_truncate);
    }
    if (ret == WT_NOTFOUND)
        return (0);
err:
    __wt_free(session, upd);
    return (ret);
}

/*
 * __wt_verify_history_store_tree --
 *     Verify the history store. There can't be an entry in the history store without having the
 *     latest value for the respective key in the data store. If given a uri, limit the verification
 *     to the corresponding btree.
 */
int
__wt_verify_history_store_tree(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *cursor, *data_cursor;
    WT_DECL_ITEM(hs_key);
    WT_DECL_ITEM(prev_hs_key);
    WT_DECL_RET;
    wt_timestamp_t hs_start_ts;
    uint64_t hs_counter;
    uint32_t btree_id, btree_id_given_uri, session_flags, prev_btree_id;
    int exact, cmp;
    char *uri_itr;
    bool is_owner;

    cursor = data_cursor = NULL;
    btree_id_given_uri = 0; /* [-Wconditional-uninitialized] */
    session_flags = 0;      /* [-Wconditional-uninitialized] */
    prev_btree_id = 0;      /* [-Wconditional-uninitialized] */
    uri_itr = NULL;
    is_owner = false; /* [-Wconditional-uninitialized] */

    WT_ERR(__wt_scr_alloc(session, 0, &hs_key));
    WT_ERR(__wt_scr_alloc(session, 0, &prev_hs_key));

    WT_ERR(__wt_hs_cursor(session, &session_flags, &is_owner));
    cursor = session->hs_cursor;

    /*
     * If a uri has been provided, limit verification to the corresponding btree by jumping to the
     * first record for that btree in the history store. Otherwise scan the whole history store.
     */
    if (uri != NULL) {
        ret = __wt_metadata_uri_to_btree_id(session, uri, &btree_id_given_uri);
        if (ret != 0)
            WT_ERR_MSG(session, ret, "Unable to locate the URI %s in the metadata file", uri);

        /*
         * Position the cursor at the first record of the specified btree, or one after. It is
         * possible there are no records in the history store for this btree.
         */
        cursor->set_key(cursor, btree_id_given_uri, hs_key, 0, 0, 0, 0);
        ret = cursor->search_near(cursor, &exact);
        if (ret == 0 && exact < 0)
            ret = cursor->next(cursor);
    } else
        ret = cursor->next(cursor);

    /* We have the history store cursor positioned at the first record that we want to verify. */
    for (; ret == 0; ret = cursor->next(cursor)) {
        WT_ERR(cursor->get_key(cursor, &btree_id, hs_key, &hs_start_ts, &hs_counter));

        /* When limiting our verification to a uri, bail out if the btree-id doesn't match. */
        if (uri != NULL && btree_id != btree_id_given_uri)
            break;

        /*
         *  Keep track of the previous comparison. The history store is stored in order, so we can
         *  avoid redundant comparisons. Previous btree ID isn't set, until data cursor is open.
         */
        if (data_cursor == NULL || (prev_btree_id != btree_id)) {
            /*
             * Check whether this btree-id exists in the metadata. We do that by finding what uri
             * this btree belongs to. Using this URI, verify the history store key with the data
             * store.
             */
            if (data_cursor != NULL) {
                WT_ERR(data_cursor->close(data_cursor));
                /* Setting data_cursor to null, to avoid double free */
                data_cursor = NULL;
            }
            /*
             * Using the btree-id find the metadata entry and extract the URI for this btree. Don't
             * forget to free the copy of the URI returned.
             *
             * Re-purpose the previous-key buffer on error, safe because we're about to error out.
             */
            __wt_free(session, uri_itr);
            if ((ret = __wt_metadata_btree_id_to_uri(session, btree_id, &uri_itr)) != 0)
                WT_ERR_MSG(session, ret,
                  "Unable to find btree-id %" PRIu32
                  " in the metadata file for the associated history store key %s",
                  btree_id,
                  __wt_buf_set_printable(session, hs_key->data, hs_key->size, prev_hs_key));

            WT_ERR(__wt_open_cursor(session, uri_itr, NULL, NULL, &data_cursor));
            F_SET(data_cursor, WT_CURSOR_RAW_OK);
        } else {
            WT_ERR(__wt_compare(session, NULL, hs_key, prev_hs_key, &cmp));
            if (cmp == 0)
                continue;
        }
        WT_ERR(__wt_buf_set(session, prev_hs_key, hs_key->data, hs_key->size));
        prev_btree_id = btree_id;

        /* Re-purpose the previous-key buffer on error, safe because we're about to error out. */
        data_cursor->set_key(data_cursor, hs_key);
        if ((ret = data_cursor->search(data_cursor)) == WT_NOTFOUND)
            WT_ERR_MSG(session, ret,
              "In %s, the associated history store key %s was not found in the data store", uri_itr,
              __wt_buf_set_printable(session, hs_key->data, hs_key->size, prev_hs_key));
        WT_ERR(ret);
    }
    WT_ERR_NOTFOUND_OK(ret, false);
err:
    if (data_cursor != NULL)
        WT_TRET(data_cursor->close(data_cursor));
    WT_TRET(__wt_hs_cursor_close(session, session_flags, is_owner));

    __wt_scr_free(session, &hs_key);
    __wt_scr_free(session, &prev_hs_key);
    __wt_free(session, uri_itr);
    return (ret);
}
