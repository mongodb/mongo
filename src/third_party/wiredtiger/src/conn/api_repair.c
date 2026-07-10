/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

typedef struct __wt_repair_config WT_REPAIR_CONFIG;

struct __wt_repair_config {

#define WT_REPAIR_COMMAND_NONE 0
#define WT_REPAIR_COMMAND_FETCH_DATABASE_SIZE 1
#define WT_REPAIR_COMMAND_FETCH_METADATA 2
    int command;

    struct {
        /* local=false recomputes the size from the metadata instead of reading the running total.
         */
        bool local;
    } fetch_database_size;

    struct {
        /* Fetch metadata from the local cursor, not the shared page-server checkpoint. */
        bool local;
        /* Target URI for the command, or NULL for all URIs. */
        const char *uri;
        /* Single metadata key to report, or NULL for the whole value. */
        const char *key;
    } fetch_metadata;
};

static int __repair_config_decode(WT_SESSION_IMPL *, WT_ITEM *, const char *, WT_REPAIR_CONFIG *);
static int __repair_config_set_command(
  WT_SESSION_IMPL *, WT_ITEM *, WT_CONFIG_ITEM *, WT_REPAIR_CONFIG *, int);

static int __repair_fetch_database_size(WT_SESSION_IMPL *, WT_ITEM *, bool);
static int __repair_fetch_metadata(WT_SESSION_IMPL *, WT_ITEM *, const char *, const char *, bool);

/*
 * WT_ERR_REPORT --
 *     Like WT_ERR_MSG, but append the diagnostic to the caller-owned report buffer (which
 *     wiredtiger_repair hands back) instead of logging it. The return of buffer write is ignored so
 *     it cannot clobber the requested error v -- v is what must propagate.
 */
#define WT_ERR_REPORT(session, v, ...)                                \
    do {                                                              \
        ret = (v);                                                    \
        WT_IGNORE_RET(__wt_buf_catfmt(session, report, __VA_ARGS__)); \
        goto err;                                                     \
    } while (0)

/*
 * __repair_fetch_database_size --
 *     Read-only database size inspection: local=true returns the maintained total; local=false
 *     recomputes it from the metadata.
 */
static int
__repair_fetch_database_size(WT_SESSION_IMPL *session, WT_ITEM *report, bool is_local)
{
    uint64_t database_size;

    if (is_local)
        WT_RET(__wt_buf_catfmt(session, report, "fetch_database_size(local): %" PRIu64,
          S2C(session)->disaggregated_storage.database_size));
    else {
        WT_RET(__wt_disagg_get_database_size(session, &database_size));
        WT_RET(__wt_buf_catfmt(session, report, "fetch_database_size(recompute): %" PRIu64,
          database_size + WT_DISAGG_CHECKPOINT_SIZE_BUFFER));
    }
    return (0);
}

/*
 * __repair_fetch_metadata --
 *     Read-only metadata inspection: return metadata without modifying anything.
 */
static int
__repair_fetch_metadata(
  WT_SESSION_IMPL *session, WT_ITEM *report, const char *uri, const char *key, bool is_local)
{
    WT_CONFIG_ITEM item;
    WT_CURSOR *cursor;
    WT_DECL_ITEM(ckpt_uri);
    WT_DECL_RET;
    const char *ckpt_name, *k, *v;
    bool found;

    cursor = NULL;
    ckpt_name = NULL;
    found = false;

    if (is_local)
        WT_ERR(__wt_metadata_cursor(session, &cursor));
    else {
        const char *cfg[] = {WT_CONFIG_BASE(session, WT_SESSION_open_cursor), NULL};

        /*
         * The require_disagg check in __repair_config_set_command already confirmed this connection
         * has picked up a checkpoint, which guarantees the shared metadata table has a local
         * checkpoint (the page-server-durable state) to open here.
         */
        WT_ERR(
          __wt_meta_checkpoint_last_name(session, WT_DISAGG_METADATA_URI, &ckpt_name, NULL, NULL));

        WT_ERR(__wt_scr_alloc(session, 0, &ckpt_uri));
        WT_ERR(__wt_buf_fmt(session, ckpt_uri, "%s/%s", WT_DISAGG_METADATA_URI, ckpt_name));
        WT_ERR(__wt_open_cursor(session, ckpt_uri->data, NULL, cfg, &cursor));
    }

    /* Walk the metadata and report the entries matching the target URI. */
    while ((ret = cursor->next(cursor)) == 0) {
        WT_ERR(cursor->get_key(cursor, &k));
        if (uri != NULL && strcmp(k, uri) != 0)
            continue;

        found = true;
        WT_ERR(cursor->get_value(cursor, &v));
        if (key != NULL) {
            WT_ERR_NOTFOUND_OK(__wt_config_getones(session, v, key, &item), true);
            if (WT_CHECK_AND_RESET(ret, WT_NOTFOUND)) {
                WT_ERR(__wt_buf_catfmt(session, report, "\n  %s: <no \"%s\">", k, key));
                continue;
            }
            WT_ERR(
              __wt_buf_catfmt(session, report, "\n  %s: %s=%.*s", k, key, (int)item.len, item.str));
        } else
            WT_ERR(__wt_buf_catfmt(session, report, "\n  %s: %s", k, v));
    }
    WT_ERR_NOTFOUND_OK(ret, false);

    if (!found)
        WT_ERR(__wt_buf_catfmt(session, report, " <no matching metadata entry for uri:\"%s\">",
          uri == NULL ? "<all>" : uri));

err:
    if (cursor != NULL) {
        if (is_local)
            WT_TRET(__wt_metadata_cursor_release(session, &cursor));
        else
            WT_TRET(cursor->close(cursor));
    }
    __wt_free(session, ckpt_name);
    __wt_scr_free(session, &ckpt_uri);
    return (ret);
}

/*
 * __repair_config_set_command --
 *     Set the command in the repair_config based on the parsed config item.
 */
static int
__repair_config_set_command(WT_SESSION_IMPL *session, WT_ITEM *report, WT_CONFIG_ITEM *config_item,
  WT_REPAIR_CONFIG *repair_config, int command)
{
    WT_CONFIG_ITEM item;
    WT_DECL_RET;
    bool require_disagg;

    require_disagg = false;

    if (repair_config->command != WT_REPAIR_COMMAND_NONE)
        WT_ERR_REPORT(session, EINVAL, "Only one command is allowed in the config");

    repair_config->command = command;

    if (repair_config->command == WT_REPAIR_COMMAND_FETCH_DATABASE_SIZE) {
        WT_ERR_NOTFOUND_OK(__wt_config_subgets(session, config_item, "local", &item), true);
        if (WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
            repair_config->fetch_database_size.local = true;
        else
            repair_config->fetch_database_size.local = item.val != 0;

        /*
         * Both variants read or derive conn->disaggregated_storage.database_size, a concept that
         * only exists on a disaggregated connection.
         */
        require_disagg = true;
    } else if (repair_config->command == WT_REPAIR_COMMAND_FETCH_METADATA) {
        WT_ERR_NOTFOUND_OK(__wt_config_subgets(session, config_item, "local", &item), true);
        if (WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
            repair_config->fetch_metadata.local = true;
        else
            repair_config->fetch_metadata.local = item.val != 0;

        /* An empty uri/key is treated as absent (NULL): all URIs / the whole value. */
        WT_ERR_NOTFOUND_OK(__wt_config_subgets(session, config_item, "uri", &item), true);
        if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND) && item.len != 0)
            WT_ERR(__wt_strndup(session, item.str, item.len, &repair_config->fetch_metadata.uri));

        WT_ERR_NOTFOUND_OK(__wt_config_subgets(session, config_item, "key", &item), true);
        if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND) && item.len != 0)
            WT_ERR(__wt_strndup(session, item.str, item.len, &repair_config->fetch_metadata.key));

        require_disagg = !repair_config->fetch_metadata.local;
    }

    if (require_disagg && !__wt_disagg_has_picked_up_checkpoint(session))
        WT_ERR_REPORT(session, EINVAL,
          "This command requires a disaggregated connection with a valid checkpoint");

err:
    return (ret);
}

/*
 * __repair_config_decode --
 *     The config is parsed with the normal WT config parser:
 *
 * fetch_database_size=(local=<bool>) Read-only inspection: return the database size (disagg-only).
 *     local=true (default) reads conn->disaggregated_storage.database_size, the maintained running
 *     total. local=false recomputes the same total from scratch by walking the metadata (the same
 *     computation session->checkpoint(debug=(database_size_fix=true)) uses to correct drift).
 *
 * fetch_metadata=(local=<bool>,uri="<uri>",key="<key>") Read-only inspection: return metadata
 *     values. local=true (default) reads the local metadata cursor; local=false (disagg-only) reads
 *     the shared, page-server-durable metadata checkpoint. uri="" selects one target; absent or
 *     empty means all URIs. key="" selects one first-level config value out of the matching
 *     entries; absent or empty means the whole value.
 */
static int
__repair_config_decode(
  WT_SESSION_IMPL *session, WT_ITEM *report, const char *config, WT_REPAIR_CONFIG *repair_config)
{
    WT_CONFIG_ITEM item;
    WT_DECL_RET;

    WT_CLEAR(item);

    repair_config->command = WT_REPAIR_COMMAND_NONE;

    /* Check for commands */
    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, config, "fetch_database_size", &item), true);
    if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
        WT_ERR(__repair_config_set_command(
          session, report, &item, repair_config, WT_REPAIR_COMMAND_FETCH_DATABASE_SIZE));

    WT_ERR_NOTFOUND_OK(__wt_config_getones(session, config, "fetch_metadata", &item), true);
    if (!WT_CHECK_AND_RESET(ret, WT_NOTFOUND))
        WT_ERR(__repair_config_set_command(
          session, report, &item, repair_config, WT_REPAIR_COMMAND_FETCH_METADATA));

    if (repair_config->command == WT_REPAIR_COMMAND_NONE)
        WT_ERR_REPORT(session, EINVAL, "No command found in the config");

err:
    return (ret);
}

/*
 * wiredtiger_repair --
 *     WiredTiger repair in runtime. Each config can only carry one active sub-command.
 */
const char *
wiredtiger_repair(WT_CONNECTION *connection, const char *config)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(report);
    WT_DECL_RET;
    WT_REPAIR_CONFIG repair_config;
    WT_SESSION_IMPL *default_session, *session;

    WT_CLEAR(repair_config);
    default_session = NULL;
    session = NULL;

    if (connection == NULL)
        return ("wiredtiger_repair: NULL connection");

    conn = (WT_CONNECTION_IMPL *)connection;
    default_session = conn->default_session;

    if (!__wt_atomic_cas_uint8(
          &conn->repair.state, WT_REPAIR_STATE_IDLE, WT_REPAIR_STATE_OPERATING))
        return ("wiredtiger_repair: another repair operation is in progress");

    /*
     * The report buffer is owned by the connection so the returned string stays valid after this
     * call returns, until the next call reuses it. Reset it and build the new report in place.
     */
    report = &conn->repair.last_report;
    report->size = 0;

    if (config == NULL || strlen(config) == 0)
        WT_ERR_REPORT(default_session, EINVAL, "wiredtiger_repair: empty config");

    /* Open a public session for the parsing and the work; the default session owns the report. */
    WT_ERR(connection->open_session(connection, NULL, NULL, (WT_SESSION **)&session));

    WT_ERR(__repair_config_decode(session, report, config, &repair_config));

    switch (repair_config.command) {
    case WT_REPAIR_COMMAND_FETCH_DATABASE_SIZE:
        WT_ERR(
          __repair_fetch_database_size(session, report, repair_config.fetch_database_size.local));
        break;
    case WT_REPAIR_COMMAND_FETCH_METADATA:
        WT_ERR(__repair_fetch_metadata(session, report, repair_config.fetch_metadata.uri,
          repair_config.fetch_metadata.key, repair_config.fetch_metadata.local));
        break;
    default:
        WT_ERR(__wt_illegal_value(session, repair_config.command));
    }

err:
    if (ret != 0)
        WT_IGNORE_RET(
          __wt_buf_catfmt(default_session, report, " Failed: %s", wiredtiger_strerror(ret)));

    __wt_free(default_session, repair_config.fetch_metadata.uri);
    __wt_free(default_session, repair_config.fetch_metadata.key);

    if (session != NULL)
        WT_IGNORE_RET(((WT_SESSION *)session)->close((WT_SESSION *)session, NULL));

    WT_IGNORE_RET(
      __wt_atomic_cas_uint8(&conn->repair.state, WT_REPAIR_STATE_OPERATING, WT_REPAIR_STATE_IDLE));

    return (report->size > 0 ? report->data : "");
}
