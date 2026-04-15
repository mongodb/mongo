/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __disagg_truncate_free --
 *     Free an entry in the layered dhandle truncate list.
 */
static void
__disagg_truncate_free(WT_SESSION_IMPL *session, WT_TRUNCATE **entry)
{
    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    if (entry == NULL)
        return;

    __wt_free(session, (*entry)->uri);
    __wt_free(session, (*entry)->start_key);
    __wt_free(session, (*entry)->stop_key);
    __wt_free(session, *entry);
    *entry = NULL;
}

/*
 * __key_within_truncate_range --
 *     Search if the key is within a truncate range.
 */
static bool
__key_within_truncate_range(WT_SESSION_IMPL *session, WT_COLLATOR *collator,
  const WT_ITEM *start_key, const WT_ITEM *stop_key, const WT_ITEM *key)
{
    int start_cmp, stop_cmp;

    WT_RET(__wt_compare(session, collator, key, start_key, &start_cmp));
    if (start_cmp < 0)
        return (false);

    /* A zeroed stop key indicates a truncate to end of table. */
    if (stop_key->size == 0)
        return (true);

    WT_RET(__wt_compare(session, collator, key, stop_key, &stop_cmp));
    if (stop_cmp > 0)
        return (false);

    return (true);
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
    WT_TRUNCATE *t;

    WT_ASSERT(session, __wt_process.disagg_fast_truncate_2026 == true);

    /*
     * Get the layered table from the provided URI. We don't hold any global locks so that's
     * possible that it was already removed.
     *
     * FIXME-WT-16789: Disallow sweep server or follower mode to clean up the dhandle from the
     * dhandle list, if there are entries in the truncate list.
     */
    WT_ASSERT(session, __wt_session_get_dhandle(session, uri, NULL, NULL, 0) == 0);
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    WT_RET(__wt_calloc_def(session, sizeof(WT_TRUNCATE), &t));
    WT_ERR(__wt_strdup(session, uri, &t->uri));
    WT_ERR(__wt_buf_set(session, &t->start_key, start_key->data, start_key->size));
    /* A NULL stop key indicates a truncate to end of table. */
    if (stop_key != NULL)
        WT_ERR(__wt_buf_set(session, &t->stop_key, stop_key->data, stop_key->size));

    /*
     * Mark the WT_TRUNCATE object modified by the current transaction. Also required to update the
     * max_upd_txn.
     */
    WT_ERR(__wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0));
    WT_ERR(__wt_txn_truncate(session, t));
    WT_ERR(__wt_session_release_dhandle(session));

    session->dhandle = (WT_DATA_HANDLE *)layered_table;
    __wt_writelock(session, &layered_table->truncate_lock);
    TAILQ_INSERT_TAIL(&layered_table->truncateqh, t, q);
    __wt_writeunlock(session, &layered_table->truncate_lock);

    if (0) {
err:
        __disagg_truncate_free(session, &t);
    }
    WT_TRET(__wt_session_release_dhandle(session));

    return (ret);
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
    WT_TRUNCATE *entry;

    if (!__wt_process.disagg_fast_truncate_2026)
        return (0);

    WT_ASSERT(session, WT_PREFIX_MATCH(layered_table->iface.name, "layered:"));

    WT_COLLATOR *collator = ((WT_LAYERED_TABLE *)layered_table)->collator;

    __wt_readlock(session, &layered_table->truncate_lock);
    TAILQ_FOREACH (entry, &layered_table->truncateqh, q) {
        /*
         * If the truncate entry has already been committed if it is visible to this transaction. We
         * can ignore these entries.
         */
        if (__wt_txn_visible(session, entry->txn_id, entry->start_ts, entry->durable_ts))
            continue;

        if (__key_within_truncate_range(
              session, collator, &entry->start_key, &entry->stop_key, key)) {
            __wt_readunlock(session, &layered_table->truncate_lock);
            return (WT_WRITE_CONFLICT);
        }
    }
    __wt_readunlock(session, &layered_table->truncate_lock);
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
    WT_TRUNCATE *entry;

    if (!__wt_process.disagg_fast_truncate_2026)
        return (WT_NOTFOUND);

    WT_ASSERT(session, WT_PREFIX_MATCH(layered_table->iface.name, "layered:"));
    WT_COLLATOR *collator = ((WT_LAYERED_TABLE *)layered_table)->collator;

    __wt_readlock(session, &layered_table->truncate_lock);
    TAILQ_FOREACH (entry, &layered_table->truncateqh, q) {
        /*
         * Ignore all truncate entries that hasn't been committed. They won't be visible to this
         * transaction.
         */
        if (!__wt_txn_visible(session, entry->txn_id, entry->start_ts, entry->durable_ts))
            continue;

        if (__key_within_truncate_range(
              session, collator, &entry->start_key, &entry->stop_key, key)) {
            if (tp != NULL)
                *tp = entry;
            __wt_readunlock(session, &layered_table->truncate_lock);
            return (0);
        }
    }
    __wt_readunlock(session, &layered_table->truncate_lock);
    return (WT_NOTFOUND);
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
    WT_ASSERT(session, __wt_session_get_dhandle(session, entry->uri, NULL, NULL, 0) == 0);
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    __wt_writelock(session, &layered_table->truncate_lock);
    entry->txn_id = session->txn->time_point.id;
    entry->start_ts = session->txn->time_point.commit_timestamp;
    entry->durable_ts = session->txn->time_point.durable_timestamp;
    __wt_writeunlock(session, &layered_table->truncate_lock);
    WT_TRET(__wt_session_release_dhandle(session));
    return (0);
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
    WT_ASSERT(session, __wt_session_get_dhandle(session, entry->uri, NULL, NULL, 0) == 0);
    layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    __wt_writelock(session, &layered_table->truncate_lock);
    TAILQ_REMOVE(&layered_table->truncateqh, entry, q);
    __wt_writeunlock(session, &layered_table->truncate_lock);

    WT_TRET(__wt_session_release_dhandle(session));
    return (0);
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
