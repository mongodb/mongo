/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static void __disagg_shared_metadata_queue_clear(WT_SESSION_IMPL *session);

/*
 * __layered_create_missing_stable_table --
 *     Create a missing stable table from an existing layered table configuration.
 */
static int
__layered_create_missing_stable_table(
  WT_SESSION_IMPL *session, const char *uri, const char *layered_cfg)
{
    WT_DECL_RET;
    const char *constituent_cfg;
    const char *stable_cfg[4] = {WT_CONFIG_BASE(session, table_meta), layered_cfg, NULL, NULL};

    constituent_cfg = NULL;

    /* Disable logging on the stable table so we have timestamps. */
    stable_cfg[2] = "log=(enabled=false)";

    WT_ERR(__wt_config_merge(session, stable_cfg, NULL, &constituent_cfg));
    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, uri, constituent_cfg));

err:
    __wt_free(session, constituent_cfg);
    return (ret);
}

/*
 * __layered_create_missing_stable_tables_legacy --
 *     Create missing stable tables in cases we don't use schema epochs. Note that this is
 *     best-effort and is not able to handle all cases of operation interleaving.
 */
static int
__layered_create_missing_stable_tables_legacy(WT_SESSION_IMPL *session)
{
    WT_CONFIG_ITEM cval;
    WT_CURSOR *cursor_check, *cursor_scan;
    WT_DECL_RET;
    char *stable_uri;
    const char *layered_uri, *layered_cfg;

    cursor_check = cursor_scan = NULL;
    stable_uri = NULL;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->schema_lock);

    /*
     * Clear all existing queue entries: the legacy path rebuilds shared metadata by scanning local
     * metadata directly rather than replaying queue entries, so stale entries (e.g. those enqueued
     * on the follower when the stable constituent did not yet exist) must be cleared before new
     * entries are added below.
     */
    __disagg_shared_metadata_queue_clear(session);

    WT_ERR(__wt_metadata_cursor(session, &cursor_check));
    WT_ERR(__wt_metadata_cursor(session, &cursor_scan));

    cursor_scan->set_key(cursor_scan, "layered:");
    WT_ERR(cursor_scan->bound(cursor_scan, "bound=lower"));
    while ((ret = cursor_scan->next(cursor_scan)) == 0) {
        WT_ERR(cursor_scan->get_key(cursor_scan, &layered_uri));
        WT_ERR(cursor_scan->get_value(cursor_scan, &layered_cfg));
        if (!WT_PREFIX_MATCH(layered_uri, "layered:"))
            break;

        /* Extract the stable URI. */
        WT_ERR(__wt_config_getones(session, layered_cfg, "stable", &cval));
        WT_ERR(__wt_strndup(session, cval.str, cval.len, &stable_uri));

        /* Check if the URI exists. */
        cursor_check->set_key(cursor_check, stable_uri);
        WT_ERR_NOTFOUND_OK(cursor_check->search(cursor_check), true);

        /*
         * Create the stable table if it does not exist: We must have picked up a checkpoint with a
         * new table.
         */
        if (ret == WT_NOTFOUND) {
            WT_ERR_MSG_CHK(session,
              __layered_create_missing_stable_table(session, stable_uri, layered_cfg),
              "Failed to create missing stable table \"%s\" from \"%s\"", stable_uri, layered_cfg);

            /*
             * Enqueue a metadata operation for creating the table. The schema epoch value does not
             * matter, because we can get here only if we are not using schema epochs.
             */
            WT_ERR(__wt_disagg_enqueue_metadata_operation(session, stable_uri,
              layered_uri + strlen("layered:"), WT_SHARED_METADATA_CREATE,
              WT_SCHEMA_EPOCH_UNPUBLISHED, true));
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Created missing stable table \"%s\" from \"%s\"", stable_uri, layered_uri);
        }

        __wt_free(session, stable_uri);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_free(session, stable_uri);
    WT_TRET(__wt_metadata_cursor_release(session, &cursor_check));
    WT_TRET(__wt_metadata_cursor_release(session, &cursor_scan));
    return (ret);
}

/*
 * __layered_create_has_following_remove --
 *     Return true if there is a REMOVE entry for the same URI after the given CREATE entry in the
 *     shared metadata queue.
 */
static bool
__layered_create_has_following_remove(
  WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *conn, WT_DISAGG_METADATA_OP *create_entry)
{
    WT_DISAGG_METADATA_OP *tmp;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    tmp = TAILQ_NEXT(create_entry, q);
    if (tmp == NULL)
        return (false);

    TAILQ_FOREACH_FROM(tmp, &conn->disaggregated_storage.shared_metadata_qh, q)
    {
        if (tmp->metadata_op == WT_SHARED_METADATA_REMOVE &&
          strcmp(tmp->stable_uri, create_entry->stable_uri) == 0) {
            /*
             * Assert queue ordering: the REMOVE must be at the same or later epoch as the CREATE. A
             * REMOVE at an earlier epoch than the CREATE is a bug in the queue.
             */
            WT_ASSERT(session, tmp->schema_epoch >= create_entry->schema_epoch);
            return (true);
        }
    }
    return (false);
}

/*
 * __layered_create_missing_stable_tables_helper --
 *     Create missing stable tables.
 */
static int
__layered_create_missing_stable_tables_helper(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry;
    wt_timestamp_t stable_schema_epoch;

    conn = S2C(session);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    /* If we don't use schema epochs, fall back to the legacy method. */
    stable_schema_epoch =
      __wt_atomic_load_uint64_acquire(&conn->txn_global.last_ckpt_disaggregated_schema_epoch);
    if (stable_schema_epoch == WT_SCHEMA_EPOCH_NONE)
        return (__layered_create_missing_stable_tables_legacy(session));

    /* Create missing stable tables for new layered tables in the shared metadata queue. */
    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    TAILQ_FOREACH (entry, &conn->disaggregated_storage.shared_metadata_qh, q) {

        /* Assert that older entries have been already pruned. */
        WT_ASSERT(session, entry->schema_epoch > stable_schema_epoch);

        if (entry->metadata_op != WT_SHARED_METADATA_CREATE)
            continue;

        WT_ASSERT(session, entry->layered_value != NULL);
        WT_ASSERT(session, entry->stable_uri != NULL);
        WT_ASSERT(session, WT_PREFIX_MATCH(entry->stable_uri, "file:"));

        /* Check if the table was dropped. */
        if (__layered_create_has_following_remove(session, conn, entry)) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Skip creating missing stable table \"%s\" with schema epoch %" PRIu64
              " from layered config \"%s\" since it is being dropped",
              entry->stable_uri, entry->schema_epoch, entry->layered_value);
            continue;
        }

        /* The table hasn't been dropped, so create it. */
        WT_ERR_MSG_CHK(session,
          __layered_create_missing_stable_table(session, entry->stable_uri, entry->layered_value),
          "Failed to create missing stable table \"%s\" with schema epoch %" PRIu64
          " from layered config \"%s\"",
          entry->stable_uri, entry->schema_epoch, entry->layered_value);
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Created missing stable table \"%s\" with schema epoch %" PRIu64 " from \"%s\"",
          entry->stable_uri, entry->schema_epoch, entry->layered_value);

        /*
         * Populate the stable value from local metadata so the queue entry can flush it to the
         * shared metadata table at the next checkpoint. This is needed when the create happened on
         * a follower (where the stable constituent is absent), so the value was not captured at
         * enqueue time.
         */
        if (entry->stable_value == NULL)
            WT_ERR_MSG_CHK(session,
              __wt_metadata_search(session, entry->stable_uri, &entry->stable_value),
              "Failed to read stable table metadata \"%s\"", entry->stable_uri);
    }

err:
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    return (ret);
}

/*
 * __layered_create_missing_stable_tables --
 *     Create missing stable tables.
 */
static int
__layered_create_missing_stable_tables(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_WITH_SCHEMA_LOCK(session, ret = __layered_create_missing_stable_tables_helper(session));
    return (ret);
}

/*
 * __wt_disagg_set_database_size --
 *     Set the database size in disaggregated storage.
 */
void
__wt_disagg_set_database_size(WT_SESSION_IMPL *session, uint64_t database_size)
{
    S2C(session)->disaggregated_storage.database_size = database_size;
    WT_STAT_CONN_SET(session, disagg_database_size, database_size);
}

/*
 * __disagg_shared_metadata_queue_free --
 *     Free an entry in the update metadata queue.
 */
static void
__disagg_shared_metadata_queue_free(WT_SESSION_IMPL *session, WT_DISAGG_METADATA_OP **entry)
{
    if (*entry == NULL)
        return;
    __wt_free(session, (*entry)->stable_uri);
    __wt_free(session, (*entry)->table_name);
    __wt_free(session, (*entry)->colgroup_value);
    __wt_free(session, (*entry)->layered_value);
    __wt_free(session, (*entry)->stable_value);
    __wt_free(session, (*entry)->table_value);
    __wt_free(session, *entry);
    *entry = NULL;
}

/*
 * __shared_metadata_op_to_string --
 *     Convert a metadata operation to string representation.
 */
static inline const char *
__shared_metadata_op_to_string(WT_SHARED_METADATA_OP op)
{
    switch (op) {
    case WT_SHARED_METADATA_NONE:
        return ("NONE");
    case WT_SHARED_METADATA_UPDATE:
        return ("UPDATE");
    case WT_SHARED_METADATA_CREATE:
        return ("CREATE");
    case WT_SHARED_METADATA_REMOVE:
        return ("REMOVE");
    }
    return ("UNKNOWN");
}

/*
 * __disagg_save_metadata --
 *     Fetch a metadata key/value pair from the metadata table and save the value.
 */
static int
__disagg_save_metadata(WT_SESSION_IMPL *session, WT_CURSOR *md_cursor, const char *prefix,
  const char *key, char **valuep)
{
    WT_DECL_ITEM(md_key);
    WT_DECL_RET;
    const char *md_value;

    WT_ERR(__wt_scr_alloc(session, 0, &md_key));
    WT_ERR(__wt_buf_fmt(session, md_key, "%s%s", prefix, key));

    md_cursor->set_key(md_cursor, md_key->data);
    WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);
    if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND)) {
        WT_ERR(md_cursor->get_value(md_cursor, &md_value));
        WT_ERR(__wt_strdup(session, md_value, valuep));
    }

err:
    __wt_scr_free(session, &md_key);
    return (ret);
}

/*
 * __wt_disagg_enqueue_metadata_operation --
 *     Enqueue a metadata operation for a given URI into the shared metadata table to be done at the
 *     next checkpoint.
 */
int
__wt_disagg_enqueue_metadata_operation(WT_SESSION_IMPL *session, const char *stable_uri,
  const char *table_name, WT_SHARED_METADATA_OP metadata_op, wt_timestamp_t schema_epoch,
  bool deferred)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry;

    conn = S2C(session);
    cursor = NULL;
    entry = NULL;

    /*
     * Ensure that the schema lock is held. We cannot check this via spinlock ownership, because
     * this function might be called from an internal session, while the lock was acquired by its
     * parent session.
     */
    WT_ASSERT(session, FLD_ISSET(session->lock_flags, WT_SESSION_LOCKED_SCHEMA));

    /* Allocate the entry structure. */
    WT_ERR(__wt_calloc_one(session, &entry));
    entry->metadata_op = metadata_op;
    entry->schema_epoch = schema_epoch;
    WT_ERR(__wt_strdup(session, stable_uri, &entry->stable_uri));
    WT_ERR(__wt_strdup(session, table_name, &entry->table_name));

    /* Get the table metadata. */
    WT_ERR(__wt_metadata_cursor(session, &cursor));

    /* Fetch the relevant data from the metadata table and save it in the entry. */
    WT_ERR(
      __disagg_save_metadata(session, cursor, "colgroup:", table_name, &entry->colgroup_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "layered:", table_name, &entry->layered_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "table:", table_name, &entry->table_value));
    WT_ERR(__disagg_save_metadata(session, cursor, "", stable_uri, &entry->stable_value));

    /*
     * Schema operations (create, drop) start deferred: at the start of each checkpoint, while the
     * schema lock is held, the deferred flag is cleared on all existing entries so they are applied
     * at the end of that checkpoint. Entries enqueued after that point, including concurrent schema
     * ops, will not be processed until the following checkpoint. The one exception is the block
     * manager callback that records the new checkpointed state of each stable table: those entries
     * pass deferred=false so that the updated checkpoint metadata is written to the shared metadata
     * table in the same checkpoint.
     */
    entry->deferred = deferred;

    /* Cannot fail past this point. */
    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    TAILQ_INSERT_TAIL(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Scheduled copying disaggregated metadata for table \"%s\" (stable URI \"%s\") with %s "
      "operation to shared "
      "metadata table at next checkpoint:",
      table_name, stable_uri, __shared_metadata_op_to_string(metadata_op));
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  colgroup: %s",
      entry->colgroup_value == NULL ? "<none>" : entry->colgroup_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  layered: %s",
      entry->layered_value == NULL ? "<none>" : entry->layered_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  table: %s",
      entry->table_value == NULL ? "<none>" : entry->table_value);
    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "  stable: %s",
      entry->stable_value == NULL ? "<none>" : entry->stable_value);

    /* No need to free the entry structure here as it has been added to the queue. */
    entry = NULL;

err:
    __disagg_shared_metadata_queue_free(session, &entry);

    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __disagg_shared_metadata_queue_clear --
 *     Clear the update metadata list.
 */
static void
__disagg_shared_metadata_queue_clear(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_METADATA_OP *entry, *tmp;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    WT_TAILQ_SAFE_REMOVE_BEGIN(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        __disagg_shared_metadata_queue_free(session, &entry);
    }
    WT_TAILQ_SAFE_REMOVE_END

    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
}

/*
 * __wti_disagg_shared_metadata_queue_prune --
 *     Prune the shared metadata queue of any entries that are older than the given checkpoint.
 */
void
__wti_disagg_shared_metadata_queue_prune(WT_SESSION_IMPL *session, wt_timestamp_t cur_schema_epoch)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_METADATA_OP *entry, *tmp;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    TAILQ_FOREACH_SAFE(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        /*
         * When EPOCH_NONE is passed (legacy step-up path that doesn't use schema epochs), prune
         * everything unconditionally. The legacy path rebuilds stable constituents directly from
         * local metadata rather than replaying queue entries, so the queue is no longer needed.
         */
        if (cur_schema_epoch != WT_SCHEMA_EPOCH_NONE && entry->schema_epoch > cur_schema_epoch)
            continue;
        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        __disagg_shared_metadata_queue_free(session, &entry);
    }

    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
}

/*
 * __wti_disagg_table_latest_create_remove --
 *     Return the latest CREATE or REMOVE operation for the given table name in the shared metadata
 *     queue. Returns WT_SHARED_METADATA_NONE as a sentinel when no CREATE or REMOVE entry is found.
 *     UPDATE entries are skipped because they do not affect whether the table exists.
 */
WT_SHARED_METADATA_OP
__wti_disagg_table_latest_create_remove(WT_SESSION_IMPL *session, const char *table_name)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_METADATA_OP *entry;
    WT_SHARED_METADATA_OP last_op;

    conn = S2C(session);
    last_op = WT_SHARED_METADATA_NONE;

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    TAILQ_FOREACH (entry, &conn->disaggregated_storage.shared_metadata_qh, q)
        if (entry->metadata_op != WT_SHARED_METADATA_UPDATE &&
          strcmp(entry->table_name, table_name) == 0)
            last_op = entry->metadata_op;
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    return (last_op);
}

/*
 * __disagg_shared_metadata_op_helper --
 *     Perform the remove/update operation in the shared metadata table.
 */
static int
__disagg_shared_metadata_op_helper(
  WT_SESSION_IMPL *session, const char *key, const char *value, WT_SHARED_METADATA_OP metadata_op)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL};

    WT_ASSERT(session, S2C(session)->layered_table_manager.leader);

    cursor = NULL;

    WT_ERR(__wt_open_cursor(session, WT_DISAGG_METADATA_URI, NULL, cfg, &cursor));
    cursor->set_key(cursor, key);

    switch (metadata_op) {
    case WT_SHARED_METADATA_NONE:
        break;
    case WT_SHARED_METADATA_REMOVE:
        /*
         * Layered tables can be created via two methods. When created with the "table:" prefix, we
         * expect metadata entries for layered, colgroup, table, and file. When created with the
         * "layered:" prefix, we expect only layered and file metadata entries.
         *
         * Since either form may appear depending on how the table was created, it is acceptable for
         * some lookups to return WT_NOTFOUND.
         */
        WT_ERR_NOTFOUND_OK(cursor->remove(cursor), false);
        break;
    case WT_SHARED_METADATA_CREATE:
    case WT_SHARED_METADATA_UPDATE:
        if (value == NULL) {
            ret = 0;
            goto err;
        }

        cursor->set_value(cursor, value);
        WT_ERR(cursor->insert(cursor));
        break;
    }

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "%s disaggregated shared metadata: key=\"%s\" value=\"%s\"",
      __shared_metadata_op_to_string(metadata_op), key, value);

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __disagg_shared_metadata_op --
 *     Remove/update all relevant metadata entries of a table in the shared metadata table.
 */
static int
__disagg_shared_metadata_op(WT_SESSION_IMPL *session, WT_DISAGG_METADATA_OP *entry)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(md_key);
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *queue_entry;

    conn = S2C(session);

    /*
     * For UPDATE operations, verify that the table's CREATE will be applied at or before the schema
     * epoch of the UPDATE, if it has not been applied yet. If a CREATE entry is still in the queue
     * with a schema epoch ahead of this UPDATE operation's schema epoch (including
     * WT_SCHEMA_EPOCH_UNPUBLISHED = WT_TS_MAX), the table will not be visible to followers before
     * the update. Having stable data in an unpublished table is an API contract violation, which
     * requires that a table must be published before the checkpoint that includes its data.
     */
    if (entry->metadata_op == WT_SHARED_METADATA_UPDATE) {
        WT_ASSERT_SPINLOCK_OWNED(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
        TAILQ_FOREACH (queue_entry, &conn->disaggregated_storage.shared_metadata_qh, q)
            if (queue_entry->metadata_op == WT_SHARED_METADATA_CREATE &&
              queue_entry->schema_epoch > entry->schema_epoch &&
              strcmp(queue_entry->table_name, entry->table_name) == 0)
                WT_RET_MSG(session, EINVAL, "Stable data checkpointed for unpublished table \"%s\"",
                  entry->table_name);
    }

    WT_ERR(__wt_scr_alloc(session, 0, &md_key));
    WT_ERR(__wt_buf_fmt(session, md_key, "colgroup:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->colgroup_value, entry->metadata_op));

    WT_ERR(__wt_buf_fmt(session, md_key, "layered:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->layered_value, entry->metadata_op));

    WT_ERR(__wt_buf_fmt(session, md_key, "table:%s", entry->table_name));
    WT_ERR(__disagg_shared_metadata_op_helper(
      session, md_key->data, entry->table_value, entry->metadata_op));

    WT_ERR(__disagg_shared_metadata_op_helper(
      session, entry->stable_uri, entry->stable_value, entry->metadata_op));
err:
    __wt_scr_free(session, &md_key);
    return (ret);
}

/*
 * __wt_disagg_shared_metadata_queue_drop_size --
 *     Walk the metadata queue and sum the checkpoint sizes of non-deferred drop operations. This is
 *     a read-only operation on the queue.
 */
int
__wt_disagg_shared_metadata_queue_drop_size(
  WT_SESSION_IMPL *session, wt_timestamp_t cur_schema_epoch, uint64_t *drop_sizep)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry;

    conn = S2C(session);
    *drop_sizep = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    TAILQ_FOREACH (entry, &conn->disaggregated_storage.shared_metadata_qh, q) {
        /* Skip entries that are not included in the current schema epoch. */
        if (entry->deferred)
            continue;
        if (cur_schema_epoch != WT_SCHEMA_EPOCH_NONE && entry->schema_epoch > cur_schema_epoch)
            continue;

        if (entry->metadata_op == WT_SHARED_METADATA_REMOVE && entry->stable_value != NULL) {
            uint64_t size;
            /*
             * A table that was created and dropped without ever being checkpointed won't have a
             * checkpoint entry in its metadata, so WT_NOTFOUND is expected.
             */
            WT_ERR_NOTFOUND_OK(__wt_ckpt_last_size(session, entry->stable_value, &size), false);
            *drop_sizep += size;
        }
    }

err:
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    return (ret);
}

/*
 * __disagg_handle_create_remove_pairing --
 *     Handle CREATE/REMOVE pairing detection during queue processing. If the entry is a CREATE with
 *     no stable value, park it in the skipped list and return true (caller should continue). If the
 *     entry is a REMOVE that cancels a parked CREATE, free both and return true. Otherwise return
 *     false.
 */
static bool
__disagg_handle_create_remove_pairing(WT_SESSION_IMPL *session, WT_CONNECTION_IMPL *conn,
  WT_DISAGG_METADATA_OP *entry, struct __wt_disagg_shared_metadata_qh *skipped_creates)
{
    WT_DISAGG_METADATA_OP *skipped, *skipped_tmp;
    bool found;

    /*
     * A CREATE entry with no stable_value means the stable constituent was never created: step-up
     * skipped it because a DROP follows in the queue, so the table no longer exists and we have no
     * data to include in shared metadata. Park it in a local list.
     *
     * A matching REMOVE in the same checkpoint is the only valid resolution -- both epochs are
     * stable so the table was never visible to any checkpoint, and we can safely skip both entries.
     * Anything else is an API violation detected below.
     */
    if (entry->metadata_op == WT_SHARED_METADATA_CREATE && entry->stable_value == NULL) {
        __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Tracking CREATE for \"%s\" with no stable value (epoch %" PRIu64
          ") for API violation detection",
          entry->stable_uri, entry->schema_epoch);
        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        TAILQ_INSERT_TAIL(skipped_creates, entry, q);
        return (true);
    }

    /*
     * For a REMOVE, cancel any parked CREATE entry for the same URI. When both land in the same
     * checkpoint, the table was created and dropped before any checkpoint required it to exist, so
     * we can safely skip both without writing anything to shared metadata.
     */
    if (entry->metadata_op == WT_SHARED_METADATA_REMOVE) {
        found = false;
        TAILQ_FOREACH_SAFE(skipped, skipped_creates, q, skipped_tmp)
        {
            if (strcmp(skipped->stable_uri, entry->stable_uri) == 0) {
                TAILQ_REMOVE(skipped_creates, skipped, q);
                __disagg_shared_metadata_queue_free(session, &skipped);
                found = true;
            }
        }
        if (found) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Skipping CREATE and REMOVE for \"%s\" (epoch %" PRIu64
              "): table created and dropped before any checkpoint required it to exist",
              entry->stable_uri, entry->schema_epoch);
            TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
            __disagg_shared_metadata_queue_free(session, &entry);
            return (true);
        }
    }

    return (false);
}

/*
 * __wt_disagg_shared_metadata_queue_process --
 *     Process the update metadata list.
 */
int
__wt_disagg_shared_metadata_queue_process(WT_SESSION_IMPL *session, wt_timestamp_t cur_schema_epoch)
{
    struct __wt_disagg_shared_metadata_qh skipped_creates;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry, *skipped, *tmp;

    conn = S2C(session);
    TAILQ_INIT(&skipped_creates);

    /*
     * This requires schema lock to ensure that we capture a consistent snapshot of metadata entries
     * related to the given shared table, e.g., the various file, colgroup, table, and layered
     * entries.
     */
    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    TAILQ_FOREACH_SAFE(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        /* Defer entries that belong to the next checkpoint. */
        if (entry->deferred) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Defer metadata operation %s for table \"%s\"",
              __shared_metadata_op_to_string(entry->metadata_op), entry->table_name);
            entry->deferred = false;
            continue;
        }

        /* Defer entries based on the schema epoch. */
        if (cur_schema_epoch != WT_SCHEMA_EPOCH_NONE && entry->schema_epoch > cur_schema_epoch) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Defer metadata operation %s for table \"%s\" with schema epoch %" PRIu64,
              __shared_metadata_op_to_string(entry->metadata_op), entry->table_name,
              entry->schema_epoch);
            WT_STAT_CONN_INCR(session, checkpoint_disagg_metadata_unstable);
            continue;
        }

        /* Park CREATE entries with no stable_value; cancel them if a matching REMOVE follows. */
        if (__disagg_handle_create_remove_pairing(session, conn, entry, &skipped_creates))
            continue;

        /* Failpoint: inject error to test panic handling during queue processing. */
        if (FLD_ISSET(conn->timing_stress_flags,
              WT_TIMING_STRESS_FAILPOINT_DISAGG_CHECKPOINT_QUEUE_DRAIN)) {
            ret = __wt_set_return(session, WT_ERROR);
            goto err;
        }

        WT_STAT_CONN_INCR(session, checkpoint_disagg_metadata_apply);
        WT_ERR(__disagg_shared_metadata_op(session, entry));

        TAILQ_REMOVE(&conn->disaggregated_storage.shared_metadata_qh, entry, q);
        __disagg_shared_metadata_queue_free(session, &entry);
    }

    /*
     * Any unmatched parked CREATE entry is an API violation: the stable epoch falls between the
     * CREATE and DROP epochs, so this checkpoint must include the table in shared metadata. But the
     * table was dropped and its stable constituent was never created, so we have no data to write.
     * Publish CREATE and DROP at the same epoch to avoid this window.
     */
    if (!TAILQ_EMPTY(&skipped_creates)) {
        TAILQ_FOREACH (skipped, &skipped_creates, q)
            __wt_verbose_error(session, WT_VERB_DISAGGREGATED_STORAGE,
              "API violation: Table \"%s\" was published with CREATE at epoch %" PRIu64
              " and DROP at a later epoch. This checkpoint must include the table in shared "
              "metadata, but the table was dropped and we have no data to write.",
              skipped->table_name, skipped->schema_epoch);
        WT_ERR_PANIC(session, EINVAL,
          "API violation: See above for details. Current schema epoch: %" PRIu64 ".",
          cur_schema_epoch);
    }

err:
    /*
     * If we failed, put back the skipped creates, so that we can revisit them if the caller
     * attempts to create a checkpoint again.
     */
    if (ret != 0)
        while (!TAILQ_EMPTY(&skipped_creates)) {
            skipped = TAILQ_LAST(&skipped_creates, __wt_disagg_shared_metadata_qh);
            TAILQ_REMOVE(&skipped_creates, skipped, q);
            TAILQ_INSERT_HEAD(&conn->disaggregated_storage.shared_metadata_qh, skipped, q);
        }

    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    while (!TAILQ_EMPTY(&skipped_creates)) {
        skipped = TAILQ_FIRST(&skipped_creates);
        TAILQ_REMOVE(&skipped_creates, skipped, q);
        __disagg_shared_metadata_queue_free(session, &skipped);
    }
    return (ret);
}

/*
 * __wt_disagg_shared_metadata_queue_publish --
 *     Publish schema operations in the shared metadata queue for the given object.
 */
int
__wt_disagg_shared_metadata_queue_publish(
  WT_SESSION_IMPL *session, const char *table_name, wt_timestamp_t schema_epoch)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_METADATA_OP *entry, *tmp;
    wt_timestamp_t prev_schema_epoch;

    conn = S2C(session);
    prev_schema_epoch = WT_SCHEMA_EPOCH_NONE;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->schema_lock);

    __wt_spin_lock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);

    TAILQ_FOREACH_SAFE(entry, &conn->disaggregated_storage.shared_metadata_qh, q, tmp)
    {
        if (strcmp(entry->table_name, table_name) != 0)
            continue;

        /* Update unpublished schema epochs before any ordering or range checks. */
        if (entry->schema_epoch == WT_SCHEMA_EPOCH_UNPUBLISHED) {
            __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
              "Publishing metadata operation %s for table \"%s\" to schema epoch %" PRIu64,
              __shared_metadata_op_to_string(entry->metadata_op), entry->table_name, schema_epoch);
            entry->schema_epoch = schema_epoch;
        }

        /* Check the ordering of schema epochs within the same table. */
        if (entry->schema_epoch < prev_schema_epoch)
            WT_ERR_MSG(session, EINVAL,
              "Schema epoch of metadata operation for table \"%s\" is out of order: current schema "
              "epoch %" PRIu64 ", previous schema epoch %" PRIu64,
              table_name, entry->schema_epoch, prev_schema_epoch);
        prev_schema_epoch = entry->schema_epoch;

        /* Check for already-published entries at a future schema epoch. */
        if (entry->schema_epoch > schema_epoch)
            WT_ERR_PANIC(session, EINVAL,
              "Table \"%s\" has published schema operations at future schema epoch %" PRIu64
              ", while publishing at schema epoch %" PRIu64,
              table_name, entry->schema_epoch, schema_epoch);
    }

err:
    __wt_spin_unlock(session, &conn->disaggregated_storage.shared_metadata_queue_lock);
    return (ret);
}

/*
 * __disagg_metadata_table_init --
 *     Initialize the shared metadata table.
 */
static int
__disagg_metadata_table_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *internal_session;

    conn = S2C(session);

    WT_ERR(__wt_open_internal_session(conn, "disagg-init", false, 0, 0, &internal_session));
    WT_ERR(__wt_session_create(
      internal_session, WT_DISAGG_METADATA_URI, "key_format=S,value_format=S,log=(enabled=false)"));

    /*
     * A follower should never access the live shared metadata dhandle. However, session->create
     * implicitly opens the live dhandle. To preserve this rule, we immediately expire the shared
     * metadata dhandle.
     *
     * FIXME-WT-17040: Investigate if it's necessary to create the shared metadata table on
     * followers.
     */
    if (!conn->layered_table_manager.leader) {
        WT_WITHOUT_DHANDLE(
          session, ret = __wti_conn_dhandle_outdated(session, WT_DISAGG_METADATA_URI));
        WT_ERR_MSG_CHK(
          session, ret, "Marking data handle outdated failed: \"%s\"", WT_DISAGG_METADATA_URI);
    }
err:
    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));
    return (ret);
}

/*
 * __wti_disagg_set_last_materialized_lsn --
 *     Set the latest materialized LSN.
 */
int
__wti_disagg_set_last_materialized_lsn(WT_SESSION_IMPL *session, uint64_t lsn)
{
    WT_DISAGGREGATED_STORAGE *disagg;
    uint64_t cur_lsn;

    disagg = &S2C(session)->disaggregated_storage;
    cur_lsn = __wt_atomic_load_uint64_acquire(&disagg->last_materialized_lsn);

    if (cur_lsn > lsn)
        return (EINVAL); /* Can't go backwards. */

    __wt_atomic_store_uint64_release(&disagg->last_materialized_lsn, lsn);
    return (0);
}

/*
 * __disagg_abandon_checkpoint --
 *     Abandon the current incomplete checkpoint, if the operation is supported by the provided PALI
 *     implementation. It is a no-op if the operation is not supported, in which case the
 *     application must either perform the equivalent operation before changing roles, or otherwise
 *     guarantee that no updates have been made since the last completed checkpoint.
 *
 * If there are any updates after the last completed checkpoint beyond this point, performing any
 *     writes to the disaggregated tables may lead to undefined behavior, such as illegal delta
 *     chains with wrong backlink LSNs, committing updates from incomplete checkpoints, or even data
 *     loss in the case of not cleaning up abandoned page discards.
 */
static int
__disagg_abandon_checkpoint(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can abandon a checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        WT_RET(EINVAL);

    /*
     * FIXME-WT-16524: This function is no longer an optional operation for testing, remove this
     * check.
     */
    if (disagg->npage_log->page_log->pl_abandon_checkpoint == NULL) {
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
          "Abandon checkpoint operation is not supported by the current PALI implementation");
        return (0);
    }

    /*
     * Call the PALI function to abandon the checkpoint. Since we are not specifying the latest
     * complete checkpoint, the implementation of this function would identify the LSN of the last
     * checkpoint completion record and drop all later records. If there are no more updates after
     * the last complete checkpoint, the function would have no effect.
     */
    ret = disagg->npage_log->page_log->pl_abandon_checkpoint(
      disagg->npage_log->page_log, &session->iface);

    if (ret == 0)
        WT_STAT_CONN_INCR(session, disagg_abandon_checkpoint_succeed);
    else
        WT_STAT_CONN_INCR(session, disagg_abandon_checkpoint_failed);

    return (ret);
}

/*
 * __disagg_begin_checkpoint --
 *     Begin the next checkpoint.
 */
static int
__disagg_begin_checkpoint(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can begin a global checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        return (0);

    /* On fresh startup, load an empty key to key provider. */
    if (conn->key_provider != NULL) {
        WT_DISAGG_METADATA metadata = {0};
        WT_RET(__wti_disagg_load_crypt_key(session, &metadata));
    }

    __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE,
      "Begin next disaggregated storage checkpoint: num_meta_put=%" PRIu64, disagg->num_meta_put);

    /* Store is sufficient because updates are protected by the checkpoint lock. */
    disagg->num_meta_put_at_ckpt_begin = disagg->num_meta_put;
    return (0);
}

/*
 * __disagg_restart_checkpoint --
 *     Restart the current checkpoint: Abandon the current checkpoint if it is incomplete (and the
 *     operation to abandon a checkpoint is supported), and begin a new checkpoint.
 */
static int
__disagg_restart_checkpoint(WT_SESSION_IMPL *session)
{
    WT_DECL_RET;

    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);

    WT_ERR_MSG_CHK(
      session, __disagg_abandon_checkpoint(session), "Failed to abandon the incomplete checkpoint");
    WT_ERR_MSG_CHK(session, __disagg_begin_checkpoint(session), "Failed to begin a new checkpoint");

err:
    return (ret);
}

/*
 * __disagg_step_up --
 *     Step up to the node to the leader mode.
 */
static int
__disagg_step_up(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *internal_session;

    conn = S2C(session);

    /*
     * Some functionality in stepping up needs a session that can open data handles. The default
     * session used to call this function cannot do that.
     */
    WT_RET(__wt_open_internal_session(conn, "disagg-step-up", false, 0, 0, &internal_session));

    /*
     * We need to hold the checkpoint lock while stepping up, because if we change the role
     * concurrently with a checkpoint, it would do only a part of the work required for the new
     * role, leaving the database in an inconsistent state.
     */
    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping up to the leader mode");
    F_SET_ATOMIC_32(conn, WT_CONN_RECONFIGURING_STEP_UP);

    /*
     * Step up to the leader mode. We need to do this first, because the rest of the operations
     * below depend on WiredTiger already being in the leader mode.
     */
    conn->layered_table_manager.leader = true;
    WT_STAT_CONN_SET(session, disagg_role_leader, 1);

    /*
     * Abandon the current checkpoint if it is incomplete, and begin a new one. We need to do this
     * before draining the ingest tables, so that the updates to the stable tables will be correctly
     * included in the new checkpoint.
     */
    WT_ERR(__disagg_restart_checkpoint(session));

    /*
     * We might not need to hold a checkpoint lock below this point, but we will keep it just to be
     * safe. If this becomes a problem, we can revisit whether we really need to hold the lock for
     * the remaining operations.
     */

    /* Create any missing stable tables. */
    WT_ERR_MSG_CHK(session, __layered_create_missing_stable_tables(internal_session),
      "Failed to create missing stable tables");

    /* Drain the ingest tables before switching to leader. */
    WT_ERR_MSG_CHK(session, __wti_layered_drain_ingest_tables(internal_session),
      "Failed to drain ingest tables");

err:
    WT_TRET(__wt_session_close_internal(internal_session));
    F_CLR_ATOMIC_32(conn, WT_CONN_RECONFIGURING_STEP_UP);
    return (ret);
}

/*
 * __disagg_mark_btrees_readonly_then_step_down --
 *     Mark all disaggregated btrees readonly and outdated, then step down to follower mode. The
 *     outdated mark makes the next leader open fresh handles instead of reusing these stale ones.
 */
static void
__disagg_mark_btrees_readonly_then_step_down(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DATA_HANDLE *dhandle;
    WT_DECL_RET;

    conn = S2C(session);

    for (dhandle = NULL;;) {
        WT_DHANDLE_NEXT(session, dhandle, &conn->dhqh, q);
        if (dhandle == NULL)
            break;

        /* Only care about open disaggregated btree dhandles. */
        if (!WT_DHANDLE_BTREE(dhandle) || !F_ISSET(dhandle, WT_DHANDLE_OPEN))
            continue;

        btree = (WT_BTREE *)dhandle->handle;

        if (!F_ISSET(btree, WT_BTREE_DISAGGREGATED) || F_ISSET(btree, WT_BTREE_READONLY))
            continue;

        WT_WITH_BTREE(session, btree, ret = __wt_evict_file_exclusive_on(session));
        WT_IGNORE_RET(ret);

        /* Mark the disaggregated as readonly. */
        F_SET(btree, WT_BTREE_READONLY);

        /*
         * Mark the handle outdated so that if we step back up as leader in the future, we open a
         * fresh one rather than reusing this handle's resident pages. Carrying those pages into a
         * new leader era lets the drain dirty a page that still holds an unresolved on-disk
         * prepared cell before the drain resolves it, which reconciliation cannot represent (leaked
         * prepared update).
         */
        F_SET(dhandle, WT_DHANDLE_OUTDATED);

        WT_WITH_BTREE(session, btree, __wt_evict_file_exclusive_off(session));
    }

    /* Step down to the follower mode. */
    conn->layered_table_manager.leader = false;
    WT_STAT_CONN_SET(session, disagg_role_leader, 0);
}

/*
 * __disagg_step_down --
 *     Step down to the follower mode.
 */
static void
__disagg_step_down(WT_SESSION_IMPL *session)
{
    WT_ASSERT_SPINLOCK_OWNED(session, &S2C(session)->checkpoint_lock);

    __wt_verbose_debug1(
      session, WT_VERB_DISAGGREGATED_STORAGE, "%s", "Stepping down to the follower mode");

    /*
     * Mark disaggregated btrees read-only before switching role to follower to prevent concurrent
     * eviction paths, especially parent split path, from dirtying pages during the step-down
     * window.
     */
    WT_WITH_HANDLE_LIST_READ_LOCK(session, __disagg_mark_btrees_readonly_then_step_down(session));

    /* Do some cleanup as we are abandoning the current checkpoint. */
    __disagg_shared_metadata_queue_clear(session);
}

/*
 * __wt_disagg_config_get_role --
 *     Parse the disaggregated role from the configuration and return whether this node is a leader.
 */
int
__wt_disagg_config_get_role(WT_SESSION_IMPL *session, const char **cfg, bool *leaderp)
{
    WT_CONFIG_ITEM cval;

    WT_RET(__wt_config_gets(session, cfg, "disaggregated.role", &cval));
    if (cval.len == 0 || WT_CONFIG_LIT_MATCH("follower", cval))
        *leaderp = false;
    else if (WT_CONFIG_LIT_MATCH("leader", cval))
        *leaderp = true;
    else
        WT_RET_MSG(session, EINVAL, "Invalid node role");

    return (0);
}

/*
 * __wti_disagg_conn_config --
 *     Parse and setup the disaggregated server options for the connection.
 */
int
__wti_disagg_conn_config(WT_SESSION_IMPL *session, const char **cfg, bool reconfig)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_ITEM complete_checkpoint_meta;
    WT_NAMED_PAGE_LOG *npage_log;
    uint64_t time_start, time_stop;
    bool leader, picked_up, was_leader;

    conn = S2C(session);
    leader = was_leader = conn->layered_table_manager.leader;
    npage_log = NULL;
    picked_up = false;

    WT_CLEAR(complete_checkpoint_meta);

    /* Reconfigure-only settings. */
    if (reconfig) {
        WT_STAT_CONN_INCR(session, disagg_conn_reconfig);

        /* Pick up a new checkpoint (followers only). */
        WT_ERR_NOTFOUND_OK(
          __wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval), true);
        if (ret == 0 && cval.len > 0) {
            /*
             * FIXME-WT-14733: currently the leader silently ignores the checkpoint_meta
             * configuration as it may have an obsolete configuration in its base config when it is
             * still a follower.
             */
            if (!leader) {
                WT_ERR_MSG_CHK(session,
                  __wti_disagg_pick_up_checkpoint_meta(session, cval.str, cval.len),
                  "Failed to pick up a new checkpoint with config: %.*s", (int)cval.len, cval.str);
            }
        }
    }

    /* Common settings between initial connection config and reconfig. */

    /* Get the last materialized LSN. */
    /* FIXME-WT-15447 Consider deprecating this. */
    WT_ERR_NOTFOUND_OK(
      __wt_config_gets(session, cfg, "disaggregated.last_materialized_lsn", &cval), true);
    if (ret == 0 && cval.len > 0 && cval.val >= 0)
        WT_ERR_MSG_CHK(session, __wti_disagg_set_last_materialized_lsn(session, (uint64_t)cval.val),
          "Failed to set the last materialized LSN to %" PRIu64, (uint64_t)cval.val);

    /* Get the configured role. */
    WT_ERR(__wt_disagg_config_get_role(session, cfg, &leader));

    if (!reconfig) {
        /* Set the initial role. */
        conn->layered_table_manager.leader = leader;
        WT_STAT_CONN_SET(session, disagg_role_leader, leader ? 1 : 0);
    } else if (!was_leader && leader) {
        /* Follower step-up. */
        time_start = __wt_clock(session);
        WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_step_up(session));
        time_stop = __wt_clock(session);
        WT_ERR_MSG_CHK(session, ret, "Failed to step up to the leader role");
        WT_STAT_CONN_SET(session, disagg_step_up_time, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Step up completed in %" PRIu64 " milliseconds", WT_CLOCKDIFF_MS(time_stop, time_start));
    } else if (was_leader && !leader) {
        /* Leader step-down. */
        time_start = __wt_clock(session);
        WT_WITH_CHECKPOINT_LOCK(session, __disagg_step_down(session));
        time_stop = __wt_clock(session);
        WT_STAT_CONN_SET(session, disagg_step_down_time, WT_CLOCKDIFF_MS(time_stop, time_start));
        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Step down completed in %" PRIu64 " milliseconds",
          WT_CLOCKDIFF_MS(time_stop, time_start));
    }
    /* Connection init settings only. */

    if (reconfig)
        goto err;

    /* Remember the configuration. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->disaggregated_storage.page_log));

    /* Setup any configured page log. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_schema_open_page_log(session, &cval, &npage_log));
    conn->disaggregated_storage.npage_log = npage_log;

    if (npage_log != NULL) {
        /* Set up a handle for accessing shared metadata. */
        WT_ERR(npage_log->page_log->pl_open_handle(npage_log->page_log, &session->iface,
          WT_SPECIAL_PALI_TURTLE_FILE_ID, &conn->disaggregated_storage.page_log_meta));

        /* Set up a handle for accessing the key provider table if configured. */
        if (conn->key_provider != NULL)
            WT_ERR(npage_log->page_log->pl_open_handle(npage_log->page_log, &session->iface,
              WT_SPECIAL_PALI_KEY_PROVIDER_FILE_ID,
              &conn->disaggregated_storage.page_log_key_provider));
    }

    /* FIXME-WT-14965: Exit the function immediately if this check returns false. */
    if (__wt_conn_is_disagg(session)) {
        /*
         * FIXME-WT-17177: Read-only connections are currently not supported with disaggregated
         * storage.
         */
        if (F_ISSET(conn, WT_CONN_READONLY))
            WT_ERR_MSG(session, ENOTSUP,
              "disaggregated storage is not supported with read-only connections");

        WT_ERR(__wti_layered_table_manager_init(session));

        /* If we are starting as a primary, abandon a previous incomplete checkpoint. */
        if (leader) {
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_abandon_checkpoint(session));
            WT_ERR_MSG_CHK(session, ret, "Failed to abandon the incomplete checkpoint");
        }

        /* Initialize the shared metadata table. */
        WT_ERR(__disagg_metadata_table_init(session));

        /* Pick up the selected checkpoint. */
        WT_ERR_NOTFOUND_OK(
          __wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval), true);
        if (ret == 0 && cval.len > 0) {
            WT_ERR_MSG_CHK(session,
              __wti_disagg_pick_up_checkpoint_meta(session, cval.str, cval.len),
              "Failed to pick up a new checkpoint with config: %.*s", (int)cval.len, cval.str);
            picked_up = true;
        }

        /* If we are starting as primary (e.g., for internal testing), begin the checkpoint. */
        if (leader && !picked_up) {
            ret = __wti_layered_get_disagg_checkpoint(
              session, cfg, NULL, NULL, &complete_checkpoint_meta);
            WT_ERR_NOTFOUND_OK(ret, true);
            if (ret == 0) {
                /* Pick up the checkpoint we just found. */
                ret = __wti_disagg_pick_up_checkpoint_meta(
                  session, complete_checkpoint_meta.data, complete_checkpoint_meta.size);

                __wt_buf_free(session, &complete_checkpoint_meta);
                WT_ERR_MSG_CHK(session, ret, "Failed to pick up checkpoint metadata");
            } else if (WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
                __wt_verbose_debug2(session, WT_VERB_DISAGGREGATED_STORAGE, "%s",
                  "Did not find any complete checkpoint to pick up at startup");
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_begin_checkpoint(session));
            WT_ERR_MSG_CHK(session, ret, "Failed to begin a new checkpoint");
        }

        WT_ERR(__wt_config_gets(session, cfg, "page_delta.internal_page_delta", &cval));
        if (cval.val != 0)
            F_SET(&conn->page_delta, WT_INTERNAL_PAGE_DELTA);

        WT_ERR(__wt_config_gets(session, cfg, "page_delta.leaf_page_delta", &cval));
        if (cval.val != 0)
            F_SET(&conn->page_delta, WT_LEAF_PAGE_DELTA);

        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.lose_all_my_data", &cval));
        if (cval.val != 0)
            F_SET(&conn->disaggregated_storage, WT_DISAGG_NO_SYNC);

        /*
         * Get the percentage of a page size that a delta must be less than in order to write that
         * delta (instead of just giving up and writing the full page).
         */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.delta_pct", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.delta_pct = (uint32_t)cval.val;

        /* Get the threshold fraction of keys removed from the disk image to force a full page. */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.delete_pct", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.delete_pct = (uint32_t)cval.val;

        /* Get the maximum number of consecutive deltas allowed for a single page. */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.max_consecutive_delta", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.max_consecutive_delta = (uint32_t)cval.val;

        /* Get the number of threads used to drain the ingest tables. */
        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.drain_threads", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->layered_drain_data.thread_count = (uint32_t)cval.val;
    }

err:
    /* Dump available logged errors into the event handler to ease debugging. */
    if (ret != 0)
        __wt_error_log_to_handler(session);

    if (ret != 0 && reconfig && !was_leader && leader)
        return (__wt_panic(session, ret, "failed to step-up as primary"));
    return (ret);
}

/*
 * __wt_conn_is_disagg --
 *     Check whether the connection uses disaggregated storage.
 */
bool
__wt_conn_is_disagg(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    return (disagg->page_log_meta != NULL);
}

/*
 * __wt_disagg_has_picked_up_checkpoint --
 *     Return whether this connection is using disaggregated storage and has picked up a checkpoint.
 */
bool
__wt_disagg_has_picked_up_checkpoint(WT_SESSION_IMPL *session)
{
    WT_DISAGGREGATED_STORAGE *disagg = &S2C(session)->disaggregated_storage;

    return (__wt_conn_is_disagg(session) &&
      __wt_atomic_load_uint64_acquire(&disagg->last_checkpoint_meta_lsn) != WT_DISAGG_LSN_NONE);
}

/*
 * __remove_or_fail_local_wt_file --
 *     Remove a local WiredTiger file or fail with EEXIST, depending on the configured action.
 */
static int
__remove_or_fail_local_wt_file(WT_SESSION_IMPL *session, const char *fname, bool fail)
{
    if (fail)
        WT_RET_MSG(session, EEXIST,
          "Disaggregated storage requires a clean directory, but found WiredTiger file %s: "
          "use 'disaggregated.local_files_action=delete' to remove it.",
          fname);

    __wt_verbose_warning(
      session, WT_VERB_METADATA, "Removing local file due to disagg mode: %s", fname);
    WT_RET(__wt_fs_remove(session, fname, false, false));

    return (0);
}

/*
 * __ensure_clean_startup_dir --
 *     Check for local files in a directory that need to be removed before starting in disaggregated
 *     mode.
 */
static int
__ensure_clean_startup_dir(WT_SESSION_IMPL *session, const char *dir, bool fail)
{
    WT_DECL_RET;

    if (*dir != '\0') {
        bool exists;
        WT_RET(__wt_fs_exist(session, dir, &exists));
        if (!exists)
            return (0); /* Nothing to do, directory does not exist. */
    }

    u_int file_count = 0;
    char **files = NULL;
    WT_ERR(__wt_fs_directory_list(session, dir, "", &files, &file_count));

    if (file_count <= 0)
        goto err;

#ifndef MAXPATHLEN
#define MAXPATHLEN 1024
#endif

#ifndef _WIN32
    { /* Limit the scope of big local stack variables. */
        char cwd[MAXPATHLEN];
        if (getcwd(cwd, MAXPATHLEN) == NULL) {
            cwd[0] = '?';
            cwd[1] = '\0';
        }
        __wt_verbose_debug1(session, WT_VERB_METADATA,
          "Found %u local files in directory <%s> -> <%s>:", file_count, cwd, dir);
    }
#endif

    for (u_int i = 0; i < file_count; i++) {
        /* Build full file name */
        char full_path_buf[MAXPATHLEN];
        char *full_path;
        if (dir[0] != '\0') {
            WT_ERR(__wt_snprintf(full_path_buf, sizeof(full_path_buf), "%s%s%s", dir,
              __wt_path_separator(), files[i]));
            full_path = full_path_buf;
        } else
            full_path = files[i];

        struct stat sb;
        if (stat(full_path, &sb) == 0) {
            __wt_verbose_debug1(session, WT_VERB_METADATA,
              "File:  %s: size=%" WT_SIZET_FMT " mode=%03o uid=%u gid=%u mtime=%s", full_path,
              (size_t)sb.st_size, (u_int)sb.st_mode & 0777, (u_int)sb.st_uid, (u_int)sb.st_gid,
              ctime(&sb.st_mtime));
        } else
            __wt_verbose_debug1(
              session, WT_VERB_METADATA, "  %s: stat failed: %s", full_path, strerror(errno));

        /*
         * Delete any WiredTiger files to prevent reading them during startup. But keep
         * WiredTiger.lock as a safety mechanism.
         *
         * Prevent deleting stat files since they can be useful for debugging.
         */
        if (WT_PREFIX_MATCH(files[i], "WiredTiger") && !WT_STREQ(files[i], WT_SINGLETHREAD) &&
          !WT_PREFIX_MATCH(files[i], "WiredTigerStat"))
            WT_ERR(__remove_or_fail_local_wt_file(session, full_path, fail));
        else if (WT_SUFFIX_MATCH(files[i], ".wt") || WT_URI_IS_INGEST(files[i]) ||
          WT_URI_IS_STABLE(files[i]))
            /*
             * Delete all normal tables since they are not usable without metadata anyway.
             *
             * Delete ingest and stable tables as they are not guaranteed to be consistent. If they
             * are not deleted now, the files will be renamed and kept around - someone will have to
             * clean them up later.
             */
            WT_ERR(__remove_or_fail_local_wt_file(session, full_path, fail));
        else
            __wt_verbose_debug1(session, WT_VERB_METADATA, "Keeping local file: %s", full_path);
    }

err:
    WT_TRET(__wt_fs_directory_list_free(session, &files, file_count));
    return (ret);
}

/*
 * __wti_ensure_clean_startup_dir --
 *     Check for local files that need to be removed before starting in disaggregated mode.
 *
 * Disaggregated storage needs to start with a clean directory, for now wipe out the directory if
 *     starting in disaggregated storage mode. Eventually this should not be necessary but at the
 *     moment WiredTiger will generate local files in disaggregated storage mode, and MongoDB
 *     expects to be able to restart without files being present.
 *
 * FIXME-WT-15163: Revisit what files get written and what needs to be deleted.
 */
int
__wti_ensure_clean_startup_dir(WT_SESSION_IMPL *session, const char *cfg[])
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;

    /*
     * FIXME-WT-14721: As it stands, __wt_conn_is_disagg only works after we have metadata access,
     * which depends on having run recovery, so the config hack is the simplest way to break that
     * dependency.
     */
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    if (cval.len == 0)
        return (0); /* Not in disaggregated mode, nothing to do. */
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.lose_all_my_data", &cval));
    if (cval.val == 0)
        return (0);

    /*
     * Possible actions for local files are: fail, delete, ignore.
     *
     * A reasonable default for Disagg would be to delete all local WT-related files, since they can
     * be in an inconsistent state anyway. Since this only works together with the
     * "lose_all_my_data" option, it's considered to be safe enough to be triggered by accident.
     */
    bool fail;
    WT_RET(__wt_config_gets(session, cfg, "disaggregated.local_files_action", &cval));
    if (WT_CONFIG_LIT_MATCH("fail", cval))
        fail = true;
    else if (WT_CONFIG_LIT_MATCH("ignore", cval))
        return (0);
    else
        fail = false; /* Default: delete */

    /* Delete from home directory. */
    WT_RET(__ensure_clean_startup_dir(session, "", fail));

    /*
     * Delete from log directory.
     *
     * Since log manager is not initialized yet, read directly from config.
     */
    const char *log_path;
    WT_RET(__wt_config_gets(session, cfg, "log.path", &cval));
    if (cval.len > 0 && !(cval.len == 1 && cval.str[0] == '.')) {
        WT_RET(__wt_strndup(session, cval.str, cval.len, &log_path));
        ret = __ensure_clean_startup_dir(session, log_path, fail);
        __wt_free(session, log_path);
    }

    return (ret);
}

/*
 * __wti_disagg_destroy --
 *     Shut down disaggregated storage.
 */
int
__wti_disagg_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    /* Remove the list of URIs for which we still need to update metadata entries. */
    __disagg_shared_metadata_queue_clear(session);

    /* Close the metadata handles. */
    if (disagg->page_log_meta != NULL) {
        WT_TRET(disagg->page_log_meta->plh_close(disagg->page_log_meta, &session->iface));
        disagg->page_log_meta = NULL;
    }

    /* Close the key provider handle. */
    if (disagg->page_log_key_provider != NULL) {
        WT_TRET(
          disagg->page_log_key_provider->plh_close(disagg->page_log_key_provider, &session->iface));
        disagg->page_log_key_provider = NULL;
    }

    __wt_free(session, disagg->last_checkpoint_root);
    __wt_free(session, disagg->page_log);
    return (ret);
}

/*
 * __wt_disagg_advance_checkpoint --
 *     Advance to the next checkpoint. If the current checkpoint is 0, just start the next one.
 */
int
__wt_disagg_advance_checkpoint(WT_SESSION_IMPL *session, bool ckpt_success)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(meta);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    WT_PAGE_LOG_COMPLETE_CHECKPOINT_ARGS complete_args;
    wt_timestamp_t checkpoint_timestamp;
    uint64_t meta_lsn;
    uint32_t meta_checksum;
    char ts_string[WT_TS_INT_STRING_SIZE];

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;
    WT_CLEAR(complete_args);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can advance the global checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        return (0);

    WT_RET(__wt_scr_alloc(session, 0, &meta));

    /* Get the checksum of the metadata page. This access is protected by the checkpoint lock. */
    meta_checksum = conn->disaggregated_storage.last_checkpoint_meta_checksum;

    /* The following accesses are read from atomic variables. */
    meta_lsn =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.last_checkpoint_meta_lsn);
    checkpoint_timestamp =
      __wt_atomic_load_uint64_acquire(&conn->disaggregated_storage.cur_checkpoint_timestamp);
    WT_ASSERT(session, meta_lsn > 0); /* The metadata page should be written by now. */

    if (ckpt_success) {
        /*
         * Important: To keep testing simple, keep the metadata to be a valid configuration string
         * without quotation marks or escape characters.
         */
        WT_ERR_MSG_CHK(session,
          __wt_buf_fmt(session, meta,
            "metadata_lsn=%" PRIu64 ",metadata_checksum=%" PRIx32 ",database_size=%" PRIu64
            ",version=%d,compatible_version=%d",
            meta_lsn, meta_checksum, conn->disaggregated_storage.database_size,
            WT_DISAGG_CHECKPOINT_META_VERSION, WT_DISAGG_CHECKPOINT_META_COMPATIBLE_VERSION),
          "Failed to format checkpoint metadata");

        complete_args.checkpoint_id = 0;
        complete_args.checkpoint_timestamp = checkpoint_timestamp;
        complete_args.checkpoint_metadata = meta;
        complete_args.checkpoint_oldest_timestamp =
          conn->disaggregated_storage.last_checkpoint_oldest_timestamp;
        complete_args.lsn = 0;
        WT_ERR_MSG_CHK(session,
          disagg->npage_log->page_log->pl_complete_checkpoint(
            disagg->npage_log->page_log, &session->iface, &complete_args),
          "Failed to complete checkpoint");

        __wt_atomic_store_uint64_release(
          &conn->disaggregated_storage.last_checkpoint_timestamp, checkpoint_timestamp);

        __wt_verbose_debug1(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Completed disaggregated storage checkpoint: lsn=%" PRIu64 ", timestamp=%" PRIu64 " %s",
          meta_lsn, checkpoint_timestamp,
          __wt_timestamp_to_string(checkpoint_timestamp, ts_string));
    } else
        __wt_verbose_warning(session, WT_VERB_DISAGGREGATED_STORAGE,
          "Checkpoint completion skipped due to unsuccessful checkpoint: lsn=%" PRIu64
          ", timestamp=%" PRIu64 " %s",
          meta_lsn, checkpoint_timestamp,
          __wt_timestamp_to_string(checkpoint_timestamp, ts_string));

    WT_ERR_MSG_CHK(session, __disagg_begin_checkpoint(session), "Failed to begin a new checkpoint");

err:
    __wt_scr_free(session, &meta);
    return (ret);
}
