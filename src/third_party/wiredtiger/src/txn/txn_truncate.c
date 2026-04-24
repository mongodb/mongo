/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * Selects which truncate-list entries __truncate_search considers: those visible to the calling
 * transaction (committed truncates we may need to honor) or those not visible (uncommitted
 * truncates that may conflict with our writes).
 */
typedef enum { WT_TRUNCATE_SEARCH_VISIBLE, WT_TRUNCATE_SEARCH_NOT_VISIBLE } WT_TRUNCATE_SEARCH_MODE;

/*
 * __disagg_truncate_free --
 *     Free an entry in the layered dhandle truncate list.
 */
static void
__disagg_truncate_free(WT_SESSION_IMPL *session, WT_TRUNCATE **entry)
{
    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    if (entry == NULL || *entry == NULL)
        return;

    __wt_free(session, (*entry)->uri);
    __wt_buf_free(session, &(*entry)->start_key);
    __wt_buf_free(session, &(*entry)->stop_key);
    __wt_free(session, *entry);
    *entry = NULL;
}

/*
 * __key_within_truncate_range --
 *     Search if the key is within a truncate range.
 */
static int
__key_within_truncate_range(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *start_key, const WT_ITEM *stop_key, const WT_ITEM *key, bool *is_within_rangep)
{
    WT_ASSERT(session, is_within_rangep != NULL);
    *is_within_rangep = false;

    int compare_result = 0;

    /* A zeroed start key indicates a truncate from the beginning of the table. */
    if (start_key->size != 0) {
        WT_RET(__wt_compare(session, collator, key, start_key, &compare_result));
        if (compare_result < 0) {
            *is_within_rangep = false;
            return (0);
        }
    }

    /* A zeroed stop key indicates a truncate to end of table. */
    if (stop_key->size == 0) {
        *is_within_rangep = true;
        return (0);
    }

    WT_RET(__wt_compare(session, collator, key, stop_key, &compare_result));
    *is_within_rangep = (compare_result <= 0);
    return (0);
}

/*
 * __txn_insert_truncate_entry_helper --
 *     Register a truncate entry to the latest transaction and store it in the truncate list.
 */
static int
__txn_insert_truncate_entry_helper(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_TRUNCATE **tp)
{
    WT_DECL_RET;
    WT_TRUNCATE *t;

    t = *tp;

    WT_RET(__wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0));
    WT_ERR(__wt_txn_truncate(session, t));

    __wt_writelock(session, &layered_table->truncate_lock);
    TAILQ_INSERT_TAIL(&layered_table->truncateqh, t, q);
    __wt_writeunlock(session, &layered_table->truncate_lock);

    /* Ownership transferred to the txn op and truncate queue. */
    *tp = NULL;

err:
    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_insert_truncate_entry --
 *     Insert a truncate entry into the layered dhandle truncate list.
 */
int
__wt_insert_truncate_entry(
  WT_SESSION_IMPL *session, const char *uri, WT_ITEM *start_key, WT_ITEM *stop_key)
{
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table;
    WT_TRUNCATE *t = NULL;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     *
     * FIXME-WT-16789: Disallow sweep server or follower mode to clean up the dhandle from the
     * dhandle list, if there are entries in the truncate list.
     */
    WT_ASSERT_ALWAYS(session, __wt_session_get_dhandle(session, uri, NULL, NULL, 0) == 0,
      "failed to get layered dhandle for truncate entry insert");
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    WT_ERR(__wt_calloc_one(session, &t));
    WT_ERR(__wt_strdup(session, uri, &t->uri));

    /* A NULL start key indicates a truncate starts from the beginning of the table. */
    if (start_key != NULL)
        WT_ERR(__wt_buf_set(session, &t->start_key, start_key->data, start_key->size));

    /* A NULL stop key indicates a truncate to end of table. */
    if (stop_key != NULL)
        WT_ERR(__wt_buf_set(session, &t->stop_key, stop_key->data, stop_key->size));

    /*
     * Mark the WT_TRUNCATE object modified by the current transaction. Also required to update the
     * max_upd_txn.
     */
    WT_SAVE_DHANDLE(session, ret = __txn_insert_truncate_entry_helper(session, layered_table, &t));
    WT_ERR(ret);

    if (0) {
err:
        __disagg_truncate_free(session, &t);
    }

    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __truncate_search --
 *     Walk the layered table truncate list looking for a committed or uncommitted entry (depending
 *     on the search mode) whose range covers the given key. The matched entry is returned through
 *     the output parameter when non-NULL.
 */
static int
__truncate_search(WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, const WT_ITEM *key,
  const WT_TRUNCATE_SEARCH_MODE mode, WT_TRUNCATE **tp, bool *is_foundp)
{
    WT_ASSERT(session, is_foundp != NULL);
    *is_foundp = false;

    WT_COLLATOR *collator = layered_table->collator;
    WT_TRUNCATE *entry = NULL;

    TAILQ_FOREACH (entry, &layered_table->truncateqh, q) {
        const bool is_visible =
          __wt_txn_visible(session, entry->txn_id, entry->start_ts, entry->durable_ts);

        if (mode == WT_TRUNCATE_SEARCH_VISIBLE && !is_visible)
            continue;

        if (mode == WT_TRUNCATE_SEARCH_NOT_VISIBLE && is_visible)
            continue;

        WT_RET(__key_within_truncate_range(
          session, collator, &entry->start_key, &entry->stop_key, key, is_foundp));

        if (*is_foundp) {
            if (tp != NULL)
                *tp = entry;
            break;
        }
    }

    return (0);
}

/*
 * __wt_layered_table_truncate_detect_write_conflict --
 *     Search if the current key we are modifying conflicts with any uncommitted truncates in the
 *     layered table truncate list.
 *
 * FIXME-WT-16812: Investigate whether this function can be called below the cursor layer. Doing so
 *     would remove the write cursor operations dependency on the truncate list.
 */
int
__wt_layered_table_truncate_detect_write_conflict(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, const WT_ITEM *key)
{
    WT_DECL_RET;
    bool is_found = false;

    if (!__wt_process.disagg_fast_truncate_2026)
        return (0);

    WT_ASSERT(session, WT_PREFIX_MATCH(layered_table->iface.name, "layered:"));

    __wt_readlock(session, &layered_table->truncate_lock);

    /*
     * The truncate entry has already been committed if it is visible to this transaction. We can
     * ignore these entries.
     */
    ret = __truncate_search(
      session, layered_table, key, WT_TRUNCATE_SEARCH_NOT_VISIBLE, NULL, &is_found);

    __wt_readunlock(session, &layered_table->truncate_lock);
    WT_RET(ret);

    if (is_found) {
        WT_STAT_CONN_INCR(session, txn_update_conflict);
        __wt_session_set_last_error(
          session, WT_ROLLBACK, WT_WRITE_CONFLICT, WT_TXN_ROLLBACK_REASON_CONFLICT);
        return (WT_ROLLBACK);
    }

    return (0);
}

/*
 * __wt_truncate_delete_visible_check --
 *     Search if the given key has been deleted in the layered table truncate list.
 */
int
__wt_truncate_delete_visible_check(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_ITEM *key, WT_TRUNCATE **tp)
{
    WT_DECL_RET;
    bool is_found = false;

    if (!__wt_process.disagg_fast_truncate_2026)
        return (WT_NOTFOUND);

    WT_ASSERT(session, WT_PREFIX_MATCH(layered_table->iface.name, "layered:"));

    __wt_readlock(session, &layered_table->truncate_lock);

    /*
     * Ignore all truncate entries that haven't been committed. They won't be visible to this
     * transaction.
     */
    ret = __truncate_search(session, layered_table, key, WT_TRUNCATE_SEARCH_VISIBLE, tp, &is_found);

    __wt_readunlock(session, &layered_table->truncate_lock);
    WT_RET(ret);

    return (is_found ? 0 : WT_NOTFOUND);
}

/*
 * __wti_mark_committed_truncate_table --
 *     Mark a truncate table entry as committed, updating truncate entries timestamp information.
 */
int
__wti_mark_committed_truncate_table(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table;
    WT_TRUNCATE *entry;

    layered_table = NULL;
    entry = op->u.follower_truncate.t;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     *
     * FIXME-WT-16789: Disallow sweep server or follower mode to clean up the dhandle from the
     * dhandle list, if there are entries in the truncate list.
     */
    WT_ASSERT_ALWAYS(session, __wt_session_get_dhandle(session, entry->uri, NULL, NULL, 0) == 0,
      "failed to get layered dhandle when marking truncate committed");
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    __wt_writelock(session, &layered_table->truncate_lock);
    entry->txn_id = session->txn->time_point.id;
    entry->start_ts = session->txn->time_point.commit_timestamp;
    entry->durable_ts = session->txn->time_point.durable_timestamp;
    __wt_writeunlock(session, &layered_table->truncate_lock);

    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wti_layered_table_truncate_rollback --
 *     Perform transaction rollback for a truncate operation, removing the truncate entry from the
 *     layered table truncate list.
 */
int
__wti_layered_table_truncate_rollback(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered_table;
    WT_TRUNCATE *entry;

    layered_table = NULL;
    entry = op->u.follower_truncate.t;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     *
     * FIXME-WT-16789: Disallow sweep server or follower mode to clean up the dhandle from the
     * dhandle list, if there are entries in the truncate list.
     */
    WT_ASSERT_ALWAYS(session, __wt_session_get_dhandle(session, entry->uri, NULL, NULL, 0) == 0,
      "failed to get layered dhandle during truncate rollback");
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    __wt_writelock(session, &layered_table->truncate_lock);
    TAILQ_REMOVE(&layered_table->truncateqh, entry, q);
    __wt_writeunlock(session, &layered_table->truncate_lock);
    __disagg_truncate_free(session, &entry);
    op->u.follower_truncate.t = NULL;

    WT_TRET(__wt_session_release_dhandle(session));
    return (ret);
}

/*
 * __wt_layered_table_truncate_clear --
 *     Clear all entries in the layered dhandle truncate list.
 */
void
__wt_layered_table_truncate_clear(WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table)
{
    WT_TRUNCATE *entry;

    entry = NULL;

    __wt_writelock(session, &layered_table->truncate_lock);
    while ((entry = TAILQ_FIRST(&layered_table->truncateqh)) != NULL) {
        TAILQ_REMOVE(&layered_table->truncateqh, entry, q);
        __disagg_truncate_free(session, &entry);
    }
    __wt_writeunlock(session, &layered_table->truncate_lock);
}
