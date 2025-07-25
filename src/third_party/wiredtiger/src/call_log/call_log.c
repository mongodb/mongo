/*-
 * Copyright (c) 2014-present MongoDB, Inc.
 * Copyright (c) 2008-2014 WiredTiger, Inc.
 *	All rights reserved.
 *
 * See the file LICENSE for redistribution information.
 */

#include "wt_internal.h"

#ifdef HAVE_CALL_LOG
/*
 * __wt_conn_call_log_setup --
 *     Setup the resources for call log tracking.
 */
int
__wt_conn_call_log_setup(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_ITEM(file_name);
    WT_DECL_RET;

    conn = S2C(session);

    /* Do not setup call log file if the connection is in readonly mode. */
    if (F_ISSET(conn, WT_CONN_READONLY)) {
        return (ret);
    }

    WT_RET(__wt_scr_alloc(session, 0, &file_name));
    WT_ERR(__wt_filename_construct(
      session, "", "wt_call_log", __wt_process_id(), UINT32_MAX, file_name));
    if ((ret = __wt_fopen(session, (const char *)file_name->data, WT_FS_OPEN_CREATE,
           WT_STREAM_APPEND, &conn->call_log_fst)) != 0)
        WT_ERR_MSG(session, ret, "Failed to open call log.");

    F_SET(conn, WT_CONN_CALL_LOG_ENABLED);

err:
    __wt_scr_free(session, &file_name);
    return (ret);
}

/*
 * __wt_conn_call_log_teardown --
 *     Clean up the resources used for the call log.
 */
int
__wt_conn_call_log_teardown(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    return (__wt_fclose(session, &conn->call_log_fst));
}

/*
 * __call_log_print_start --
 *     Helper function for printing the beginning section of the call log entry. Each entry will
 *     specify the class and method names corresponding to the timestamp simulator.
 */
static int
__call_log_print_start(WT_SESSION_IMPL *session, const char *class_name, const char *method_name)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    WT_RET(__wt_fprintf(session, conn->call_log_fst,
      "{\n"
      "    \"class_name\" : \"%s\",\n"
      "    \"method_name\" : \"%s\",\n",
      class_name, method_name));

    return (0);
}

/*
 * __call_log_print_input --
 *     Helper function for printing the input section of the call log entry. A variable number of
 *     arguments is accepted since each API call may use a different number of inputs. Inputs are
 *     expected to be a string in JSON format to be appended to the input list in the JSON API call
 *     entry.
 */
static int
__call_log_print_input(WT_SESSION_IMPL *session, int n, ...)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    va_list valist;

    conn = S2C(session);

    va_start(valist, n);

    WT_ERR(__wt_fprintf(session, conn->call_log_fst, "    \"input\" : {\n"));

    for (int i = 0; i < n; i++) {
        /* Don't print the comma at the end of the input entry if it's the last one. */
        if (i == n - 1)
            WT_ERR(
              __wt_fprintf(session, conn->call_log_fst, "        %s\n", va_arg(valist, char *)));
        else
            WT_ERR(
              __wt_fprintf(session, conn->call_log_fst, "        %s,\n", va_arg(valist, char *)));
    }

    WT_ERR(__wt_fprintf(session, conn->call_log_fst, "    },\n"));

err:
    va_end(valist);

    return (ret);
}

/*
 * __call_log_print_output --
 *     Helper function for printing the output section of the call log entry. A variable number of
 *     arguments is accepted since each API call may use a different number of outputs. Outputs are
 *     expected to be a string in JSON format to be appended to the output list in the JSON API call
 *     entry.
 */
static int
__call_log_print_output(WT_SESSION_IMPL *session, int n, ...)
{
    WT_CONNECTION_IMPL *conn;
    WT_DECL_RET;
    va_list valist;

    conn = S2C(session);

    va_start(valist, n);

    WT_ERR(__wt_fprintf(session, conn->call_log_fst, "    \"output\" : {\n"));

    for (int i = 0; i < n; i++) {
        /* Don't print the comma at the end of the output entry if it's the last one. */
        if (i == n - 1)
            WT_ERR(
              __wt_fprintf(session, conn->call_log_fst, "        %s\n", va_arg(valist, char *)));
        else
            WT_ERR(
              __wt_fprintf(session, conn->call_log_fst, "        %s,\n", va_arg(valist, char *)));
    }

    WT_ERR(__wt_fprintf(session, conn->call_log_fst, "    },\n"));

err:
    va_end(valist);

    return (ret);
}

/*
 * __wt_call_log_print_return --
 *     Helper function for printing the return section of the call log entry. Each return section of
 *     the call log entry expects to have the return value of the API call and an error message if
 *     it exists.
 */
int
__wt_call_log_print_return(
  WT_CONNECTION_IMPL *conn, WT_SESSION_IMPL *session, int ret_val, const char *err_msg)
{
    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    return (__wt_fprintf(session, conn->call_log_fst,
      "    \"return\" : {\n"
      "        \"return_val\" : %d,\n"
      "        \"error_message\" : \"%s\"\n"
      "    }\n"
      "},\n",
      ret_val, err_msg));
}

/*
 * __wt_call_log_open_session --
 *     Print the call log entry for the open session API call.
 */
int
__wt_call_log_open_session(WT_SESSION_IMPL *session, int ret_val)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "connection", "open_session"));

    /*
     * Open session includes the session address as an id in the call log entry. This ID is used to
     * map the session used by wiredtiger to a new session in the simulator.
     */
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /* Open session has no input or output arguments. */
    WT_RET(__call_log_print_input(session, 0));
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_set_timestamp --
 *     Print the call log entry for the set timestamp API call.
 */
int
__wt_call_log_set_timestamp(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "connection", "set_timestamp"));

    /* Connection ID to be used by the call log manager. */
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"connection_id\": \"%p\",\n", conn));

    /*
     * The set timestamp entry includes the timestamp configuration string which is copied from the
     * original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /* Set timestamp has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_query_timestamp --
 *     Print the call log entry for the query timestamp API call.
 */
int
__wt_call_log_query_timestamp(
  WT_SESSION_IMPL *session, const char *config, const char *hex_timestamp, int ret_val, bool global)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128], hex_ts_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    /*
     * The global transaction timestamps are queried on the connection object while the session
     * transaction timestamps are queried on the session object.
     */
    if (global) {
        WT_RET(__call_log_print_start(session, "connection", "query_timestamp"));
        WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"connection_id\": \"%p\",\n", conn));
    } else {
        WT_RET(__call_log_print_start(session, "session", "query_timestamp"));
        WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));
    }

    /*
     * The query timestamp entry includes the timestamp configuration string which is copied from
     * the original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /*
     * The timestamp queried is the output from the original query timestamp API call. Don't output
     * the timestamp to the call log if the query has failed as it will be a garbage value.
     */
    if (ret_val == 0) {
        WT_RET(__wt_snprintf(
          hex_ts_buf, sizeof(hex_ts_buf), "\"timestamp_queried\": \"%s\"", hex_timestamp));
        WT_RET(__call_log_print_output(session, 1, hex_ts_buf));
    }

    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_begin_transaction --
 *     Print the call log entry for the begin transaction API call.
 */
int
__wt_call_log_begin_transaction(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "begin_transaction"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The begin transaction entry includes the timestamp configuration string which is copied from
     * the original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /* Begin transaction has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_prepare_transaction --
 *     Print the call log entry for the prepare transaction API call.
 */
int
__wt_call_log_prepare_transaction(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "prepare_transaction"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The prepare transaction entry includes the timestamp configuration string which is copied
     * from the original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /* Prepare transaction has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_commit_transaction --
 *     Print the call log entry for the commit transaction API call.
 */
int
__wt_call_log_commit_transaction(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "commit_transaction"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The commit transaction entry includes the timestamp configuration string which is copied from
     * the original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /* Commit transaction has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_rollback_transaction --
 *     Print the call log entry for the rollback transaction API call.
 */
int
__wt_call_log_rollback_transaction(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "rollback_transaction"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The rollback_transaction entry includes the configuration string which is copied from the
     * original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_timestamp_transaction --
 *     Print the call log entry for the timestamp_transaction API call.
 */
int
__wt_call_log_timestamp_transaction(WT_SESSION_IMPL *session, const char *config, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char config_buf[128];

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "timestamp_transaction"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The timestamp transaction entry includes the timestamp configuration string which is copied
     * from the original API call.
     */
    WT_RET(__wt_snprintf(config_buf, sizeof(config_buf), "\"config\": \"%s\"", config));
    WT_RET(__call_log_print_input(session, 1, config_buf));

    /* timestamp_transaction has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_timestamp_transaction_uint --
 *     Print the call log entry for the timestamp_transaction_uint API call.
 */
int
__wt_call_log_timestamp_transaction_uint(
  WT_SESSION_IMPL *session, WT_TS_TXN_TYPE which, uint64_t ts, int ret_val)
{
    WT_CONNECTION_IMPL *conn;
    char ts_buf[128];
    char ts_type_buf[128];
    const char *name;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    name = "unknown";
    switch (which) {
    case WT_TS_TXN_TYPE_COMMIT:
        name = "commit";
        break;
    case WT_TS_TXN_TYPE_DURABLE:
        name = "durable";
        break;
    case WT_TS_TXN_TYPE_PREPARE:
        name = "prepare";
        break;
    case WT_TS_TXN_TYPE_READ:
        name = "read";
        break;
    }

    WT_RET(__call_log_print_start(session, "session", "timestamp_transaction_uint"));
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /*
     * The timestamp transaction uint entry includes the timestamp being set, indicated by
     * WT_TS_TXN_TYPE and the timestamp itself as input.
     */
    WT_RET(__wt_snprintf(ts_type_buf, sizeof(ts_type_buf), "\"wt_ts_txp_type\": \"%s\"", name));
    WT_RET(__wt_snprintf(ts_buf, sizeof(ts_buf), "\"timestamp\": %" PRIu64, ts));
    WT_RET(__call_log_print_input(session, 2, ts_type_buf, ts_buf));

    /* timestamp_transaction_uint has no output arguments. */
    WT_RET(__call_log_print_output(session, 0));
    WT_RET(__wt_call_log_print_return(conn, session, ret_val, ""));

    return (0);
}

/*
 * __wt_call_log_close_session --
 *     Print the call log entry for the close session API call.
 */
int
__wt_call_log_close_session(WT_SESSION_IMPL *session)
{
    WT_CONNECTION_IMPL *conn;

    conn = S2C(session);

    if (!F_ISSET(conn, WT_CONN_CALL_LOG_ENABLED))
        return (0);

    WT_RET(__call_log_print_start(session, "session", "close_session"));

    /*
     * Close session includes the session address as an id in the call log entry. This ID is used to
     * map the session used by wiredtiger to a new session in the simulator.
     */
    WT_RET(__wt_fprintf(session, conn->call_log_fst, "    \"session_id\": \"%p\",\n", session));

    /* Close session has no input or output arguments. */
    WT_RET(__call_log_print_input(session, 0));
    WT_RET(__call_log_print_output(session, 0));

    return (0);
}

#endif
