/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

/*
 * __chunkcache_create_metadata_file --
 *     Create the table that will persistently track what chunk cache content is on disk.
 */
static int
__chunkcache_create_metadata_file(
  WT_SESSION_IMPL *session, uint64_t capacity, unsigned int hashtable_size, size_t chunk_size)
{
    char cfg[128];
    WT_RET(__wt_snprintf(cfg, sizeof(cfg), WT_CC_APP_META_FORMAT "," WT_CC_META_CONFIG, capacity,
      hashtable_size, chunk_size));

    return (__wt_session_create(session, WT_CC_METAFILE_URI, cfg));
}

/*
 * __chunkcache_get_metadata_config --
 *     If present, retrieve the on-disk configuration for the chunk cache metadata file. The caller
 *     must only use *config if this returns zero. The caller is responsible for freeing the memory
 *     allocated into *config.
 */
static int
__chunkcache_get_metadata_config(WT_SESSION_IMPL *session, char **config)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    char *tmp;

    *config = NULL;

    WT_RET(__wt_metadata_cursor(session, &cursor));
    cursor->set_key(cursor, WT_CC_METAFILE_URI);
    WT_ERR(cursor->search(cursor));

    WT_ERR(cursor->get_value(cursor, &tmp));
    WT_ERR(__wt_strdup(session, tmp, config));

err:
    WT_TRET(__wt_metadata_cursor_release(session, &cursor));
    return (ret);
}

/*
 * __chunkcache_verify_metadata_config --
 *     Check that the existing chunk cache configuration is compatible with our current
 *     configuration (and ergo, whether we can reuse the chunk cache contents).
 */
static int
__chunkcache_verify_metadata_config(WT_SESSION_IMPL *session, char *md_config, uint64_t capacity,
  unsigned int hashtable_size, size_t chunk_size)
{
    char tmp[128];

    WT_RET(
      __wt_snprintf(tmp, sizeof(tmp), WT_CC_APP_META_FORMAT, capacity, hashtable_size, chunk_size));

    if (strstr(md_config, tmp) == NULL) {
        __wt_verbose_error(session, WT_VERB_CHUNKCACHE,
          "stored chunk cache config (%s) incompatible with runtime config (%s)", md_config, tmp);
        return (-1);
    }

    return (0);
}

/*
 * __chunkcache_apply_metadata_content --
 *     Extract key/value pairs from a metadata file to allocate chunks in the chunk cache.
 */
static int
__chunkcache_apply_metadata_content(WT_SESSION_IMPL *session)
{
    WT_CURSOR *cursor;
    WT_DECL_RET;
    wt_off_t file_offset;
    size_t data_sz;
    uint64_t cache_offset;
    uint32_t id;
    const char *name;

    WT_ERR(__wt_open_cursor(session, WT_CC_METAFILE_URI, NULL, NULL, &cursor));

    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &name, &id, &file_offset));
        WT_ERR(cursor->get_value(cursor, &cache_offset, &data_sz));
        WT_ERR(__wt_chunkcache_create_from_metadata(
          session, name, id, file_offset, cache_offset, data_sz));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));

    return (ret);
}

/*
 * __chunkcache_metadata_run_chk --
 *     Check to decide if the chunk cache metadata server should continue running.
 */
static bool
__chunkcache_metadata_run_chk(WT_SESSION_IMPL *session)
{
    return (FLD_ISSET(S2C(session)->server_flags, WT_CONN_SERVER_CHUNKCACHE_METADATA));
}

/*
 * __chunkcache_metadata_insert --
 *     Insert the specified work queue entry into the chunk cache metadata file.
 */
static int
__chunkcache_metadata_insert(WT_CURSOR *cursor, WT_CHUNKCACHE_METADATA_WORK_UNIT *entry)
{
    cursor->set_key(cursor, entry->name, entry->id, entry->file_offset);
    cursor->set_value(cursor, entry->cache_offset, entry->data_sz);

    return (cursor->insert(cursor));
}

/*
 * __chunkcache_metadata_delete --
 *     Remove the specified entry from the chunk cache metadata file.
 */
static int
__chunkcache_metadata_delete(WT_CURSOR *cursor, WT_CHUNKCACHE_METADATA_WORK_UNIT *entry)
{
    cursor->set_key(cursor, entry->name, entry->id, entry->file_offset);

    return (cursor->remove(cursor));
}

/*
 * __chunkcache_metadata_pop_work --
 *     Pop a work unit from the queue. The caller is responsible for freeing the returned work unit
 *     structure.
 */
static void
__chunkcache_metadata_pop_work(WT_SESSION_IMPL *session, WT_CHUNKCACHE_METADATA_WORK_UNIT **entryp)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    __wt_spin_lock(session, &conn->chunkcache_metadata_lock);
    if ((*entryp = TAILQ_FIRST(&conn->chunkcache_metadataqh)) != NULL) {
        TAILQ_REMOVE(&conn->chunkcache_metadataqh, *entryp, q);
        --conn->chunkcache_queue_len;
        WT_STAT_CONN_INCR(session, chunkcache_metadata_work_units_dequeued);
    }
    __wt_spin_unlock(session, &conn->chunkcache_metadata_lock);
}

/*
 * __chunkcache_metadata_work --
 *     Pop chunk cache work items off the queue, and write out the metadata.
 */
static int
__chunkcache_metadata_work(WT_SESSION_IMPL *session)
{
    WT_CHUNKCACHE_METADATA_WORK_UNIT *entry;
    WT_CURSOR *cursor;
    WT_DECL_RET;

    entry = NULL;

    WT_ERR(__wt_open_cursor(session, WT_CC_METAFILE_URI, NULL, NULL, &cursor));

    for (int i = 0; i < WT_CHUNKCACHE_METADATA_MAX_WORK; i++) {
        if (!__chunkcache_metadata_run_chk(session))
            break;

        __chunkcache_metadata_pop_work(session, &entry);
        if (entry == NULL)
            break;

        if (entry->type == WT_CHUNKCACHE_METADATA_WORK_INS) {
            WT_ERR(__chunkcache_metadata_insert(cursor, entry));
            WT_STAT_CONN_INCR(session, chunkcache_metadata_inserted);
        } else if (entry->type == WT_CHUNKCACHE_METADATA_WORK_DEL) {
            WT_ERR_NOTFOUND_OK(__chunkcache_metadata_delete(cursor, entry), false);
            WT_STAT_CONN_INCR(session, chunkcache_metadata_removed);
        } else {
            __wt_verbose_error(
              session, WT_VERB_CHUNKCACHE, "got invalid event type %d\n", entry->type);
            ret = -1;
            goto err;
        }

        __wt_free(session, entry);
        entry = NULL;
    }

err:
    if (cursor != NULL)
        WT_TRET(cursor->close(cursor));
    if (entry != NULL)
        __wt_free(session, entry);
    return (ret);
}

/*
 * __chunkcache_metadata_server --
 *     Dispatch chunks of work (or stop the server) when signalled.
 */
static WT_THREAD_RET
__chunkcache_metadata_server(void *arg)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    WT_SESSION_IMPL *session;
    uint64_t cond_time_us;
    bool signalled;

    session = arg;
    conn = S2C(session);
    cond_time_us = WT_MILLION;

    for (;;) {
        /* Wait until the next event. */
        __wt_cond_wait_signal(session, conn->chunkcache_metadata_cond, cond_time_us,
          __chunkcache_metadata_run_chk, &signalled);

        if (!__chunkcache_metadata_run_chk(session))
            break;

        if (signalled)
            WT_ERR(__chunkcache_metadata_work(session));
    }

    if (0) {
err:
        WT_IGNORE_RET(__wt_panic(session, ret, "%s", "chunk cache metadata server error"));
    }

    return (WT_THREAD_RET_VALUE);
}

/*
 * __wti_chunkcache_metadata_create --
 *     Start the server component of the chunk cache metadata subsystem.
 */
int
__wti_chunkcache_metadata_create(WT_SESSION_IMPL *session)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    char *metadata_config;

    conn = S2C(session);
    chunkcache = &conn->chunkcache;

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED) || chunkcache->type != WT_CHUNKCACHE_FILE)
        return (0);

    /* Retrieve the chunk cache metadata config, and ensure it matches our startup config. */
    ret = __chunkcache_get_metadata_config(session, &metadata_config);
    if (ret == WT_NOTFOUND) {
        WT_ERR(__chunkcache_create_metadata_file(
          session, chunkcache->capacity, chunkcache->hashtable_size, chunkcache->chunk_size));
        __wt_verbose(session, WT_VERB_CHUNKCACHE, "%s", "created chunk cache metadata file");
        ret = 0;
    } else if (ret == 0) {
        WT_ERR(__chunkcache_verify_metadata_config(session, metadata_config, chunkcache->capacity,
          chunkcache->hashtable_size, chunkcache->chunk_size));
        __wt_verbose(session, WT_VERB_CHUNKCACHE, "%s", "reused chunk cache metadata file");
    }
    WT_ERR(ret);

    FLD_SET(conn->server_flags, WT_CONN_SERVER_CHUNKCACHE_METADATA);

    WT_ERR(__wt_open_internal_session(
      conn, "chunkcache-metadata-server", true, 0, 0, &conn->chunkcache_metadata_session));
    session = conn->chunkcache_metadata_session;

    WT_ERR(__wt_cond_alloc(session, "chunk cache metadata", &conn->chunkcache_metadata_cond));

    WT_ERR(__chunkcache_apply_metadata_content(session));

    /* Start the thread. */
    WT_ERR(__wt_thread_create(
      session, &conn->chunkcache_metadata_tid, __chunkcache_metadata_server, session));
    conn->chunkcache_metadata_tid_set = true;

    if (0) {
err:
        FLD_CLR(conn->server_flags, WT_CONN_SERVER_CHUNKCACHE_METADATA);
        WT_TRET(__wti_chunkcache_metadata_destroy(session));
    }
    __wt_free(session, metadata_config);

    return (ret);
}

/*
 * __wti_chunkcache_metadata_destroy --
 *     Destroy the chunk cache metadata server thread.
 */
int
__wti_chunkcache_metadata_destroy(WT_SESSION_IMPL *session)
{
    WT_CHUNKCACHE *chunkcache;
    WT_CHUNKCACHE_METADATA_WORK_UNIT *entry;
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;

    conn = S2C(session);
    chunkcache = &conn->chunkcache;

    if (!F_ISSET(chunkcache, WT_CHUNKCACHE_CONFIGURED) || chunkcache->type != WT_CHUNKCACHE_FILE)
        return (0);

    FLD_CLR(conn->server_flags, WT_CONN_SERVER_CHUNKCACHE_METADATA);
    if (conn->chunkcache_metadata_tid_set) {
        WT_ASSERT(session, conn->chunkcache_metadata_cond != NULL);
        WT_TRET(__wt_thread_join(session, &conn->chunkcache_metadata_tid));
        conn->chunkcache_metadata_tid_set = false;
        while ((entry = TAILQ_FIRST(&conn->chunkcache_metadataqh)) != NULL) {
            TAILQ_REMOVE(&conn->chunkcache_metadataqh, entry, q);
            __wt_free(session, entry);
            --conn->chunkcache_queue_len;
        }
        WT_ASSERT(session, conn->chunkcache_queue_len == 0);
    }

    if (conn->chunkcache_metadata_session != NULL) {
        WT_TRET(__wt_session_close_internal(conn->chunkcache_metadata_session));
        conn->chunkcache_metadata_session = NULL;
    }

    __wt_cond_destroy(session, &conn->chunkcache_metadata_cond);

    return (ret);
}
