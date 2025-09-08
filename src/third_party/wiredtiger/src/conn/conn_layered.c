/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

static int __disagg_copy_shared_metadata_one(WT_SESSION_IMPL *session, const char *uri);
static int __layered_drain_ingest_tables(WT_SESSION_IMPL *session);
static int __layered_update_gc_ingest_tables_prune_timestamps(WT_SESSION_IMPL *session);
static int __layered_track_checkpoint(WT_SESSION_IMPL *session, uint64_t checkpoint_timestamp);
static int __layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order);

/*
 * __layered_get_disagg_checkpoint --
 *     Get existing checkpoint information from disaggregated storage.
 */
static int
__layered_get_disagg_checkpoint(WT_SESSION_IMPL *session, const char **cfg,
  uint64_t *complete_checkpoint_lsn, uint64_t *complete_checkpoint_timestamp,
  WT_ITEM *complete_checkpoint_metadata)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_PAGE_LOG *page_log;
    char *page_log_name;

    conn = S2C(session);
    page_log_name = NULL;

    /*
     * We need our own copy of the page log config string, it must be NULL terminated to look it up.
     */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &page_log_name));
    WT_ERR(conn->iface.get_page_log(&conn->iface, page_log_name, &page_log));

    /*
     * Getting the last opened checkpoint and the complete checkpoint from disaggregated storage are
     * only supported in test implementations of the page log interface. This function will never be
     * called in production.
     */
    if (page_log->pl_get_complete_checkpoint_ext == NULL)
        WT_ERR(ENOTSUP);

    WT_ERR(page_log->pl_get_complete_checkpoint_ext(page_log, &session->iface,
      complete_checkpoint_lsn, NULL, complete_checkpoint_timestamp, complete_checkpoint_metadata));

err:
    __wt_free(session, page_log_name);
    return (ret);
}

/*
 * __layered_create_missing_ingest_table --
 *     Create a missing ingest table from an existing layered table configuration.
 */
static int
__layered_create_missing_ingest_table(
  WT_SESSION_IMPL *session, const char *uri, const char *layered_cfg)
{
    WT_CONFIG_ITEM key_format, value_format;
    WT_DECL_ITEM(ingest_config);
    WT_DECL_RET;

    WT_ERR(__wt_config_getones(session, layered_cfg, "key_format", &key_format));
    WT_ERR(__wt_config_getones(session, layered_cfg, "value_format", &value_format));

    /* FIXME-WT-14728: Refactor this with __create_layered? */
    WT_ERR(__wt_scr_alloc(session, 0, &ingest_config));
    WT_ERR(__wt_buf_fmt(session, ingest_config,
      "key_format=\"%.*s\",value_format=\"%.*s\","
      "in_memory=true,log=(enabled=false),"
      "disaggregated=(page_log=none,storage_source=none)",
      (int)key_format.len, key_format.str, (int)value_format.len, value_format.str));

    WT_WITH_SCHEMA_LOCK(session, ret = __wt_schema_create(session, uri, ingest_config->data));

err:
    __wt_scr_free(session, &ingest_config);
    return (ret);
}

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
 * __layered_create_missing_stable_tables --
 *     Create missing stable tables.
 */
static int
__layered_create_missing_stable_tables(WT_SESSION_IMPL *session)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor_check, *cursor_scan;
    WT_DECL_RET;
    WT_SESSION_IMPL *internal_session;
    char *stable_uri;
    const char *layered_uri, *layered_cfg;

    conn = S2C(session);
    cursor_check = cursor_scan = NULL;
    stable_uri = NULL;

    WT_ERR(__wt_open_internal_session(conn, "disagg-step-up", false, 0, 0, &internal_session));
    WT_ERR(__wt_metadata_cursor(internal_session, &cursor_check));
    WT_ERR(__wt_metadata_cursor(internal_session, &cursor_scan));

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

        /* Create the stable table if it does not exist. */
        if (ret == WT_NOTFOUND) {
            WT_ERR(
              __layered_create_missing_stable_table(internal_session, stable_uri, layered_cfg));
            /* Ensure that we properly handle empty tables. */
            WT_ERR(__wt_disagg_copy_metadata_later(
              internal_session, stable_uri, layered_uri + strlen("layered:")));
        }

        __wt_free(session, stable_uri);
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    __wt_free(session, stable_uri);
    WT_TRET(__wt_metadata_cursor_release(internal_session, &cursor_check));
    WT_TRET(__wt_metadata_cursor_release(internal_session, &cursor_scan));
    WT_TRET(__wt_session_close_internal(internal_session));
    return (ret);
}

/*
 * __disagg_get_meta --
 *     Read metadata from disaggregated storage.
 */
static int
__disagg_get_meta(WT_SESSION_IMPL *session, uint64_t page_id, uint64_t lsn, WT_ITEM *item)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;
    WT_PAGE_LOG_GET_ARGS get_args;
    u_int count, retry;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;
    WT_CLEAR(get_args);
    get_args.lsn = lsn;

    if (disagg->page_log_meta != NULL) {
        retry = 0;
        for (;;) {
            count = 1;
            WT_RET(disagg->page_log_meta->plh_get(
              disagg->page_log_meta, &session->iface, page_id, 0, &get_args, item, &count));
            WT_ASSERT(session, count <= 1); /* Corrupt data. */

            /* Found the data. */
            if (count == 1)
                break;

            /* Otherwise retry up to 100 times to account for page materialization delay. */
            if (retry > 100)
                return (WT_NOTFOUND);
            __wt_verbose_notice(session, WT_VERB_READ,
              "retry #%" PRIu32 " for metadata page_id %" PRIu64 ", lsn %" PRIu64, retry, page_id,
              lsn);
            __wt_sleep(0, 10000 + retry * 5000);
            ++retry;
        }

        return (0);
    }

    return (ENOTSUP);
}

/*
 * __disagg_put_meta --
 *     Write metadata to disaggregated storage.
 */
static int
__disagg_put_meta(WT_SESSION_IMPL *session, uint64_t page_id, const WT_ITEM *item, uint64_t *lsnp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGGREGATED_STORAGE *disagg;
    WT_PAGE_LOG_PUT_ARGS put_args;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_CLEAR(put_args);
    if (disagg->page_log_meta == NULL)
        return (ENOTSUP);

    WT_RET(disagg->page_log_meta->plh_put(
      disagg->page_log_meta, &session->iface, page_id, 0, &put_args, item));
    if (lsnp != NULL)
        *lsnp = put_args.lsn;
    __wt_atomic_addv64(&disagg->num_meta_put, 1);
    return (0);
}

/*
 * __wt_disagg_put_checkpoint_meta --
 *     Write checkpoint information to the metadata page log and do the relevant bookkeeping.
 */
int
__wt_disagg_put_checkpoint_meta(WT_SESSION_IMPL *session, const char *checkpoint_root,
  size_t checkpoint_root_size, uint64_t checkpoint_timestamp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(buf);
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *disagg;
    uint64_t lsn;
    char *checkpoint_root_copy;

    buf = NULL;
    checkpoint_root_copy = NULL;
    conn = S2C(session);
    disagg = &conn->disaggregated_storage;
    lsn = 0;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    if (checkpoint_root == NULL) {
        WT_ASSERT(session, checkpoint_root_size == 0);
        checkpoint_root = "";
    }
    if (checkpoint_root_size == 0)
        checkpoint_root_size = strlen(checkpoint_root);

    WT_ERR(__wt_strndup(session, checkpoint_root, checkpoint_root_size, &checkpoint_root_copy));

    WT_ERR(__wt_scr_alloc(session, 0, &buf));
    WT_ERR(__wt_buf_fmt(session, buf,
      "%s\n"
      "timestamp=%" PRIx64,
      checkpoint_root_copy, checkpoint_timestamp));

    /*
     * Write the metadata to disaggregated storage. This should be the last statement in this
     * function that is allowed to fail.
     */
    WT_ERR(__disagg_put_meta(session, WT_DISAGG_METADATA_MAIN_PAGE_ID, buf, &lsn));

    /* Do the bookkeeping. */
    WT_RELEASE_WRITE(disagg->last_checkpoint_meta_lsn, lsn);
    WT_RELEASE_WRITE(disagg->last_checkpoint_timestamp, checkpoint_timestamp);

    __wt_free(session, disagg->last_checkpoint_root);
    disagg->last_checkpoint_root = checkpoint_root_copy;
    checkpoint_root_copy = NULL;

err:
    __wt_free(session, checkpoint_root_copy);
    __wt_scr_free(session, &buf);
    return (ret);
}

/*
 * __disagg_pick_up_checkpoint --
 *     Pick up a new checkpoint.
 */
static int
__disagg_pick_up_checkpoint(WT_SESSION_IMPL *session, uint64_t meta_lsn)
{
    WT_CONFIG_ITEM cval;
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *cursor, *md_cursor;
    WT_DECL_RET;
    WT_ITEM item;
    WT_SESSION_IMPL *internal_session, *shared_metadata_session;
    size_t len, metadata_value_cfg_len;
    uint64_t checkpoint_timestamp;
    char *buf, *cfg_ret, *checkpoint_config, *root, *metadata_value_cfg, *layered_ingest_uri;
    const char *cfg[3], *current_value, *metadata_key, *metadata_value;

    conn = S2C(session);

    buf = NULL;
    cursor = NULL;
    internal_session = NULL;
    md_cursor = NULL;
    metadata_key = NULL;
    metadata_value = NULL;
    metadata_value_cfg = NULL;
    layered_ingest_uri = NULL;
    root = NULL;
    shared_metadata_session = NULL;
    cfg_ret = NULL;
    WT_CLEAR(item);

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /*
     * Part 1: Get the metadata of the shared metadata table and insert it into our metadata table.
     */

    /* Read the checkpoint metadata of the shared metadata table from the special metadata page. */
    WT_ERR(__disagg_get_meta(session, WT_DISAGG_METADATA_MAIN_PAGE_ID, meta_lsn, &item));

    /* Add the terminating zero byte to the end of the buffer. */
    len = item.size + 1;
    WT_ERR(__wt_calloc_def(session, len, &buf)); /* This already zeroes out the buffer. */
    memcpy(buf, item.data, item.size);

    /* Parse out the checkpoint config string. */
    checkpoint_config = strchr(buf, '\n');
    if (checkpoint_config == NULL)
        WT_ERR_MSG(session, EINVAL, "Invalid checkpoint metadata: No checkpoint config string");
    *checkpoint_config = '\0';
    checkpoint_config++;

    /* Parse the checkpoint config. */
    WT_ERR(__wt_config_getones(session, checkpoint_config, "timestamp", &cval));
    if (cval.len > 0 && cval.val == 0)
        checkpoint_timestamp = WT_TS_NONE;
    else
        WT_ERR(
          __wt_txn_parse_timestamp(session, "checkpoint timestamp", &checkpoint_timestamp, &cval));

    /* Save the metadata key-value pair. */
    metadata_key = WT_DISAGG_METADATA_URI;
    metadata_value = root = buf;

    /* We need an internal session when modifying metadata. */
    WT_ERR(__wt_open_internal_session(conn, "checkpoint-pick-up", false, 0, 0, &internal_session));

    /* Open up a metadata cursor pointing at our table */
    WT_ERR(__wt_metadata_cursor(internal_session, &md_cursor));
    md_cursor->set_key(md_cursor, metadata_key);
    WT_ERR(md_cursor->search(md_cursor));

    /* Pull the value out. */
    WT_ERR(md_cursor->get_value(md_cursor, &current_value));
    len = strlen("checkpoint=") + strlen(metadata_value) + 1 /* for NUL */;

    /* Allocate/create a new config we're going to insert */
    metadata_value_cfg_len = len;
    WT_ERR(__wt_calloc_def(session, metadata_value_cfg_len, &metadata_value_cfg));
    WT_ERR(__wt_snprintf(metadata_value_cfg, len, "checkpoint=%s", metadata_value));
    cfg[0] = current_value;
    cfg[1] = metadata_value_cfg;
    cfg[2] = NULL;
    WT_ERR(__wt_config_collapse(session, cfg, &cfg_ret));

    /* Put our new config in */
    WT_ERR(__wt_metadata_insert(internal_session, metadata_key, cfg_ret));
    __wt_free(session, cfg_ret);

    /*
     * Part 2: Get the metadata for other tables from the shared metadata table.
     */

    /* We need a separate internal session to pick up the new checkpoint. */
    WT_ERR(__wt_open_internal_session(
      conn, "checkpoint-pick-up-shared", false, 0, 0, &shared_metadata_session));

    /*
     * Throw away any references to the old disaggregated metadata table. This ensures that we are
     * on the most recent checkpoint from now on.
     */
    WT_ERR(__wti_conn_dhandle_outdated(session, WT_DISAGG_METADATA_URI));

    cfg[0] = WT_CONFIG_BASE(session, WT_SESSION_open_cursor);
    cfg[1] = NULL;
    WT_ERR(__wt_open_cursor(shared_metadata_session, WT_DISAGG_METADATA_URI, NULL, cfg, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &metadata_key));
        WT_ERR(cursor->get_value(cursor, &metadata_value));

        md_cursor->set_key(md_cursor, metadata_key);
        WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);

        if (ret == 0 && WT_PREFIX_MATCH(metadata_key, "file:")) {
            /* Existing table: Just apply the new metadata. */
            WT_ERR(__wt_config_getones(session, metadata_value, "checkpoint", &cval));
            len = strlen("checkpoint=") + strlen(metadata_value) + 1 /* for NUL */;
            if (len > metadata_value_cfg_len) {
                metadata_value_cfg_len = len;
                WT_ERR(
                  __wt_realloc_noclear(session, NULL, metadata_value_cfg_len, &metadata_value_cfg));
            }
            WT_ERR(
              __wt_snprintf(metadata_value_cfg, len, "checkpoint=%.*s", (int)cval.len, cval.str));

            /* Merge the new checkpoint metadata into the current table metadata. */
            WT_ERR(md_cursor->get_value(md_cursor, &current_value));
            cfg[0] = current_value;
            cfg[1] = metadata_value_cfg;
            cfg[2] = NULL;
            WT_ERR(__wt_config_collapse(session, cfg, &cfg_ret));

            /* FIXME-WT-14730: check that the other parts of the metadata are identical. */

            /* Put our new config in */
            md_cursor->set_value(md_cursor, cfg_ret);
            WT_ERR(md_cursor->insert(md_cursor));

            /*
             * Mark any matching data handles to be out of date. Any new opens will get the new
             * metadata.
             */
            WT_ERR(__wti_conn_dhandle_outdated(session, metadata_key));
            __wt_free(session, cfg_ret);
        } else if (ret == WT_NOTFOUND) {
            /* New table: Insert new metadata. */
            /* FIXME-WT-14730: verify that there is no btree ID conflict. */

            /* Create the corresponding ingest table, if it does not exist. */
            if (WT_PREFIX_MATCH(metadata_key, "layered:")) {
                WT_ERR(__wt_config_getones(session, metadata_value, "ingest", &cval));
                if (cval.len > 0) {
                    WT_ERR(__wt_calloc_def(session, cval.len + 1, &layered_ingest_uri));
                    memcpy(layered_ingest_uri, cval.str, cval.len);
                    layered_ingest_uri[cval.len] = '\0';
                    md_cursor->set_key(md_cursor, layered_ingest_uri);
                    WT_ERR_NOTFOUND_OK(md_cursor->search(md_cursor), true);
                    if (ret == WT_NOTFOUND)
                        WT_ERR(__layered_create_missing_ingest_table(
                          internal_session, layered_ingest_uri, metadata_value));
                    __wt_free(session, layered_ingest_uri);
                }
            }

            /* Insert the actual metadata. */
            md_cursor->set_key(md_cursor, metadata_key);
            md_cursor->set_value(md_cursor, metadata_value);
            WT_ERR(md_cursor->insert(md_cursor));
        }
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    /*
     * Part 3: Do the bookkeeping.
     */

    /*
     * WiredTiger will reload the dir store's checkpoint when opening a cursor: Opening a file
     * cursor triggers __wt_btree_open (even if the file has been opened before).
     */
    WT_STAT_CONN_DSRC_INCR(session, layered_table_manager_checkpoints_refreshed);

    /*
     * Update the checkpoint metadata LSN. This doesn't require further synchronization, because the
     * updates are protected by the checkpoint lock.
     */
    WT_RELEASE_WRITE(conn->disaggregated_storage.last_checkpoint_meta_lsn, meta_lsn);

    /* Update the checkpoint timestamp. */
    WT_RELEASE_WRITE(conn->disaggregated_storage.last_checkpoint_timestamp, checkpoint_timestamp);

    /* Remember the root config of the last checkpoint. */
    __wt_free(session, conn->disaggregated_storage.last_checkpoint_root);
    WT_ERR(__wt_strdup(session, root, &conn->disaggregated_storage.last_checkpoint_root));

    /* Keep a record of past checkpoints, they will be needed for ingest garbage collection. */
    WT_ERR(__layered_track_checkpoint(session, checkpoint_timestamp));

    /* Update ingest tables' prune timestamps. */
    WT_ERR(__layered_update_gc_ingest_tables_prune_timestamps(internal_session));

err:
    /* Free memory allocated by the page log interface */
    __wt_free(session, item.mem);

    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(internal_session, &md_cursor));

    if (internal_session != NULL)
        WT_TRET(__wt_session_close_internal(internal_session));
    if (shared_metadata_session != NULL)
        WT_TRET(__wt_session_close_internal(shared_metadata_session));

    __wt_free(session, buf);
    __wt_free(session, metadata_value_cfg);
    __wt_free(session, layered_ingest_uri);
    __wt_free(session, cfg_ret);

    return (ret);
}

/*
 * __disagg_pick_up_checkpoint_meta --
 *     Pick up a new checkpoint from metadata config.
 */
static int
__disagg_pick_up_checkpoint_meta(WT_SESSION_IMPL *session, WT_CONFIG_ITEM *meta_item)
{
    WT_CONFIG_ITEM cval;
    uint64_t metadata_lsn;

    /* Extract the arguments. */
    WT_RET(__wt_config_subgets(session, meta_item, "metadata_lsn", &cval));
    metadata_lsn = (uint64_t)cval.val;

    /* Now actually pick up the checkpoint. */
    return (__disagg_pick_up_checkpoint(session, metadata_lsn));
}

/*
 * __disagg_pick_up_checkpoint_meta_item --
 *     Pick up a new checkpoint from metadata config, expressed as an item.
 */
static int
__disagg_pick_up_checkpoint_meta_item(WT_SESSION_IMPL *session, WT_ITEM *meta_item)
{
    WT_CONFIG_ITEM cval;
    WT_DECL_RET;
    uint64_t metadata_lsn;
    char *meta_str;

    meta_str = NULL;

    /* Extract the item into a string. */
    WT_ERR(__wt_strndup(session, meta_item->data, meta_item->size, &meta_str));

    /* Extract the arguments. */
    WT_ERR(__wt_config_getones(session, meta_str, "metadata_lsn", &cval));
    metadata_lsn = (uint64_t)cval.val;

    /* Now actually pick up the checkpoint. */
    WT_ERR(__disagg_pick_up_checkpoint(session, metadata_lsn));

err:
    __wt_free(session, meta_str);
    return (ret);
}

/*
 * __layered_table_manager_thread_chk --
 *     Check to decide if the layered table manager thread should continue running
 */
static bool
__layered_table_manager_thread_chk(WT_SESSION_IMPL *session)
{
    if (!S2C(session)->layered_table_manager.leader)
        return (false);
    return (__wt_atomic_load32(&S2C(session)->layered_table_manager.state) ==
      WT_LAYERED_TABLE_MANAGER_RUNNING);
}

/*
 * __layered_table_manager_thread_run --
 *     Entry function for a layered table manager thread. This is called repeatedly from the thread
 *     group code so it does not need to loop itself.
 */
static int
__layered_table_manager_thread_run(WT_SESSION_IMPL *session_shared, WT_THREAD *thread)
{
    WT_SESSION_IMPL *session;

    WT_UNUSED(session_shared);
    session = thread->session;
    WT_ASSERT(session, !WT_SESSION_IS_DEFAULT(session));

    WT_STAT_CONN_SET(session, layered_table_manager_active, 1);

    /* Right now we just sleep. In the future, do whatever we need to do here. */
    __wt_sleep(1, 0);

    WT_STAT_CONN_SET(session, layered_table_manager_active, 0);

    return (0);
}

/*
 * __layered_table_manager_start --
 *     Start the layered table manager thread
 */
static int
__layered_table_manager_start(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    uint32_t session_flags;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    WT_ASSERT_ALWAYS(session, manager->state == WT_LAYERED_TABLE_MANAGER_OFF,
      "Layered table manager initialization conflict");

    WT_RET(__wt_spin_init(session, &manager->layered_table_lock, "layered table manager"));

    /* Allow for up to 1000 files to be allocated at start. */
    manager->open_layered_table_count = conn->next_file_id + 1000;
    WT_ERR(__wt_calloc(session, sizeof(WT_LAYERED_TABLE_MANAGER_ENTRY *),
      manager->open_layered_table_count, &manager->entries));
    manager->entries_allocated_bytes =
      manager->open_layered_table_count * sizeof(WT_LAYERED_TABLE_MANAGER_ENTRY *);

    session_flags = WT_THREAD_CAN_WAIT | WT_THREAD_PANIC_FAIL;
    WT_ERR(__wt_thread_group_create(session, &manager->threads, "layered-table-manager",
      WT_LAYERED_TABLE_THREAD_COUNT, WT_LAYERED_TABLE_THREAD_COUNT, session_flags,
      __layered_table_manager_thread_chk, __layered_table_manager_thread_run, NULL));

    WT_STAT_CONN_SET(session, layered_table_manager_running, 1);
    __wt_verbose_level(
      session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5, "%s", "__layered_table_manager_start");
    FLD_SET(conn->server_flags, WT_CONN_SERVER_LAYERED);

    manager->state = WT_LAYERED_TABLE_MANAGER_RUNNING;
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

    WT_ASSERT_ALWAYS(session, manager->state == WT_LAYERED_TABLE_MANAGER_RUNNING,
      "Adding a layered table, but the manager isn't running");
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
    WT_ERR(__wt_calloc_one(session, &entry));
    entry->ingest_id = ingest_id;
    entry->layered_table = (WT_LAYERED_TABLE *)session->dhandle;

    /*
     * There is a bootstrapping problem. Use the global oldest ID as a starting point. Nothing can
     * have been written into the ingest table, so it will be a conservative choice.
     */
    entry->checkpoint_txn_id = __wt_atomic_loadv64(&conn->txn_global.oldest_id);

    /*
     * It's safe to just reference the same string. The lifecycle of the layered tree is longer than
     * it will live in the tracker here.
     */
    entry->stable_uri = layered->stable_uri;
    entry->ingest_uri = layered->ingest_uri;
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
    if (manager->state == WT_LAYERED_TABLE_MANAGER_OFF)
        return;

    __wt_spin_lock(session, &manager->layered_table_lock);
    __layered_table_manager_remove_table_inlock(session, ingest_id);

    __wt_spin_unlock(session, &manager->layered_table_lock);
}

/*
 * __layered_table_get_constituent_cursor --
 *     Retrieve or open a constituent cursor for a layered tree.
 */
static int
__layered_table_get_constituent_cursor(
  WT_SESSION_IMPL *session, uint32_t ingest_id, WT_CURSOR **cursorp)
{
    WT_CONNECTION_IMPL *conn;
    WT_CURSOR *stable_cursor;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;

    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL, NULL};

    conn = S2C(session);
    entry = conn->layered_table_manager.entries[ingest_id];

    *cursorp = NULL;

    if (entry == NULL)
        return (0);

    /* Open the cursor and keep a reference in the manager entry and our caller */
    WT_RET(__wt_open_cursor(session, entry->stable_uri, NULL, cfg, &stable_cursor));
    *cursorp = stable_cursor;

    return (0);
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
    uint32_t i;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    __wt_verbose_level(
      session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5, "%s", "__wti_layered_table_manager_destroy");

    if (__wt_atomic_load32(&manager->state) == WT_LAYERED_TABLE_MANAGER_OFF)
        return (0);

    /* Ensure other things that engage with the layered table server know it's gone. */
    FLD_CLR(conn->server_flags, WT_CONN_SERVER_LAYERED);

    /* Let any running threads finish up. */
    __wt_cond_signal(session, manager->threads.wait_cond);
    __wt_writelock(session, &manager->threads.lock);

    WT_RET(__wt_thread_group_destroy(session, &manager->threads));

    /* Close any cursors and free any related memory */
    for (i = 0; i < manager->open_layered_table_count; i++) {
        if (manager->entries[i] != NULL)
            __layered_table_manager_remove_table_inlock(session, i);
    }
    __wt_free(session, manager->entries);
    manager->open_layered_table_count = 0;
    manager->entries_allocated_bytes = 0;

    manager->state = WT_LAYERED_TABLE_MANAGER_OFF;
    WT_STAT_CONN_SET(session, layered_table_manager_running, 0);

    __wt_spin_destroy(session, &manager->layered_table_lock);

    return (0);
}

/*
 * __wt_disagg_copy_metadata_later --
 *     Copy the metadata that belongs to the given URI into the shared metadata table at the next
 *     checkpoint.
 */
int
__wt_disagg_copy_metadata_later(
  WT_SESSION_IMPL *session, const char *stable_uri, const char *table_name)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_COPY_METADATA *entry;

    conn = S2C(session);

    WT_RET(__wt_calloc_one(session, &entry));
    WT_RET(__wt_strdup(session, stable_uri, &entry->stable_uri));
    WT_RET(__wt_strdup(session, table_name, &entry->table_name));

    __wt_spin_lock(session, &conn->disaggregated_storage.copy_metadata_lock);
    TAILQ_INSERT_TAIL(&conn->disaggregated_storage.copy_metadata_qh, entry, q);
    __wt_spin_unlock(session, &conn->disaggregated_storage.copy_metadata_lock);

    return (0);
}

/*
 * __disagg_copy_metadata_clear --
 *     Clear the copy metadata list.
 */
static void
__disagg_copy_metadata_clear(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DISAGG_COPY_METADATA *entry, *tmp;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->disaggregated_storage.copy_metadata_lock);

    WT_TAILQ_SAFE_REMOVE_BEGIN(entry, &conn->disaggregated_storage.copy_metadata_qh, q, tmp)
    {
        TAILQ_REMOVE(&conn->disaggregated_storage.copy_metadata_qh, entry, q);
        __wt_free(session, entry->stable_uri);
        __wt_free(session, entry->table_name);
        __wt_free(session, entry);
    }
    WT_TAILQ_SAFE_REMOVE_END

    __wt_spin_unlock(session, &conn->disaggregated_storage.copy_metadata_lock);
}

/*
 * __wt_disagg_copy_metadata_process --
 *     Process the copy metadata list.
 */
int
__wt_disagg_copy_metadata_process(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGG_COPY_METADATA *entry, *tmp;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->disaggregated_storage.copy_metadata_lock);

    WT_TAILQ_SAFE_REMOVE_BEGIN(entry, &conn->disaggregated_storage.copy_metadata_qh, q, tmp)
    {
        WT_ERR(__disagg_copy_shared_metadata_one(session, entry->stable_uri));
        WT_ERR(__wt_disagg_copy_shared_metadata_layered(session, entry->table_name));

        TAILQ_REMOVE(&conn->disaggregated_storage.copy_metadata_qh, entry, q);
        __wt_free(session, entry->stable_uri);
        __wt_free(session, entry->table_name);
        __wt_free(session, entry);
    }
    WT_TAILQ_SAFE_REMOVE_END

err:
    __wt_spin_unlock(session, &conn->disaggregated_storage.copy_metadata_lock);

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
    WT_ACQUIRE_READ(cur_lsn, disagg->last_materialized_lsn);

    if (cur_lsn > lsn)
        return (EINVAL); /* Can't go backwards. */

    WT_RELEASE_WRITE(disagg->last_materialized_lsn, lsn);
    return (0);
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

    WT_RET(disagg->npage_log->page_log->pl_begin_checkpoint(
      disagg->npage_log->page_log, &session->iface, 0));

    /* Store is sufficient because updates are protected by the checkpoint lock. */
    disagg->num_meta_put_at_ckpt_begin = disagg->num_meta_put;
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
    bool leader, picked_up, was_leader;

    conn = S2C(session);
    leader = was_leader = conn->layered_table_manager.leader;
    npage_log = NULL;
    picked_up = false;

    WT_CLEAR(complete_checkpoint_meta);

    /* Reconfigure-only settings. */
    if (reconfig) {

        /* Pick up a new checkpoint (followers only). */
        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval));
        if (cval.len > 0) {
            /*
             * FIXME-WT-14733: currently the leader silently ignores the checkpoint_meta
             * configuration as it may have an obsolete configuration in its base config when it is
             * still a follower.
             */
            if (!leader) {
                WT_WITH_CHECKPOINT_LOCK(
                  session, ret = __disagg_pick_up_checkpoint_meta(session, &cval));
                WT_ERR(ret);
            }
        }
    }

    /* Common settings between initial connection config and reconfig. */

    /* Get the last materialized LSN. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.last_materialized_lsn", &cval));
    if (cval.len > 0 && cval.val >= 0)
        WT_ERR(__wti_disagg_set_last_materialized_lsn(session, (uint64_t)cval.val));

    /* Set the role. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.role", &cval));
    if (cval.len == 0)
        conn->layered_table_manager.leader = leader = false;
    else {
        if (WT_CONFIG_LIT_MATCH("follower", cval))
            conn->layered_table_manager.leader = leader = false;
        else if (WT_CONFIG_LIT_MATCH("leader", cval))
            conn->layered_table_manager.leader = leader = true;
        else
            WT_ERR_MSG(session, EINVAL, "Invalid node role");

        /* Follower step-up. */
        if (reconfig && !was_leader && leader) {
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_begin_checkpoint(session));
            WT_ERR(ret);

            /* Create any missing stable tables. */
            WT_ERR(__layered_create_missing_stable_tables(session));

            /* Drain the ingest tables before switching to leader. */
            WT_ERR(__layered_drain_ingest_tables(session));
        }

        /* Leader step-down. */
        if (reconfig && was_leader && !leader)
            /* Do some cleanup as we are abandoning the current checkpoint. */
            __disagg_copy_metadata_clear(session);
    }
    WT_STAT_CONN_SET(session, disagg_role_leader, leader ? 1 : 0);

    /* Connection init settings only. */

    if (reconfig)
        return (0);

    /* Remember the configuration. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_strndup(session, cval.str, cval.len, &conn->disaggregated_storage.page_log));

    /* Setup any configured page log. */
    WT_ERR(__wt_config_gets(session, cfg, "disaggregated.page_log", &cval));
    WT_ERR(__wt_schema_open_page_log(session, &cval, &npage_log));
    conn->disaggregated_storage.npage_log = npage_log;

    /* Set up a handle for accessing shared metadata. */
    if (npage_log != NULL) {
        WT_ERR(npage_log->page_log->pl_open_handle(npage_log->page_log, &session->iface,
          WT_DISAGG_METADATA_TABLE_ID, &conn->disaggregated_storage.page_log_meta));
    }

    /* FIXME-WT-14965: Exit the function immediately if this check returns false. */
    if (__wt_conn_is_disagg(session)) {
        WT_ERR(__layered_table_manager_start(session));

        /* Initialize the shared metadata table. */
        WT_ERR(__disagg_metadata_table_init(session));

        /* Pick up the selected checkpoint. */
        WT_ERR(__wt_config_gets(session, cfg, "disaggregated.checkpoint_meta", &cval));
        if (cval.len > 0) {
            WT_WITH_CHECKPOINT_LOCK(
              session, ret = __disagg_pick_up_checkpoint_meta(session, &cval));
            WT_ERR(ret);
            picked_up = true;
        }

        /* If we are starting as primary (e.g., for internal testing), begin the checkpoint. */
        if (leader && !picked_up) {
            WT_ERR(__wt_config_gets(session, cfg, "create", &cval));
            if (cval.val == 0) {
                ret = __layered_get_disagg_checkpoint(
                  session, cfg, NULL, NULL, &complete_checkpoint_meta);
                if (ret == WT_NOTFOUND)
                    WT_ERR_MSG(session, ret, "disaggregated checkpoint not found.");
                WT_ERR(ret);

                WT_WITH_CHECKPOINT_LOCK(session,
                  ret = __disagg_pick_up_checkpoint_meta_item(session, &complete_checkpoint_meta));

                __wt_buf_free(session, &complete_checkpoint_meta);
                WT_ERR(ret);
            }
            WT_WITH_CHECKPOINT_LOCK(session, ret = __disagg_begin_checkpoint(session));
            WT_ERR(ret);
        }

        WT_ERR(__wt_config_gets(session, cfg, "page_delta.flatten_leaf_page_delta", &cval));
        if (cval.val != 0)
            F_SET(&conn->page_delta, WT_FLATTEN_LEAF_PAGE_DELTA);

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

        /* Get the maximum number of consecutive deltas allowed for a single page. */
        WT_ERR(__wt_config_gets(session, cfg, "page_delta.max_consecutive_delta", &cval));
        if (cval.len > 0 && cval.val >= 0)
            conn->page_delta.max_consecutive_delta = (uint32_t)cval.val;
    }

err:
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
 * __on_file_in_wt_dir --
 *     Act on a file in WT directory: delete or fail depending on the flag.
 */
static int
__on_file_in_wt_dir(WT_SESSION_IMPL *session, const char *fname, bool fail)
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
         */
        if (WT_PREFIX_MATCH(files[i], "WiredTiger") && !WT_STREQ(files[i], WT_SINGLETHREAD))
            WT_ERR(__on_file_in_wt_dir(session, full_path, fail));
        else if (WT_SUFFIX_MATCH(files[i], ".wt") || WT_SUFFIX_MATCH(files[i], ".wt_ingest") ||
          WT_SUFFIX_MATCH(files[i], ".wt_stable"))
            /*
             * Delete all normal tables since they are not usable without metadata anyway.
             *
             * Delete ingest and stable tables as they are not guaranteed to be consistent. If they
             * are not deleted now, the files will be renamed and kept around - someone will have to
             * clean them up later.
             */
            WT_ERR(__on_file_in_wt_dir(session, full_path, fail));
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

    /* Remove the list of URIs for which we still need to copy metadata entries. */
    __disagg_copy_metadata_clear(session);

    /* Close the metadata handles. */
    if (disagg->page_log_meta != NULL) {
        WT_TRET(disagg->page_log_meta->plh_close(disagg->page_log_meta, &session->iface));
        disagg->page_log_meta = NULL;
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
    wt_timestamp_t checkpoint_timestamp;
    uint64_t meta_lsn;

    conn = S2C(session);
    disagg = &conn->disaggregated_storage;

    WT_ASSERT_SPINLOCK_OWNED(session, &conn->checkpoint_lock);

    /* Only the leader can advance the global checkpoint. */
    if (disagg->npage_log == NULL || !conn->layered_table_manager.leader)
        return (0);

    WT_RET(__wt_scr_alloc(session, 0, &meta));
    WT_ACQUIRE_READ(meta_lsn, conn->disaggregated_storage.last_checkpoint_meta_lsn);
    WT_ACQUIRE_READ(checkpoint_timestamp, conn->disaggregated_storage.cur_checkpoint_timestamp);
    WT_ASSERT(session, meta_lsn > 0); /* The metadata page should be written by now. */

    if (ckpt_success) {
        /*
         * Important: To keep testing simple, keep the metadata to be a valid configuration string
         * without quotation marks or escape characters.
         */
        WT_ERR(__wt_buf_fmt(session, meta, "metadata_lsn=%" PRIu64, meta_lsn));
        WT_ERR(disagg->npage_log->page_log->pl_complete_checkpoint_ext(disagg->npage_log->page_log,
          &session->iface, 0, (uint64_t)checkpoint_timestamp, meta, NULL));
        WT_RELEASE_WRITE(
          conn->disaggregated_storage.last_checkpoint_timestamp, checkpoint_timestamp);
    }

    WT_ERR(__disagg_begin_checkpoint(session));

err:
    __wt_scr_free(session, &meta);
    return (ret);
}

/*
 * __layered_move_updates --
 *     Move the updates of a key to the stable table
 */
static int
__layered_move_updates(
  WT_SESSION_IMPL *session, WT_CURSOR_BTREE *cbt, WT_ITEM *key, WT_UPDATE *upds)
{
    WT_DECL_RET;

    /* Search the page. */
    WT_WITH_PAGE_INDEX(session, ret = __wt_row_search(cbt, key, true, NULL, false, NULL));
    WT_ERR(ret);

    /* Apply the modification. */
    WT_ERR(__wt_row_modify(cbt, key, NULL, &upds, WT_UPDATE_INVALID, false, false));

err:
    WT_TRET(__wt_btcur_reset(cbt));
    return (ret);
}

/*
 * __layered_clear_ingest_table --
 *     After ingest content has been drained to the stable table, clear out the ingest table.
 */
static int
__layered_clear_ingest_table(WT_SESSION_IMPL *session, const char *uri)
{
    WT_ASSERT(session, WT_SUFFIX_MATCH(uri, ".wt_ingest"));

    /*
     * Truncate needs a running txn. We should probably do something more like the history store and
     * make this non-transactional -- this happens during step-up, so we know there are no other
     * transactions running, so it's safe.
     */
    WT_RET(__wt_txn_begin(session, NULL));

    /*
     * No other transactions are running, we're only doing this truncate, and it should become
     * immediately visible. So this transaction doesn't have to care about timestamps.
     */
    F_SET(session->txn, WT_TXN_TS_NOT_SET);

    WT_RET(session->iface.truncate(&session->iface, uri, NULL, NULL, NULL));

    WT_RET(__wt_txn_commit(session, NULL));

    return (0);
}

/*
 * __layered_copy_ingest_table --
 *     Moving all the data from a single ingest table to the corresponding stable table
 */
static int
__layered_copy_ingest_table(WT_SESSION_IMPL *session, WT_LAYERED_TABLE_MANAGER_ENTRY *entry)
{
    WT_CURSOR *stable_cursor, *version_cursor;
    WT_CURSOR_BTREE *cbt;
    WT_DECL_ITEM(key);
    WT_DECL_ITEM(tmp_key);
    WT_DECL_ITEM(value);
    WT_DECL_RET;
    WT_TIME_WINDOW tw;
    WT_UPDATE *prev_upd, *tombstone, *upd, *upds;
    wt_timestamp_t last_checkpoint_timestamp;
    uint8_t flags, location, prepare, type;
    int cmp;
    char buf[256], buf2[64];
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL, NULL, NULL};

    stable_cursor = version_cursor = NULL;
    prev_upd = tombstone = upd = upds = NULL;
    WT_TIME_WINDOW_INIT(&tw);

    WT_ACQUIRE_READ(
      last_checkpoint_timestamp, S2C(session)->disaggregated_storage.last_checkpoint_timestamp);
    WT_RET(__layered_table_get_constituent_cursor(session, entry->ingest_id, &stable_cursor));
    cbt = (WT_CURSOR_BTREE *)stable_cursor;
    if (last_checkpoint_timestamp != WT_TS_NONE)
        WT_ERR(__wt_snprintf(
          buf2, sizeof(buf2), "start_timestamp=%" PRIx64 "", last_checkpoint_timestamp));
    else
        buf2[0] = '\0';
    WT_ERR(__wt_snprintf(buf, sizeof(buf),
      "debug=(dump_version=(enabled=true,raw_key_value=true,visible_only=true,timestamp_order=true,"
      "%s))",
      buf2));
    cfg[1] = buf;
    WT_ERR(__wt_open_cursor(session, entry->ingest_uri, NULL, cfg, &version_cursor));

    WT_ERR(__wt_scr_alloc(session, 0, &key));
    WT_ERR(__wt_scr_alloc(session, 0, &tmp_key));
    WT_ERR(__wt_scr_alloc(session, 0, &value));

    for (;;) {
        tombstone = upd = NULL;
        WT_ERR_NOTFOUND_OK(version_cursor->next(version_cursor), true);
        if (ret == WT_NOTFOUND) {
            if (key->size > 0 && upds != NULL) {
                WT_WITH_DHANDLE(
                  session, cbt->dhandle, ret = __layered_move_updates(session, cbt, key, upds));
                WT_ERR(ret);
                upds = NULL;
            } else
                ret = 0;
            break;
        }

        WT_ERR(version_cursor->get_key(version_cursor, tmp_key));
        WT_ERR(__wt_compare(session, CUR2BT(cbt)->collator, key, tmp_key, &cmp));
        if (cmp != 0) {
            WT_ASSERT(session, cmp <= 0);

            if (upds != NULL) {
                WT_WITH_DHANDLE(
                  session, cbt->dhandle, ret = __layered_move_updates(session, cbt, key, upds));
                WT_ERR(ret);
            }

            upds = NULL;
            prev_upd = NULL;
            WT_ERR(__wt_buf_set(session, key, tmp_key->data, tmp_key->size));
        }

        WT_ERR(version_cursor->get_value(version_cursor, &tw.start_txn, &tw.start_ts,
          &tw.durable_start_ts, &tw.stop_txn, &tw.stop_ts, &tw.durable_stop_ts, &type, &prepare,
          &flags, &location, value));
        /* We shouldn't see any prepared updates. */
        WT_ASSERT(session, prepare == 0);

        /* We assume the updates returned will be in timestamp order. */
        if (prev_upd != NULL) {
            /* If we see a single tombstone in the previous iteration, we must be reaching the end
             * and should never be here. */
            WT_ASSERT(session, prev_upd->type == WT_UPDATE_STANDARD);
            WT_ASSERT(session,
              tw.stop_txn <= prev_upd->txnid && tw.stop_ts <= prev_upd->upd_start_ts &&
                tw.durable_stop_ts <= prev_upd->upd_durable_ts);
            WT_ASSERT(session,
              tw.start_txn <= prev_upd->txnid && tw.start_ts <= prev_upd->upd_start_ts &&
                tw.durable_start_ts <= prev_upd->upd_durable_ts);
            if (tw.stop_txn != prev_upd->txnid || tw.stop_ts != prev_upd->upd_start_ts ||
              tw.durable_stop_ts != prev_upd->upd_durable_ts)
                WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, NULL));
        } else if (WT_TIME_WINDOW_HAS_STOP(&tw))
            WT_ERR(__wt_upd_alloc_tombstone(session, &tombstone, NULL));

        /*
         * It is possible to see a full value that is smaller than or equal to the last checkpoint
         * timestamp with a tombstone that is larger than the last checkpoint timestamp. Ignore the
         * update in this case.
         */
        if (tw.durable_start_ts > last_checkpoint_timestamp) {
            /* FIXME-WT-14732: this is an ugly layering violation. But I can't think of a better way
             * now. */
            if (__wt_clayered_deleted(value)) {
                /*
                 * If we use tombstone value, we should never see a real tombstone on the ingest
                 * table.
                 */
                WT_ASSERT(session, tombstone == NULL);
                WT_ERR(__wt_upd_alloc_tombstone(session, &upd, NULL));
            } else
                WT_ERR(__wt_upd_alloc(session, value, WT_UPDATE_STANDARD, &upd, NULL));
            upd->txnid = tw.start_txn;
            upd->upd_start_ts = tw.start_ts;
            upd->upd_durable_ts = tw.durable_start_ts;
            upd->prepare_ts = tw.start_prepare_ts;
            upd->prepared_id = tw.start_prepared_id;
        } else
            WT_ASSERT(session, tombstone != NULL);

        /* FIXME-WT-14732: we can simplify the algorithm if we don't use real tombstones on the
         * ingest table. */
        if (tombstone != NULL) {
            tombstone->txnid = tw.stop_txn;
            tombstone->upd_start_ts = tw.stop_ts;
            tombstone->upd_durable_ts = tw.durable_stop_ts;
            tombstone->prepare_ts = tw.stop_prepare_ts;
            tombstone->prepared_id = tw.stop_prepared_id;
            tombstone->next = upd;

            WT_ASSERT(session, tombstone->upd_durable_ts > last_checkpoint_timestamp);

            if (prev_upd != NULL)
                prev_upd->next = tombstone;
            else
                upds = tombstone;

            prev_upd = upd;
            tombstone = NULL;
            upd = NULL;
        } else {
            if (prev_upd != NULL)
                prev_upd->next = upd;
            else
                upds = upd;

            prev_upd = upd;
            upd = NULL;
        }
    }

err:
    if (tombstone != NULL)
        __wt_free(session, tombstone);
    if (upd != NULL)
        __wt_free(session, upd);
    if (upds != NULL)
        __wt_free_update_list(session, &upds);
    __wt_scr_free(session, &key);
    __wt_scr_free(session, &tmp_key);
    __wt_scr_free(session, &value);
    if (version_cursor != NULL)
        WT_TRET(version_cursor->close(version_cursor));
    if (stable_cursor != NULL)
        WT_TRET(stable_cursor->close(stable_cursor));
    return (ret);
}

/*
 * __layered_drain_ingest_tables --
 *     Moving all the data from the ingest tables to the stable tables
 */
static int
__layered_drain_ingest_tables(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    WT_SESSION_IMPL *internal_session;
    size_t i, table_count;

    conn = S2C(session);
    manager = &conn->layered_table_manager;

    WT_RET(__wt_open_internal_session(conn, "disagg-drain", false, 0, 0, &internal_session));

    __wt_spin_lock(session, &manager->layered_table_lock);

    table_count = manager->open_layered_table_count;

    /*
     * FIXME-WT-14734: shouldn't we hold this lock longer, e.g. manager->entries could get
     * reallocated, or individual entries could get removed or freed.
     */
    __wt_spin_unlock(session, &manager->layered_table_lock);

    /* FIXME-WT-14735: skip empty ingest tables. */
    for (i = 0; i < table_count; i++) {
        if ((entry = manager->entries[i]) != NULL) {
            WT_ERR(__layered_copy_ingest_table(internal_session, entry));
            WT_ERR(__layered_clear_ingest_table(internal_session, entry->ingest_uri));
        }
    }

err:
    WT_TRET(__wt_session_close_internal(internal_session));
    return (ret);
}

/*
 * __layered_update_gc_ingest_tables_prune_timestamps --
 *     Update the timestamp we can prune the ingest tables.
 */
static int
__layered_update_gc_ingest_tables_prune_timestamps(WT_SESSION_IMPL *session)
{
    WT_BTREE *btree;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *ds;
    WT_LAYERED_TABLE *layered_table;
    WT_LAYERED_TABLE_MANAGER *manager;
    WT_LAYERED_TABLE_MANAGER_ENTRY *entry;
    wt_timestamp_t prune_timestamp;
    size_t i, len, table_count, uri_alloc;
    int64_t ckpt_inuse, last_ckpt, min_ckpt_inuse;
    uint32_t track;
    char *uri_at_checkpoint;

    conn = S2C(session);
    manager = &conn->layered_table_manager;
    ds = &conn->disaggregated_storage;
    min_ckpt_inuse = ds->ckpt_track_cnt;
    uri_at_checkpoint = NULL;
    uri_alloc = 0;

    WT_ASSERT(session, manager->state == WT_LAYERED_TABLE_MANAGER_RUNNING);

    __wt_spin_lock(session, &manager->layered_table_lock);

    table_count = manager->open_layered_table_count;

    for (i = 0; i < table_count; i++) {
        if ((entry = manager->entries[i]) != NULL) {
            layered_table = entry->layered_table;
            WT_ERR_NOTFOUND_OK(
              __layered_last_checkpoint_order(session, layered_table->stable_uri, &last_ckpt),
              true);
            /*
             * If we've never seen a checkpoint, then there's nothing in the ingest table we can
             * remove. Move on.
             */
            if (ret == WT_NOTFOUND)
                continue;

            /*
             * For each layered table, we want to see what is the oldest checkpoint on that table
             * that is in use by any open cursor. Even if there are no open cursors on it, the most
             * recent checkpoint on the table is always considered in use. The basic plan is to
             * start with the last checkpoint in use that we knew about, and check it again. If it's
             * no longer in use, we go to the next one, etc. This gives us a list (possibly zero
             * length), of checkpoints that are no longer in use by cursors on this table. Thus, the
             * timestamp associated with the newest such checkpoint can be used for garbage
             * collection pruning. Any item in the ingest table older than that timestamp must be
             * including in one of the checkpoints we're saving, and thus can be removed.
             */
            ckpt_inuse = layered_table->last_ckpt_inuse;
            if (ckpt_inuse == 0) {
                /*
                 * If we've never checked this layered table before, it's safe to start at the
                 * oldest checkpoint that we're tracking. There are no cursors in the system that
                 * are open at checkpoints older than that one. It's probably impossible that we
                 * haven't tracked any checkpoints, if that happens, we'll start checking at zero.
                 */
                if (ds->ckpt_track_cnt > 0)
                    ckpt_inuse = ds->ckpt_track[0].ckpt_order;
            }

            /*
             * Allocate enough room for the uri and the WiredTigerCheckpoint.NNN
             */
            len = strlen(layered_table->stable_uri) + strlen(WT_CHECKPOINT) + 20;
            WT_ERR(__wt_realloc_def(session, &uri_alloc, len, &uri_at_checkpoint));

            /*
             * For each checkpoint, see of the handle is in use. If not, it is safe to gc.
             * FIXME-WT-15192: `ckpt_inuse` and `last_ckpt` could be obtained from different tables
             * and that's not correct to compare checkpoint orders from different tables since they
             * are unrelated.
             */
            while (ckpt_inuse < last_ckpt) {
                WT_ERR(__wt_snprintf(uri_at_checkpoint, uri_alloc, "%s/%s.%" PRId64,
                  layered_table->stable_uri, WT_CHECKPOINT, ckpt_inuse));

                /* If it's in use, then it must be in the connection cache. */
                WT_WITH_HANDLE_LIST_READ_LOCK(
                  session, (ret = __wt_conn_dhandle_find(session, uri_at_checkpoint, NULL)));

                /* If it's in use by any session, then we're done. */
                if (ret == 0 && session->dhandle->session_inuse > 0)
                    break;

                WT_ERR_NOTFOUND_OK(ret, false);
                ++ckpt_inuse;
            }

            /*
             * We now have the oldest checkpoint in use for this table. If it's different from the
             * last time we checked, find out what timestamp that checkpoint corresponds to. That
             * will be the timestamp we use for pruning.
             */
            if (ckpt_inuse != layered_table->last_ckpt_inuse) {
                for (track = 0; track < ds->ckpt_track_cnt; ++track)
                    if (ds->ckpt_track[track].ckpt_order == ckpt_inuse)
                        break;
                if (track >= ds->ckpt_track_cnt)
                    WT_ERR_MSG(session, WT_NOTFOUND,
                      "could not find checkpoint order %" PRId64 " in list of tracked checkpoints",
                      ckpt_inuse);

                prune_timestamp = ds->ckpt_track[track].timestamp;

                /*
                 * Set the prune timestamp in the btree if it is open, typically it is. However,
                 * it's possible that it hasn't been opened yet. In that case, we need to skip
                 * updating its timestamp for pruning, and we'll get another chance to update the
                 * prune timestamp at the next checkpoint.
                 */
                WT_ERR_NOTFOUND_OK(
                  __wt_session_get_dhandle(session, layered_table->ingest_uri, NULL, NULL, 0),
                  true);
                if (ret != WT_NOTFOUND) {
                    btree = (WT_BTREE *)session->dhandle->handle;

                    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
                      "GC %s: update prune timestamp from %" PRIu64 " to %" PRIu64,
                      layered_table->iface.name, btree->prune_timestamp, prune_timestamp);
                    WT_ASSERT(session, prune_timestamp >= btree->prune_timestamp);
                    WT_RELEASE_WRITE(btree->prune_timestamp, prune_timestamp);

                    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
                      "GC %s: update checkpoint in use from %" PRId64 " to %" PRId64,
                      layered_table->iface.name, layered_table->last_ckpt_inuse, ckpt_inuse);
                    layered_table->last_ckpt_inuse = ckpt_inuse;
                    WT_ERR(__wt_session_release_dhandle(session));
                } else
                    ret = 0;
            }
            min_ckpt_inuse = WT_MIN(layered_table->last_ckpt_inuse, min_ckpt_inuse);
        }
    }
    ds->ckpt_min_inuse = min_ckpt_inuse;

err:
    /*
     * FIXME-WT-14735: we could hold lock for a shorter time. Maybe release it after getting/copying
     * each URI, then an individual URI could be garbage collected without a lock, then re-acquire
     * to get the next entry in the table.
     */
    __wt_spin_unlock(session, &manager->layered_table_lock);

    return (ret);
}

/*
 * __layered_last_checkpoint_order --
 *     For a URI, get the order number for the most recent checkpoint.
 */
static int
__layered_last_checkpoint_order(
  WT_SESSION_IMPL *session, const char *shared_uri, int64_t *ckpt_order)
{
    int scanf_ret;

    const char *checkpoint_name;
    int64_t order_from_name;

    *ckpt_order = 0;

    /* Pull up the last checkpoint for this URI. It could return WT_NOTFOUND. */
    WT_RET(__wt_meta_checkpoint_last_name(session, shared_uri, &checkpoint_name, ckpt_order, NULL));

    /* Sanity check: we make sure that the name returned matches the order number. */
    scanf_ret = sscanf(checkpoint_name, WT_CHECKPOINT ".%" PRId64, &order_from_name);
    __wt_free(session, checkpoint_name);

    if (scanf_ret != 1)
        WT_RET_MSG(session, EINVAL,
          "shared metadata checkpoint unknown format: %s, scan returns %d", checkpoint_name,
          scanf_ret);

    /* These should always be the same. */
    WT_ASSERT(session, *ckpt_order == order_from_name);

    return (0);
}

/*
 * __layered_track_checkpoint --
 *     Keep a record of past checkpoints, they will be needed for ingest garbage collection.
 */
static int
__layered_track_checkpoint(WT_SESSION_IMPL *session, uint64_t checkpoint_timestamp)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_DISAGGREGATED_STORAGE *ds;
    int64_t order;
    uint32_t entry, expire;

    conn = S2C(session);
    ds = &conn->disaggregated_storage;

    WT_RET_NOTFOUND_OK(
      ret = __layered_last_checkpoint_order(session, WT_DISAGG_METADATA_URI, &order));

    /* If we didn't find a checkpoint, it means that there are no data in the shared storage. */
    if (ret == WT_NOTFOUND)
        return (0);

    /* Figure out how many entries at the beginning are no longer useful. */
    for (expire = 0; expire < ds->ckpt_track_cnt; ++expire) {
        if (ds->ckpt_track[expire].ckpt_order >= ds->ckpt_min_inuse)
            break;
        __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
          "expiring tracked checkpoint: %" PRId64 " %" PRIu64 "\n",
          ds->ckpt_track[expire].ckpt_order, ds->ckpt_track[expire].timestamp);
    }

    /* Shift the array of tracked items down to remove any expired entries. */
    if (expire != 0) {
        memmove(&ds->ckpt_track[0], &ds->ckpt_track[expire],
          sizeof(ds->ckpt_track[0]) * (ds->ckpt_track_cnt - expire));
        ds->ckpt_track_cnt -= expire;
    }

    /* Allocate one more, and fill it. */
    entry = ds->ckpt_track_cnt;
    WT_RET(__wt_realloc_def(session, &ds->ckpt_track_alloc, entry + 1, &ds->ckpt_track));
    ds->ckpt_track[entry].ckpt_order = order;
    ds->ckpt_track[entry].timestamp = checkpoint_timestamp;
    ++ds->ckpt_track_cnt;
    __wt_verbose_level(session, WT_VERB_LAYERED, WT_VERBOSE_DEBUG_5,
      "tracking checkpoint: %" PRId64 " %" PRIu64 "\n", order, checkpoint_timestamp);

    return (0);
}

/*
 * __wt_disagg_update_shared_metadata --
 *     Update the shared metadata.
 */
int
__wt_disagg_update_shared_metadata(WT_SESSION_IMPL *session, const char *key, const char *value)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), "overwrite", NULL};

    WT_ASSERT(session, S2C(session)->layered_table_manager.leader);

    cursor = NULL;

    WT_ERR(__wt_open_cursor(session, WT_DISAGG_METADATA_URI, NULL, cfg, &cursor));
    cursor->set_key(cursor, key);
    cursor->set_value(cursor, value);
    WT_ERR(cursor->insert(cursor));

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    return (ret);
}

/*
 * __disagg_copy_shared_metadata --
 *     Copy shared metadata from the main metadata table to the shared metadata table.
 */
static int
__disagg_copy_shared_metadata(WT_SESSION_IMPL *session, WT_CURSOR *md_cursor, const char *key)
{
    WT_DECL_RET;
    const char *md_value;

    md_value = NULL;

    md_cursor->set_key(md_cursor, key);
    WT_RET(md_cursor->search(md_cursor));
    WT_RET(md_cursor->get_value(md_cursor, &md_value));
    WT_SAVE_DHANDLE(session, ret = __wt_disagg_update_shared_metadata(session, key, md_value));
    WT_RET(ret);

    return (0);
}

/*
 * __disagg_copy_shared_metadata_one --
 *     Copy the metadata associated with the given URI from the main metadata table to the shared
 *     metadata table.
 */
static int
__disagg_copy_shared_metadata_one(WT_SESSION_IMPL *session, const char *uri)
{
    WT_CURSOR *md_cursor;
    WT_DECL_RET;

    md_cursor = NULL;

    WT_ERR(__wt_metadata_cursor(session, &md_cursor));
    WT_ERR_NOTFOUND_OK(__disagg_copy_shared_metadata(session, md_cursor, uri), false);

err:
    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(session, &md_cursor));

    return (ret);
}

/*
 * __wt_disagg_copy_shared_metadata_layered --
 *     Copy all metadata relevant to the given base name (without prefix or suffix) from the main
 *     metadata table to the shared metadata table.
 */
int
__wt_disagg_copy_shared_metadata_layered(WT_SESSION_IMPL *session, const char *name)
{
    WT_CURSOR *md_cursor;
    WT_DECL_RET;
    size_t len;
    char *md_key;

    md_cursor = NULL;
    md_key = NULL;

    len = strlen(name) + 16;
    WT_ERR(__wt_calloc_def(session, len, &md_key));
    WT_ERR(__wt_metadata_cursor(session, &md_cursor));

    WT_ERR(__wt_snprintf(md_key, len, "colgroup:%s", name));
    WT_ERR_NOTFOUND_OK(__disagg_copy_shared_metadata(session, md_cursor, md_key), false);

    WT_ERR(__wt_snprintf(md_key, len, "layered:%s", name));
    WT_ERR_NOTFOUND_OK(__disagg_copy_shared_metadata(session, md_cursor, md_key), false);

    WT_ERR(__wt_snprintf(md_key, len, "table:%s", name));
    WT_ERR_NOTFOUND_OK(__disagg_copy_shared_metadata(session, md_cursor, md_key), false);

err:
    __wt_free(session, md_key);
    if (md_cursor != NULL)
        WT_TRET(__wt_metadata_cursor_release(session, &md_cursor));

    return (ret);
}
