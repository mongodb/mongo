/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __wti_layered_table_manager_init --
 *     Start the layered table manager thread
 */
int
__wti_layered_table_manager_init(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    WT_ASSERT_ALWAYS(
      session, manager->init == false, "Layered table manager initialization conflict");

    WT_RET(__wt_spin_init(session, &manager->layered_table_lock, "layered table manager"));

    /* Allow for up to 1000 files to be allocated at start. */
    WT_WITH_SCHEMA_LOCK(session, manager->open_layered_table_count = conn->next_file_id + 1000);
    WT_ERR(__wt_calloc(session, sizeof(WT_LAYERED_TABLE_MANAGER_ENTRY *),
      manager->open_layered_table_count, &manager->entries));
    manager->entries_allocated_bytes =
      manager->open_layered_table_count * sizeof(WT_LAYERED_TABLE_MANAGER_ENTRY *);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_LAYERED);

    manager->init = true;
    return (0);

err:
    /* Quit the layered table server. */
    WT_TRET(__wti_layered_table_manager_destroy(session));
    return (ret);
}

/*
 * __wt_layered_table_manager_add_table --
 *     Add a table to the layered table manager when it's opened
 */
int
__wt_layered_table_manager_add_table(WT_SESSION_IMPL *session, uint32_t ingest_id)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE *layered;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    WT_ASSERT_ALWAYS(session, session->dhandle->type == WT_DHANDLE_TYPE_LAYERED,
      "Adding a layered tree to tracking without the right dhandle context.");
    layered = (WT_LAYERED_TABLE *)session->dhandle;

    WT_ASSERT_ALWAYS(
      session, manager->init, "Adding a layered table, but the manager isn't initialized");

    WT_ERR(__wt_calloc_one(session, &entry));
    /*
     * It's safe to just reference the same string. The lifecycle of the layered tree is longer than
     * it will live in the tracker here.
     */
    entry->stable_uri = layered->stable_uri;
    entry->ingest_uri = layered->ingest_uri;
    entry->layered_uri = session->dhandle->name;
    entry->ingest_id = ingest_id;

    __wt_spin_lock(session, &manager->layered_table_lock);
    WT_ASSERT(session, manager->open_layered_table_count > 0);
    if (ingest_id >= manager->open_layered_table_count) {
        WT_ERR(__wt_realloc_def(
          session, &manager->entries_allocated_bytes, ingest_id * 2, &manager->entries));
        manager->open_layered_table_count = ingest_id * 2;
    }

    /* Diagnostic sanity check - don't keep adding the same table */
    if (manager->entries[ingest_id] != NULL)
        WT_IGNORE_RET(__wt_panic(session, WT_PANIC,
          "Internal server error: opening the same layered table multiple times"));

    WT_STAT_CONN_INCR(session, layered_table_manager_tables);
    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "__wt_layered_table_manager_add_table uri=%s ingest=%" PRIu32 " name=%s", entry->stable_uri,
      ingest_id, session->dhandle->name);
    manager->entries[ingest_id] = entry;

err:
    __wt_spin_unlock(session, &manager->layered_table_lock);

    return (ret);
}

/*
 * __layered_table_manager_remove_table_inlock --
 *     Internal table remove implementation.
 */
static void
__layered_table_manager_remove_table_inlock(WT_SESSION_IMPL *session, uint32_t ingest_id)
{
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    manager = &S2C(session)->layered_table_manager;

    if ((entry = manager->entries[ingest_id]) != NULL) {
        WT_STAT_CONN_DECR(session, layered_table_manager_tables);
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "__wt_layered_table_manager_remove_table stable_uri=%s ingest_id=%" PRIu32,
          entry->stable_uri, ingest_id);

        WT_ASSERT(session, entry->pinned_dhandle == NULL);
        __wt_free(session, entry);
        manager->entries[ingest_id] = NULL;
    }
}

/*
 * __wt_layered_table_manager_remove_table --
 *     Remove a table to the layered table manager when it's opened. Note that it is always safe to
 *     remove a table from tracking immediately here. It will only be removed when the handle is
 *     closed and a handle is only closed after a checkpoint has completed that included all writes
 *     to the table. By that time the processor would have finished with any records from the
 *     layered table.
 */
void
__wt_layered_table_manager_remove_table(WT_SESSION_IMPL *session, uint32_t ingest_id)
{
    WT_LAYERED_TABLE_MANAGER *manager;

    manager = &S2C(session)->layered_table_manager;

    /* Shutdown calls this redundantly - ignore cases when the manager is already closed. */
    if (manager->init == false)
        return;

    __wt_spin_lock(session, &manager->layered_table_lock);
    __layered_table_manager_remove_table_inlock(session, ingest_id);

    __wt_spin_unlock(session, &manager->layered_table_lock);
}

/*
 * __wti_layered_table_manager_destroy --
 *     Destroy the layered table manager thread(s)
 */
int
__wti_layered_table_manager_destroy(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    uint32_t i;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    __wt_verbose_level(
      session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5, "%s", "__wti_layered_table_manager_destroy");

    if (manager->init == false)
        return (0);

    __wt_spin_lock(session, &manager->layered_table_lock);
    /* Ensure other things that engage with the layered table server know it's gone. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_LAYERED);

    /* Close any cursors and free any related memory */
    for (i = 0; i < manager->open_layered_table_count; i++) {
        if ((entry = manager->entries[i]) != NULL) {
            WT_ASSERT(session, entry->pinned_dhandle == NULL);
            __layered_table_manager_remove_table_inlock(session, i);
        }
    }
    __wt_free(session, manager->entries);
    manager->open_layered_table_count = 0;
    manager->entries_allocated_bytes = 0;

    manager->init = false;

    __wt_spin_unlock(session, &manager->layered_table_lock);
    __wt_spin_destroy(session, &manager->layered_table_lock);

    return (0);
}
