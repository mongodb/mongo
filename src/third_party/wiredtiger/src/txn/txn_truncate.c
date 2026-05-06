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

    WT_RET(__wt_compare(session, collator, key, start_key, &compare_result));
    if (compare_result < 0) {
        *is_within_rangep = false;
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

    if (TAILQ_EMPTY(&layered_table->truncateqh))
        WT_DHANDLE_ACQUIRE(&layered_table->iface);

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
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_ITEM *start_key, WT_ITEM *stop_key)
{
    WT_DECL_ITEM(start_buf);
    WT_DECL_ITEM(stop_buf);
    WT_DECL_RET;
    WT_TRUNCATE *t = NULL;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);
    WT_ASSERT(session, layered_table != NULL);
    WT_ASSERT(session, F_ISSET(&layered_table->iface, WT_DHANDLE_OPEN));

    /* Caller resolves open-ended ranges to concrete keys before reaching us. */
    WT_ASSERT(session, start_key != NULL && stop_key != NULL);
    WT_ASSERT(session, start_key->size != 0 && stop_key->size != 0);

    WT_RET(__wt_scr_alloc(session, 0, &start_buf));
    WT_RET(__wt_scr_alloc(session, 0, &stop_buf));
    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_3,
      "insert entry into truncate list on table %s: start=%s stop=%s", layered_table->iface.name,
      __wt_key_string(
        session, start_key->data, start_key->size, layered_table->key_format, start_buf),
      __wt_key_string(
        session, stop_key->data, stop_key->size, layered_table->key_format, stop_buf));

    WT_ERR(__wt_calloc_one(session, &t));
    t->layered_table = layered_table;

    WT_ERR(__wt_buf_set(session, &t->start_key, start_key->data, start_key->size));
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
    __wt_scr_free(session, &start_buf);
    __wt_scr_free(session, &stop_buf);

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

    WT_STAT_CONN_INCR(session, layered_truncate_list_search_calls);

    WT_COLLATOR *collator = layered_table->collator;
    WT_TRUNCATE *entry = NULL;

    TAILQ_FOREACH (entry, &layered_table->truncateqh, q) {
        WT_STAT_CONN_INCR(session, layered_truncate_list_search_entries_walked);

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

    /* FIXME-WT-17384: Investigate the use of atomics to minimize locking. */
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

    /* FIXME-WT-17384: Investigate the use of atomics to minimize locking. */
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
 * __disagg_truncate_apply --
 *     Helper function to apply a truncate operation to the layered table. The actual application
 *     logic is passed in through the apply_func parameter.
 */
static void
__disagg_truncate_apply(WT_SESSION_IMPL *session, WT_TXN_OP *op,
  void (*apply_func)(WT_SESSION_IMPL *, WT_LAYERED_TABLE *, WT_TXN_OP *))
{
    WT_ASSERT(session, op != NULL);
    WT_TRUNCATE *entry = op->u.follower_truncate.t;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);
    WT_ASSERT(session, entry != NULL);
    WT_ASSERT(session, entry->layered_table != NULL);
    WT_ASSERT(
      session, __wt_atomic_load_uint32_relaxed(&entry->layered_table->iface.references) > 0);

    apply_func(session, entry->layered_table, op);
}

/*
 * __wti_mark_committed_truncate_table_apply --
 *     Stamp commit metadata onto a truncate entry in the provided layered table. The write lock
 *     serializes readers that walk the truncate list under truncate_lock while checking the entry's
 *     plain txn/timestamp fields for visibility.
 */
void
__wti_mark_committed_truncate_table_apply(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_TXN_OP *op)
{
    WT_TRUNCATE *entry = op->u.follower_truncate.t;

    /*
     * FIXME-WT-17347 Remove the queue-wide write lock when applying commit metadata to truncate
     * entry
     */
    __wt_writelock(session, &layered_table->truncate_lock);
    entry->txn_id = session->txn->time_point.id;
    entry->start_ts = session->txn->time_point.commit_timestamp;
    entry->durable_ts = session->txn->time_point.durable_timestamp;
    __wt_writeunlock(session, &layered_table->truncate_lock);
}

/*
 * __wti_mark_committed_truncate_table --
 *     Mark a truncate table entry as committed, updating truncate entries timestamp information.
 */
void
__wti_mark_committed_truncate_table(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    __disagg_truncate_apply(session, op, __wti_mark_committed_truncate_table_apply);
}

/*
 * __truncate_entry_remove --
 *     Remove an entry from the truncate queue. Must be called under the truncate write lock.
 */
static void
__truncate_entry_remove(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_TRUNCATE *entry)
{
    WT_ASSERT(session, !TAILQ_EMPTY(&layered_table->truncateqh));
    WT_ASSERT(session, __wt_atomic_load_uint32_relaxed(&layered_table->iface.references) > 0);

    TAILQ_REMOVE(&layered_table->truncateqh, entry, q);

    if (TAILQ_EMPTY(&layered_table->truncateqh))
        WT_DHANDLE_RELEASE(&layered_table->iface);
}

/*
 * __wti_layered_table_truncate_rollback_apply --
 *     Remove a truncate entry from a layered table as part of rollback processing.
 */
void
__wti_layered_table_truncate_rollback_apply(
  WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table, WT_TXN_OP *op)
{
    WT_TRUNCATE *entry = op->u.follower_truncate.t;

    __wt_writelock(session, &layered_table->truncate_lock);
    __truncate_entry_remove(session, layered_table, entry);
    __wt_writeunlock(session, &layered_table->truncate_lock);

    op->u.follower_truncate.t = NULL;
    __disagg_truncate_free(session, &entry);
}

/*
 * __wti_layered_table_truncate_rollback --
 *     Perform transaction rollback for a truncate operation, removing the truncate entry from the
 *     layered table truncate list.
 */
void
__wti_layered_table_truncate_rollback(WT_SESSION_IMPL *session, WT_TXN_OP *op)
{
    __disagg_truncate_apply(session, op, __wti_layered_table_truncate_rollback_apply);
}

/*
 * __wt_layered_table_truncate_clear --
 *     Clear all entries in the layered dhandle truncate list.
 */
void
__wt_layered_table_truncate_clear(WT_SESSION_IMPL *session, WT_LAYERED_TABLE *layered_table)
{
    WT_ASSERT(session, layered_table != NULL);

    WT_TRUNCATE *entry = NULL;

    __wt_writelock(session, &layered_table->truncate_lock);

    while ((entry = TAILQ_FIRST(&layered_table->truncateqh)) != NULL) {
        __truncate_entry_remove(session, layered_table, entry);
        __disagg_truncate_free(session, &entry);
    }
    __wt_writeunlock(session, &layered_table->truncate_lock);
}
