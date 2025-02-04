/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/proxy_impl.h>

#include <aws/common/encoding.h>
#include <aws/common/environment.h>
#include <aws/common/hash_table.h>
#include <aws/common/string.h>
#include <aws/http/connection_manager.h>
#include <aws/http/private/connection_impl.h>
#include <aws/http/proxy.h>
#include <aws/http/request_response.h>
#include <aws/io/channel.h>
#include <aws/io/logging.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4204) /* non-constant aggregate initializer */
#    pragma warning(disable : 4232) /* function pointer to dll symbol */
#endif

AWS_STATIC_STRING_FROM_LITERAL(s_host_header_name, "Host");
AWS_STATIC_STRING_FROM_LITERAL(s_proxy_connection_header_name, "Proxy-Connection");
AWS_STATIC_STRING_FROM_LITERAL(s_proxy_connection_header_value, "Keep-Alive");
AWS_STATIC_STRING_FROM_LITERAL(s_options_method, "OPTIONS");
AWS_STATIC_STRING_FROM_LITERAL(s_star_path, "*");

AWS_STATIC_STRING_FROM_LITERAL(s_http_proxy_env_var, "HTTP_PROXY");
AWS_STATIC_STRING_FROM_LITERAL(s_http_proxy_env_var_low, "http_proxy");
AWS_STATIC_STRING_FROM_LITERAL(s_https_proxy_env_var, "HTTPS_PROXY");
AWS_STATIC_STRING_FROM_LITERAL(s_https_proxy_env_var_low, "https_proxy");

#ifndef BYO_CRYPTO
AWS_STATIC_STRING_FROM_LITERAL(s_proxy_no_verify_peer_env_var, "AWS_PROXY_NO_VERIFY_PEER");
#endif

static struct aws_http_proxy_system_vtable s_default_vtable = {
    .aws_channel_setup_client_tls = &aws_channel_setup_client_tls,
};

static struct aws_http_proxy_system_vtable *s_vtable = &s_default_vtable;

void aws_http_proxy_system_set_vtable(struct aws_http_proxy_system_vtable *vtable) {
    s_vtable = vtable;
}

void aws_http_proxy_user_data_destroy(struct aws_http_proxy_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }
    aws_hash_table_clean_up(&user_data->alpn_string_map);

    /*
     * For tunneling connections, this is now internal and never surfaced to the user, so it's our responsibility
     * to clean up the last reference.
     */
    if (user_data->proxy_connection != NULL && user_data->proxy_config->connection_type == AWS_HPCT_HTTP_TUNNEL) {
        aws_http_connection_release(user_data->proxy_connection);
        user_data->proxy_connection = NULL;
    }

    aws_string_destroy(user_data->original_host);
    if (user_data->proxy_config) {
        aws_http_proxy_config_destroy(user_data->proxy_config);
    }

    if (user_data->original_tls_options) {
        aws_tls_connection_options_clean_up(user_data->original_tls_options);
        aws_mem_release(user_data->allocator, user_data->original_tls_options);
    }

    aws_http_proxy_negotiator_release(user_data->proxy_negotiator);

    aws_client_bootstrap_release(user_data->original_bootstrap);

    aws_mem_release(user_data->allocator, user_data);
}

struct aws_http_proxy_user_data *aws_http_proxy_user_data_new(
    struct aws_allocator *allocator,
    const struct aws_http_client_connection_options *orig_options,
    aws_client_bootstrap_on_channel_event_fn *on_channel_setup,
    aws_client_bootstrap_on_channel_event_fn *on_channel_shutdown) {

    AWS_FATAL_ASSERT(orig_options->proxy_options != NULL);
    /* make copy of options, and add defaults for missing optional structs */
    struct aws_http_client_connection_options options = *orig_options;

    struct aws_http1_connection_options default_http1_options;
    AWS_ZERO_STRUCT(default_http1_options);
    if (options.http1_options == NULL) {
        options.http1_options = &default_http1_options;
    }

    struct aws_http2_connection_options default_http2_options;
    AWS_ZERO_STRUCT(default_http2_options);
    if (options.http2_options == NULL) {
        options.http2_options = &default_http2_options;
    }

    struct aws_http2_setting *setting_array = NULL;
    struct aws_http_proxy_user_data *user_data = NULL;
    aws_mem_acquire_many(
        options.allocator,
        2,
        &user_data,
        sizeof(struct aws_http_proxy_user_data),
        &setting_array,
        options.http2_options->num_initial_settings * sizeof(struct aws_http2_setting));
    AWS_ZERO_STRUCT(*user_data);

    user_data->allocator = allocator;
    user_data->state = AWS_PBS_SOCKET_CONNECT;
    user_data->error_code = AWS_ERROR_SUCCESS;
    user_data->connect_status_code = AWS_HTTP_STATUS_CODE_UNKNOWN;
    user_data->original_bootstrap = aws_client_bootstrap_acquire(options.bootstrap);
    if (options.socket_options != NULL) {
        user_data->original_socket_options = *options.socket_options;
    }
    user_data->original_manual_window_management = options.manual_window_management;
    user_data->original_initial_window_size = options.initial_window_size;

    user_data->original_host = aws_string_new_from_cursor(allocator, &options.host_name);
    if (user_data->original_host == NULL) {
        goto on_error;
    }

    user_data->original_port = options.port;

    user_data->proxy_config = aws_http_proxy_config_new_from_connection_options(allocator, &options);
    if (user_data->proxy_config == NULL) {
        goto on_error;
    }

    user_data->proxy_negotiator =
        aws_http_proxy_strategy_create_negotiator(user_data->proxy_config->proxy_strategy, allocator);
    if (user_data->proxy_negotiator == NULL) {
        goto on_error;
    }

    if (options.tls_options) {
        /* clone tls options, but redirect user data to what we're creating */
        user_data->original_tls_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_tls_connection_options));
        if (user_data->original_tls_options == NULL ||
            aws_tls_connection_options_copy(user_data->original_tls_options, options.tls_options)) {
            goto on_error;
        }

        user_data->original_tls_options->user_data = user_data;
    }

    if (aws_http_alpn_map_init_copy(options.allocator, &user_data->alpn_string_map, options.alpn_string_map)) {
        goto on_error;
    }

    user_data->original_http_on_setup = options.on_setup;
    user_data->original_http_on_shutdown = options.on_shutdown;
    user_data->original_channel_on_setup = on_channel_setup;
    user_data->original_channel_on_shutdown = on_channel_shutdown;
    user_data->requested_event_loop = options.requested_event_loop;
    user_data->host_resolution_config = options.host_resolution_config;
    user_data->prior_knowledge_http2 = options.prior_knowledge_http2;

    /* one and only one setup callback must be valid */
    AWS_FATAL_ASSERT((user_data->original_http_on_setup == NULL) != (user_data->original_channel_on_setup == NULL));

    /* one and only one shutdown callback must be valid */
    AWS_FATAL_ASSERT(
        (user_data->original_http_on_shutdown == NULL) != (user_data->original_channel_on_shutdown == NULL));

    /* callback set must be self-consistent.  Technically the second check is redundant given the previous checks */
    AWS_FATAL_ASSERT((user_data->original_http_on_setup == NULL) == (user_data->original_http_on_shutdown == NULL));
    AWS_FATAL_ASSERT(
        (user_data->original_channel_on_setup == NULL) == (user_data->original_channel_on_shutdown == NULL));

    user_data->original_user_data = options.user_data;
    user_data->original_http1_options = *options.http1_options;
    user_data->original_http2_options = *options.http2_options;

    /* keep a copy of the settings array if it's not NULL */
    if (options.http2_options->num_initial_settings > 0) {
        memcpy(
            setting_array,
            options.http2_options->initial_settings_array,
            options.http2_options->num_initial_settings * sizeof(struct aws_http2_setting));
        user_data->original_http2_options.initial_settings_array = setting_array;
    }

    return user_data;

on_error:

    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "(STATIC) Proxy connection failed to create user data with error %d(%s)",
        aws_last_error(),
        aws_error_str(aws_last_error()));

    aws_http_proxy_user_data_destroy(user_data);

    return NULL;
}

struct aws_http_proxy_user_data *aws_http_proxy_user_data_new_reset_clone(
    struct aws_allocator *allocator,
    struct aws_http_proxy_user_data *old_user_data) {

    AWS_FATAL_ASSERT(old_user_data != NULL);

    struct aws_http2_setting *setting_array = NULL;
    struct aws_http_proxy_user_data *user_data = NULL;
    aws_mem_acquire_many(
        allocator,
        2,
        &user_data,
        sizeof(struct aws_http_proxy_user_data),
        &setting_array,
        old_user_data->original_http2_options.num_initial_settings * sizeof(struct aws_http2_setting));

    AWS_ZERO_STRUCT(*user_data);
    user_data->allocator = allocator;
    user_data->state = AWS_PBS_SOCKET_CONNECT;
    user_data->error_code = AWS_ERROR_SUCCESS;
    user_data->connect_status_code = AWS_HTTP_STATUS_CODE_UNKNOWN;
    user_data->original_bootstrap = aws_client_bootstrap_acquire(old_user_data->original_bootstrap);
    user_data->original_socket_options = old_user_data->original_socket_options;
    user_data->original_manual_window_management = old_user_data->original_manual_window_management;
    user_data->original_initial_window_size = old_user_data->original_initial_window_size;
    user_data->prior_knowledge_http2 = old_user_data->prior_knowledge_http2;

    user_data->original_host = aws_string_new_from_string(allocator, old_user_data->original_host);
    if (user_data->original_host == NULL) {
        goto on_error;
    }

    user_data->original_port = old_user_data->original_port;

    user_data->proxy_config = aws_http_proxy_config_new_clone(allocator, old_user_data->proxy_config);
    if (user_data->proxy_config == NULL) {
        goto on_error;
    }

    user_data->proxy_negotiator = aws_http_proxy_negotiator_acquire(old_user_data->proxy_negotiator);
    if (user_data->proxy_negotiator == NULL) {
        goto on_error;
    }

    if (old_user_data->original_tls_options) {
        /* clone tls options, but redirect user data to what we're creating */
        user_data->original_tls_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_tls_connection_options));
        if (user_data->original_tls_options == NULL ||
            aws_tls_connection_options_copy(user_data->original_tls_options, old_user_data->original_tls_options)) {
            goto on_error;
        }

        user_data->original_tls_options->user_data = user_data;
    }

    if (aws_http_alpn_map_init_copy(allocator, &user_data->alpn_string_map, &old_user_data->alpn_string_map)) {
        goto on_error;
    }

    user_data->original_http_on_setup = old_user_data->original_http_on_setup;
    user_data->original_http_on_shutdown = old_user_data->original_http_on_shutdown;
    user_data->original_channel_on_setup = old_user_data->original_channel_on_setup;
    user_data->original_channel_on_shutdown = old_user_data->original_channel_on_shutdown;
    user_data->original_user_data = old_user_data->original_user_data;
    user_data->original_http1_options = old_user_data->original_http1_options;
    user_data->original_http2_options = old_user_data->original_http2_options;

    /* keep a copy of the settings array if it's not NULL */
    if (old_user_data->original_http2_options.num_initial_settings > 0) {
        memcpy(
            setting_array,
            old_user_data->original_http2_options.initial_settings_array,
            old_user_data->original_http2_options.num_initial_settings * sizeof(struct aws_http2_setting));
        user_data->original_http2_options.initial_settings_array = setting_array;
    }

    return user_data;

on_error:

    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "(STATIC) Proxy connection failed to create user data with error %d(%s)",
        aws_last_error(),
        aws_error_str(aws_last_error()));

    aws_http_proxy_user_data_destroy(user_data);

    return NULL;
}

/*
 * Examines the proxy user data state and determines whether to make an http-interface setup callback
 * or a raw channel setup callback
 */
static void s_do_on_setup_callback(
    struct aws_http_proxy_user_data *proxy_ud,
    struct aws_http_connection *connection,
    int error_code) {
    if (proxy_ud->original_http_on_setup) {
        proxy_ud->original_http_on_setup(connection, error_code, proxy_ud->original_user_data);
        proxy_ud->original_http_on_setup = NULL;
    }

    if (proxy_ud->original_channel_on_setup) {
        struct aws_channel *channel = NULL;
        if (connection != NULL) {
            channel = aws_http_connection_get_channel(connection);
        }
        proxy_ud->original_channel_on_setup(
            proxy_ud->original_bootstrap, error_code, channel, proxy_ud->original_user_data);
        proxy_ud->original_channel_on_setup = NULL;
    }
}

/*
 * Examines the proxy user data state and determines whether to make an http-interface shutdown callback
 * or a raw channel shutdown callback
 */
static void s_do_on_shutdown_callback(struct aws_http_proxy_user_data *proxy_ud, int error_code) {
    AWS_FATAL_ASSERT(proxy_ud->proxy_connection);

    if (proxy_ud->original_http_on_shutdown) {
        AWS_FATAL_ASSERT(proxy_ud->final_connection);
        proxy_ud->original_http_on_shutdown(proxy_ud->final_connection, error_code, proxy_ud->original_user_data);
        proxy_ud->original_http_on_shutdown = NULL;
    }

    if (proxy_ud->original_channel_on_shutdown) {
        struct aws_channel *channel = aws_http_connection_get_channel(proxy_ud->proxy_connection);
        proxy_ud->original_channel_on_shutdown(
            proxy_ud->original_bootstrap, error_code, channel, proxy_ud->original_user_data);
        proxy_ud->original_channel_on_shutdown = NULL;
    }
}

/*
 * Connection callback used ONLY by forwarding http proxy connections.  After this,
 * the connection is live and the user is notified
 */
static void s_aws_http_on_client_connection_http_forwarding_proxy_setup_fn(
    struct aws_http_connection *connection,
    int error_code,
    void *user_data) {
    struct aws_http_proxy_user_data *proxy_ud = user_data;

    s_do_on_setup_callback(proxy_ud, connection, error_code);

    if (error_code != AWS_ERROR_SUCCESS) {
        aws_http_proxy_user_data_destroy(user_data);
    } else {
        /*
         * The proxy connection and final connection are the same in forwarding proxy connections.  This lets
         * us unconditionally use fatal asserts on these being non-null regardless of proxy configuration.
         */
        proxy_ud->proxy_connection = connection;
        proxy_ud->final_connection = connection;
        proxy_ud->state = AWS_PBS_SUCCESS;
    }
}

/*
 * Connection shutdown callback used by both http and https proxy connections.  Only invokes
 * user shutdown if the connection was successfully established.  Otherwise, it invokes
 * the user setup function with an error.
 */
static void s_aws_http_on_client_connection_http_proxy_shutdown_fn(
    struct aws_http_connection *connection,
    int error_code,
    void *user_data) {

    struct aws_http_proxy_user_data *proxy_ud = user_data;

    if (proxy_ud->state == AWS_PBS_SUCCESS) {
        AWS_LOGF_INFO(
            AWS_LS_HTTP_CONNECTION,
            "(%p) Proxy connection (channel %p) shutting down.",
            (void *)connection,
            (void *)aws_http_connection_get_channel(connection));
        s_do_on_shutdown_callback(proxy_ud, error_code);
    } else {
        int ec = error_code;
        if (ec == AWS_ERROR_SUCCESS) {
            ec = proxy_ud->error_code;
        }
        if (ec == AWS_ERROR_SUCCESS) {
            ec = AWS_ERROR_UNKNOWN;
        }

        AWS_LOGF_WARN(
            AWS_LS_HTTP_CONNECTION,
            "(%p) Error %d while connecting to \"%s\" via proxy.",
            (void *)connection,
            ec,
            (char *)proxy_ud->original_host->bytes);

        s_do_on_setup_callback(proxy_ud, NULL, ec);
    }

    aws_http_proxy_user_data_destroy(user_data);
}

/*
 * On-any-error entry point that releases all resources involved in establishing the proxy connection.
 * This must not be invoked any time after a successful setup callback.
 */
static void s_aws_http_proxy_user_data_shutdown(struct aws_http_proxy_user_data *user_data) {

    user_data->state = AWS_PBS_FAILURE;

    if (user_data->proxy_connection == NULL) {
        s_do_on_setup_callback(user_data, NULL, user_data->error_code);
        aws_http_proxy_user_data_destroy(user_data);
        return;
    }

    if (user_data->connect_stream) {
        aws_http_stream_release(user_data->connect_stream);
        user_data->connect_stream = NULL;
    }

    if (user_data->connect_request) {
        aws_http_message_destroy(user_data->connect_request);
        user_data->connect_request = NULL;
    }

    struct aws_http_connection *http_connection = user_data->proxy_connection;
    user_data->proxy_connection = NULL;

    aws_channel_shutdown(http_connection->channel_slot->channel, user_data->error_code);
    aws_http_connection_release(http_connection);
}

static struct aws_http_message *s_build_h1_proxy_connect_request(struct aws_http_proxy_user_data *user_data) {
    struct aws_http_message *request = aws_http_message_new_request(user_data->allocator);
    if (request == NULL) {
        return NULL;
    }

    struct aws_byte_buf path_buffer;
    AWS_ZERO_STRUCT(path_buffer);

    if (aws_http_message_set_request_method(request, aws_http_method_connect)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&path_buffer, user_data->allocator, user_data->original_host->len + 10)) {
        goto on_error;
    }

    struct aws_byte_cursor host_cursor = aws_byte_cursor_from_string(user_data->original_host);
    if (aws_byte_buf_append(&path_buffer, &host_cursor)) {
        goto on_error;
    }

    struct aws_byte_cursor colon_cursor = aws_byte_cursor_from_c_str(":");
    if (aws_byte_buf_append(&path_buffer, &colon_cursor)) {
        goto on_error;
    }

    char port_str[20] = "\0";
    snprintf(port_str, sizeof(port_str), "%u", user_data->original_port);
    struct aws_byte_cursor port_cursor = aws_byte_cursor_from_c_str(port_str);
    if (aws_byte_buf_append(&path_buffer, &port_cursor)) {
        goto on_error;
    }

    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_array(path_buffer.buffer, path_buffer.len);
    if (aws_http_message_set_request_path(request, path_cursor)) {
        goto on_error;
    }

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_string(s_host_header_name),
        .value = aws_byte_cursor_from_array(path_buffer.buffer, path_buffer.len),
    };
    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    struct aws_http_header keep_alive_header = {
        .name = aws_byte_cursor_from_string(s_proxy_connection_header_name),
        .value = aws_byte_cursor_from_string(s_proxy_connection_header_value),
    };
    if (aws_http_message_add_header(request, keep_alive_header)) {
        goto on_error;
    }

    aws_byte_buf_clean_up(&path_buffer);

    return request;

on_error:

    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "(%p) TLS proxy connection failed to build CONNECT request with error %d(%s)",
        (void *)user_data->proxy_connection,
        aws_last_error(),
        aws_error_str(aws_last_error()));

    aws_byte_buf_clean_up(&path_buffer);
    aws_http_message_destroy(request);

    return NULL;
}

/*
 * Builds the CONNECT request issued after proxy connection establishment, during the creation of
 * tls-enabled proxy connections.
 */
static struct aws_http_message *s_build_proxy_connect_request(struct aws_http_proxy_user_data *user_data) {
    struct aws_http_connection *proxy_connection = user_data->proxy_connection;
    switch (proxy_connection->http_version) {
        case AWS_HTTP_VERSION_1_1:
            return s_build_h1_proxy_connect_request(user_data);
        default:
            aws_raise_error(AWS_ERROR_HTTP_UNSUPPORTED_PROTOCOL);
            return NULL;
    }
}

static int s_aws_http_on_incoming_body_tunnel_proxy(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {
    (void)stream;

    struct aws_http_proxy_user_data *context = user_data;
    aws_http_proxy_negotiator_connect_on_incoming_body_fn *on_incoming_body =
        context->proxy_negotiator->strategy_vtable.tunnelling_vtable->on_incoming_body_callback;
    if (on_incoming_body != NULL) {
        (*on_incoming_body)(context->proxy_negotiator, data);
    }

    aws_http_stream_update_window(stream, data->len);

    return AWS_OP_SUCCESS;
}

static int s_aws_http_on_response_headers_tunnel_proxy(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data) {
    (void)stream;

    struct aws_http_proxy_user_data *context = user_data;
    aws_http_proxy_negotiation_connect_on_incoming_headers_fn *on_incoming_headers =
        context->proxy_negotiator->strategy_vtable.tunnelling_vtable->on_incoming_headers_callback;
    if (on_incoming_headers != NULL) {
        (*on_incoming_headers)(context->proxy_negotiator, header_block, header_array, num_headers);
    }

    return AWS_OP_SUCCESS;
}

/*
 * Headers done callback for the CONNECT request made during tls proxy connections
 */
static int s_aws_http_on_incoming_header_block_done_tunnel_proxy(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    void *user_data) {

    struct aws_http_proxy_user_data *context = user_data;

    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        int status_code = AWS_HTTP_STATUS_CODE_UNKNOWN;
        aws_http_stream_get_incoming_response_status(stream, &status_code);
        context->connect_status_code = (enum aws_http_status_code)status_code;
        if (context->connect_status_code != AWS_HTTP_STATUS_CODE_200_OK) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "(%p) Proxy CONNECT request failed with status code %d",
                (void *)context->proxy_connection,
                context->connect_status_code);
            context->error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
        }

        aws_http_proxy_negotiator_connect_status_fn *on_status =
            context->proxy_negotiator->strategy_vtable.tunnelling_vtable->on_status_callback;
        if (on_status != NULL) {
            (*on_status)(context->proxy_negotiator, context->connect_status_code);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_aws_http_apply_http_connection_to_proxied_channel(struct aws_http_proxy_user_data *context) {
    AWS_FATAL_ASSERT(context->proxy_connection != NULL);
    AWS_FATAL_ASSERT(context->original_http_on_setup != NULL);

    struct aws_channel *channel = aws_http_connection_get_channel(context->proxy_connection);

    struct aws_http_connection *connection = aws_http_connection_new_channel_handler(
        context->allocator,
        channel,
        false,
        context->original_tls_options != NULL,
        context->original_manual_window_management,
        context->prior_knowledge_http2,
        context->original_initial_window_size,
        context->alpn_string_map.p_impl == NULL ? NULL : &context->alpn_string_map,
        &context->original_http1_options,
        &context->original_http2_options,
        context->original_user_data);
    if (connection == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "static: Failed to create the client connection object, error %d (%s).",
            aws_last_error(),
            aws_error_name(aws_last_error()));

        return AWS_OP_ERR;
    }

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "id=%p: " PRInSTR " client connection established.",
        (void *)connection,
        AWS_BYTE_CURSOR_PRI(aws_http_version_to_str(connection->http_version)));

    context->final_connection = connection;

    return AWS_OP_SUCCESS;
}

static void s_do_final_proxied_channel_setup(struct aws_http_proxy_user_data *proxy_ud) {
    if (proxy_ud->original_http_on_setup != NULL) {
        /*
         * If we're transitioning to http with http setup/shutdown callbacks, try to apply a new http connection to
         * the channel
         */
        if (s_aws_http_apply_http_connection_to_proxied_channel(proxy_ud)) {
            proxy_ud->error_code = aws_last_error();
            s_aws_http_proxy_user_data_shutdown(proxy_ud);
            return;
        }

        s_do_on_setup_callback(proxy_ud, proxy_ud->final_connection, AWS_ERROR_SUCCESS);
    } else {
        /*
         * Otherwise invoke setup directly (which will end up being channel setup)
         */
        s_do_on_setup_callback(proxy_ud, proxy_ud->proxy_connection, AWS_ERROR_SUCCESS);
    }

    /* Tell user of successful connection. */
    proxy_ud->state = AWS_PBS_SUCCESS;
}

/*
 * Tls negotiation callback for tls proxy connections
 */
static void s_on_origin_server_tls_negotation_result(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    int error_code,
    void *user_data) {

    (void)handler;
    (void)slot;

    struct aws_http_proxy_user_data *context = user_data;
    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "(%p) Proxy connection failed origin server TLS negotiation with error %d(%s)",
            (void *)context->proxy_connection,
            error_code,
            aws_error_str(error_code));
        context->error_code = error_code;
        s_aws_http_proxy_user_data_shutdown(context);
        return;
    }

    s_do_final_proxied_channel_setup(context);
}

static int s_create_tunneling_connection(struct aws_http_proxy_user_data *user_data);
static int s_make_proxy_connect_request(struct aws_http_proxy_user_data *user_data);

static void s_zero_callbacks(struct aws_http_proxy_user_data *proxy_ud) {
    proxy_ud->original_http_on_shutdown = NULL;
    proxy_ud->original_http_on_setup = NULL;
    proxy_ud->original_channel_on_shutdown = NULL;
    proxy_ud->original_channel_on_setup = NULL;
}

/*
 * Stream done callback for the CONNECT request made during tls proxy connections
 */
static void s_aws_http_on_stream_complete_tunnel_proxy(
    struct aws_http_stream *stream,
    int error_code,
    void *user_data) {
    struct aws_http_proxy_user_data *context = user_data;
    AWS_FATAL_ASSERT(stream == context->connect_stream);

    if (context->error_code == AWS_ERROR_SUCCESS && error_code != AWS_ERROR_SUCCESS) {
        context->error_code = error_code;
    }

    if (context->error_code != AWS_ERROR_SUCCESS) {
        context->error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED;
        if (context->connect_status_code == AWS_HTTP_STATUS_CODE_407_PROXY_AUTHENTICATION_REQUIRED) {
            enum aws_http_proxy_negotiation_retry_directive retry_directive =
                aws_http_proxy_negotiator_get_retry_directive(context->proxy_negotiator);

            if (retry_directive == AWS_HPNRD_NEW_CONNECTION) {
                struct aws_http_proxy_user_data *new_context =
                    aws_http_proxy_user_data_new_reset_clone(context->allocator, context);
                if (new_context != NULL && s_create_tunneling_connection(new_context) == AWS_OP_SUCCESS) {
                    /*
                     * We successfully kicked off a new connection.  By NULLing the callbacks on the old one, we can
                     * shut it down quietly without the user being notified.  The new connection will notify the user
                     * based on its success or failure.
                     */
                    s_zero_callbacks(context);
                    context->error_code = AWS_ERROR_HTTP_PROXY_CONNECT_FAILED_RETRYABLE;
                }
            } else if (retry_directive == AWS_HPNRD_CURRENT_CONNECTION) {
                context->error_code = AWS_ERROR_SUCCESS;
                if (s_make_proxy_connect_request(context) == AWS_OP_SUCCESS) {
                    return;
                }
            }
        }

        s_aws_http_proxy_user_data_shutdown(context);
        return;
    }

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "(%p) Proxy connection made successful CONNECT request to \"%s\" via proxy",
        (void *)context->proxy_connection,
        context->original_host->bytes);

    /*
     * We're finished with these, let's release
     */
    aws_http_stream_release(stream);
    context->connect_stream = NULL;
    aws_http_message_destroy(context->connect_request);
    context->connect_request = NULL;

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION, "(%p) Beginning TLS negotiation through proxy", (void *)context->proxy_connection);

    if (context->original_tls_options != NULL) {
        /*
         * Perform TLS negotiation to the origin server through proxy
         */
        context->original_tls_options->on_negotiation_result = s_on_origin_server_tls_negotation_result;

        context->state = AWS_PBS_TLS_NEGOTIATION;
        struct aws_channel *channel = aws_http_connection_get_channel(context->proxy_connection);

        struct aws_channel_slot *last_slot = aws_channel_get_first_slot(channel);
        while (last_slot->adj_right != NULL) {
            last_slot = last_slot->adj_right;
        }

        if (s_vtable->aws_channel_setup_client_tls(last_slot, context->original_tls_options)) {
            AWS_LOGF_ERROR(
                AWS_LS_HTTP_CONNECTION,
                "(%p) Proxy connection failed to start TLS negotiation with error %d(%s)",
                (void *)context->proxy_connection,
                aws_last_error(),
                aws_error_str(aws_last_error()));
            s_aws_http_proxy_user_data_shutdown(context);
            return;
        }
    } else {
        s_do_final_proxied_channel_setup(context);
    }
}

static void s_terminate_tunneling_connect(
    struct aws_http_message *message,
    int error_code,
    void *internal_proxy_user_data) {
    (void)message;

    struct aws_http_proxy_user_data *proxy_ud = internal_proxy_user_data;

    AWS_LOGF_ERROR(
        AWS_LS_HTTP_CONNECTION,
        "(%p) Tunneling proxy connection failed to create request stream for CONNECT request with error %d(%s)",
        (void *)proxy_ud->proxy_connection,
        error_code,
        aws_error_str(error_code));

    proxy_ud->error_code = error_code;
    s_aws_http_proxy_user_data_shutdown(proxy_ud);
}

static void s_continue_tunneling_connect(struct aws_http_message *message, void *internal_proxy_user_data) {
    struct aws_http_proxy_user_data *proxy_ud = internal_proxy_user_data;

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .request = message,
        .user_data = proxy_ud,
        .on_response_headers = s_aws_http_on_response_headers_tunnel_proxy,
        .on_response_header_block_done = s_aws_http_on_incoming_header_block_done_tunnel_proxy,
        .on_response_body = s_aws_http_on_incoming_body_tunnel_proxy,
        .on_complete = s_aws_http_on_stream_complete_tunnel_proxy,
    };

    if (proxy_ud->connect_stream != NULL) {
        aws_http_stream_release(proxy_ud->connect_stream);
    }

    proxy_ud->connect_stream = aws_http_connection_make_request(proxy_ud->proxy_connection, &request_options);
    if (proxy_ud->connect_stream == NULL) {
        goto on_error;
    }

    aws_http_stream_activate(proxy_ud->connect_stream);

    return;

on_error:

    s_aws_http_proxy_user_data_shutdown(proxy_ud);
}

/*
 * Issues a CONNECT request on an http connection
 */
static int s_make_proxy_connect_request(struct aws_http_proxy_user_data *user_data) {
    if (user_data->connect_request != NULL) {
        aws_http_message_destroy(user_data->connect_request);
        user_data->connect_request = NULL;
    }

    user_data->connect_request = s_build_proxy_connect_request(user_data);
    if (user_data->connect_request == NULL) {
        return AWS_OP_ERR;
    }

    (*user_data->proxy_negotiator->strategy_vtable.tunnelling_vtable->connect_request_transform)(
        user_data->proxy_negotiator,
        user_data->connect_request,
        s_terminate_tunneling_connect,
        s_continue_tunneling_connect,
        user_data);

    return AWS_OP_SUCCESS;
}

/*
 * Connection setup callback for tunneling proxy connections.
 */
static void s_aws_http_on_client_connection_http_tunneling_proxy_setup_fn(
    struct aws_http_connection *connection,
    int error_code,
    void *user_data) {

    struct aws_http_proxy_user_data *proxy_ud = user_data;

    proxy_ud->error_code = error_code;
    if (error_code != AWS_ERROR_SUCCESS) {
        goto on_error;
    }

    AWS_LOGF_INFO(AWS_LS_HTTP_CONNECTION, "(%p) Making CONNECT request to proxy", (void *)proxy_ud->proxy_connection);

    proxy_ud->proxy_connection = connection;
    proxy_ud->state = AWS_PBS_HTTP_CONNECT;
    if (s_make_proxy_connect_request(proxy_ud)) {
        goto on_error;
    }

    return;

on_error:

    s_aws_http_proxy_user_data_shutdown(proxy_ud);
}

/*
 * Checks for the special case when a request is an OPTIONS request with *
 * path and no query params
 */
static bool s_is_star_path_options_method(const struct aws_http_message *request) {
    struct aws_byte_cursor method_cursor;
    if (aws_http_message_get_request_method(request, &method_cursor)) {
        return false;
    }

    struct aws_byte_cursor options_cursor = aws_byte_cursor_from_string(s_options_method);
    if (!aws_byte_cursor_eq_ignore_case(&method_cursor, &options_cursor)) {
        return false;
    }

    struct aws_byte_cursor path_cursor;
    if (aws_http_message_get_request_path(request, &path_cursor)) {
        return false;
    }

    struct aws_byte_cursor star_cursor = aws_byte_cursor_from_string(s_star_path);
    if (!aws_byte_cursor_eq_ignore_case(&path_cursor, &star_cursor)) {
        return false;
    }

    return true;
}

/*
 * Modifies a requests uri by transforming it to absolute form according to
 * section 5.3.2 of rfc 7230
 *
 * We do this by parsing the existing uri and then rebuilding it as an
 * absolute resource path (using the original connection options)
 */
int aws_http_rewrite_uri_for_proxy_request(
    struct aws_http_message *request,
    struct aws_http_proxy_user_data *proxy_user_data) {
    int result = AWS_OP_ERR;

    struct aws_uri target_uri;
    AWS_ZERO_STRUCT(target_uri);

    struct aws_byte_cursor path_cursor;
    AWS_ZERO_STRUCT(path_cursor);

    if (aws_http_message_get_request_path(request, &path_cursor)) {
        goto done;
    }

    /* Pull out the original path/query */
    struct aws_uri uri;
    if (aws_uri_init_parse(&uri, proxy_user_data->allocator, &path_cursor)) {
        goto done;
    }

    const struct aws_byte_cursor *actual_path_cursor = aws_uri_path(&uri);
    const struct aws_byte_cursor *actual_query_cursor = aws_uri_query_string(&uri);

    /* now rebuild the uri with scheme, host and port subbed in from the original connection options */
    struct aws_uri_builder_options target_uri_builder;
    AWS_ZERO_STRUCT(target_uri_builder);
    target_uri_builder.scheme = aws_http_scheme_http;
    target_uri_builder.path = *actual_path_cursor;
    target_uri_builder.host_name = aws_byte_cursor_from_string(proxy_user_data->original_host);
    target_uri_builder.port = proxy_user_data->original_port;
    target_uri_builder.query_string = *actual_query_cursor;

    if (aws_uri_init_from_builder_options(&target_uri, proxy_user_data->allocator, &target_uri_builder)) {
        goto done;
    }

    struct aws_byte_cursor full_target_uri =
        aws_byte_cursor_from_array(target_uri.uri_str.buffer, target_uri.uri_str.len);

    /*
     * By rfc 7230, Section 5.3.4, a star-pathed options request made through a proxy MUST be transformed (at the last
     * proxy) back into a star-pathed request if the proxy request has an empty path and no query string.  This
     * is behavior we want to support.  So from our side, we need to make sure that star-pathed options requests
     * get translated into options requests with the authority as the uri and an empty path-query.
     *
     * Our URI transform always ends with a '/' which is technically not an empty path. To address this,
     * the easiest thing to do is just detect if this was originally a star-pathed options request
     * and drop the final '/' from the path.
     */
    if (s_is_star_path_options_method(request)) {
        if (full_target_uri.len > 0 && *(full_target_uri.ptr + full_target_uri.len - 1) == '/') {
            full_target_uri.len -= 1;
        }
    }

    /* mutate the request with the new path value */
    if (aws_http_message_set_request_path(request, full_target_uri)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_uri_clean_up(&target_uri);
    aws_uri_clean_up(&uri);

    return result;
}

/*
 * Plaintext proxy request transformation function
 *
 * Rewrites the target uri to absolute form and injects any desired headers
 */
static int s_proxy_http_request_transform(struct aws_http_message *request, void *user_data) {
    struct aws_http_proxy_user_data *proxy_ud = user_data;

    if (aws_http_rewrite_uri_for_proxy_request(request, proxy_ud)) {
        return AWS_OP_ERR;
    }

    if ((*proxy_ud->proxy_negotiator->strategy_vtable.forwarding_vtable->forward_request_transform)(
            proxy_ud->proxy_negotiator, request)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Top-level function to route a connection request through a proxy server, with no channel security
 */
static int s_aws_http_client_connect_via_forwarding_proxy(const struct aws_http_client_connection_options *options) {
    AWS_FATAL_ASSERT(options->tls_options == NULL);

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "(STATIC) Connecting to \"" PRInSTR "\" via proxy \"" PRInSTR "\"",
        AWS_BYTE_CURSOR_PRI(options->host_name),
        AWS_BYTE_CURSOR_PRI(options->proxy_options->host));

    /* Create a wrapper user data that contains all proxy-related information, state, and user-facing callbacks */
    struct aws_http_proxy_user_data *proxy_user_data =
        aws_http_proxy_user_data_new(options->allocator, options, NULL, NULL);
    if (proxy_user_data == NULL) {
        return AWS_OP_ERR;
    }

    AWS_FATAL_ASSERT(options->proxy_options != NULL);

    /* Fill in a new connection options pointing at the proxy */
    struct aws_http_client_connection_options options_copy = *options;

    options_copy.proxy_options = NULL;
    options_copy.host_name = options->proxy_options->host;
    options_copy.port = options->proxy_options->port;
    options_copy.user_data = proxy_user_data;
    options_copy.on_setup = s_aws_http_on_client_connection_http_forwarding_proxy_setup_fn;
    options_copy.on_shutdown = s_aws_http_on_client_connection_http_proxy_shutdown_fn;
    options_copy.tls_options = options->proxy_options->tls_options;
    options_copy.requested_event_loop = options->requested_event_loop;
    options_copy.host_resolution_config = options->host_resolution_config;
    options_copy.prior_knowledge_http2 = false; /* ToDo, expose the protocol specific config for proxy connection. */

    int result = aws_http_client_connect_internal(&options_copy, s_proxy_http_request_transform);
    if (result == AWS_OP_ERR) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "(STATIC) Proxy http connection failed client connect with error %d(%s)",
            aws_last_error(),
            aws_error_str(aws_last_error()));

        aws_http_proxy_user_data_destroy(proxy_user_data);
    }

    return result;
}

static int s_create_tunneling_connection(struct aws_http_proxy_user_data *user_data) {
    struct aws_http_client_connection_options connect_options;
    AWS_ZERO_STRUCT(connect_options);

    connect_options.self_size = sizeof(struct aws_http_client_connection_options);
    connect_options.allocator = user_data->allocator;
    connect_options.bootstrap = user_data->original_bootstrap;
    connect_options.host_name = aws_byte_cursor_from_buf(&user_data->proxy_config->host);
    connect_options.port = user_data->proxy_config->port;
    connect_options.socket_options = &user_data->original_socket_options;
    connect_options.tls_options = user_data->proxy_config->tls_options;
    connect_options.monitoring_options = NULL; /* ToDo */
    connect_options.manual_window_management = user_data->original_manual_window_management;
    connect_options.initial_window_size = user_data->original_initial_window_size;
    connect_options.user_data = user_data;
    connect_options.on_setup = s_aws_http_on_client_connection_http_tunneling_proxy_setup_fn;
    connect_options.on_shutdown = s_aws_http_on_client_connection_http_proxy_shutdown_fn;
    connect_options.http1_options = NULL; /* ToDo, expose the protocol specific config for proxy connection. */
    connect_options.http2_options = NULL; /* ToDo */
    connect_options.requested_event_loop = user_data->requested_event_loop;
    connect_options.host_resolution_config = user_data->host_resolution_config;

    int result = aws_http_client_connect(&connect_options);
    if (result == AWS_OP_ERR) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "(STATIC) Proxy tunnel connection failed client connect with error %d(%s)",
            aws_last_error(),
            aws_error_str(aws_last_error()));
        aws_http_proxy_user_data_destroy(user_data);
    }

    return result;
}

/*
 * Top-level function to route a connection through a proxy server via a CONNECT request
 */
static int s_aws_http_client_connect_via_tunneling_proxy(
    const struct aws_http_client_connection_options *options,
    aws_client_bootstrap_on_channel_event_fn *on_channel_setup,
    aws_client_bootstrap_on_channel_event_fn *on_channel_shutdown) {
    AWS_FATAL_ASSERT(options->proxy_options != NULL);

    AWS_LOGF_INFO(
        AWS_LS_HTTP_CONNECTION,
        "(STATIC) Connecting to \"" PRInSTR "\" through a tunnel via proxy \"" PRInSTR "\"",
        AWS_BYTE_CURSOR_PRI(options->host_name),
        AWS_BYTE_CURSOR_PRI(options->proxy_options->host));

    /* Create a wrapper user data that contains all proxy-related information, state, and user-facing callbacks */
    struct aws_http_proxy_user_data *user_data =
        aws_http_proxy_user_data_new(options->allocator, options, on_channel_setup, on_channel_shutdown);
    if (user_data == NULL) {
        return AWS_OP_ERR;
    }

    return s_create_tunneling_connection(user_data);
}

static enum aws_http_proxy_connection_type s_determine_proxy_connection_type(
    enum aws_http_proxy_connection_type proxy_connection_type,
    bool is_tls_connection) {
    if (proxy_connection_type != AWS_HPCT_HTTP_LEGACY) {
        return proxy_connection_type;
    }

    if (is_tls_connection) {
        return AWS_HPCT_HTTP_TUNNEL;
    } else {
        return AWS_HPCT_HTTP_FORWARD;
    }
}

static struct aws_string *s_get_proxy_environment_value(
    struct aws_allocator *allocator,
    const struct aws_string *env_name) {
    struct aws_string *out_string = NULL;
    if (aws_get_environment_value(allocator, env_name, &out_string) == AWS_OP_SUCCESS && out_string != NULL &&
        out_string->len > 0) {
        AWS_LOGF_DEBUG(
            AWS_LS_HTTP_CONNECTION,
            "%s environment found, %s",
            aws_string_c_str(env_name),
            aws_string_c_str(out_string));
        return out_string;
    }
    aws_string_destroy(out_string);
    return NULL;
}

static int s_proxy_uri_init_from_env_variable(
    struct aws_allocator *allocator,
    const struct aws_http_client_connection_options *options,
    struct aws_uri *proxy_uri,
    bool *found) {
    struct aws_string *proxy_uri_string = NULL;
    *found = false;
    if (options->tls_options) {
        proxy_uri_string = s_get_proxy_environment_value(allocator, s_https_proxy_env_var_low);
        if (proxy_uri_string == NULL) {
            proxy_uri_string = s_get_proxy_environment_value(allocator, s_https_proxy_env_var);
        }
        if (proxy_uri_string == NULL) {
            return AWS_OP_SUCCESS;
        }
    } else {
        proxy_uri_string = s_get_proxy_environment_value(allocator, s_http_proxy_env_var_low);
        if (proxy_uri_string == NULL) {
            proxy_uri_string = s_get_proxy_environment_value(allocator, s_http_proxy_env_var);
        }
        if (proxy_uri_string == NULL) {
            return AWS_OP_SUCCESS;
        }
    }
    struct aws_byte_cursor proxy_uri_cursor = aws_byte_cursor_from_string(proxy_uri_string);
    if (aws_uri_init_parse(proxy_uri, allocator, &proxy_uri_cursor)) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "Could not parse found proxy URI.");
        aws_string_destroy(proxy_uri_string);
        return AWS_OP_ERR;
    }
    *found = true;
    aws_string_destroy(proxy_uri_string);
    return AWS_OP_SUCCESS;
}

static int s_connect_proxy(const struct aws_http_client_connection_options *options) {
    if (aws_http_options_validate_proxy_configuration(options)) {
        return AWS_OP_ERR;
    }

    enum aws_http_proxy_connection_type proxy_connection_type =
        s_determine_proxy_connection_type(options->proxy_options->connection_type, options->tls_options != NULL);

    switch (proxy_connection_type) {
        case AWS_HPCT_HTTP_FORWARD:
            return s_aws_http_client_connect_via_forwarding_proxy(options);

        case AWS_HPCT_HTTP_TUNNEL:
            return s_aws_http_client_connect_via_tunneling_proxy(options, NULL, NULL);

        default:
            return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
    }
}

static int s_setup_proxy_tls_env_variable(
    const struct aws_http_client_connection_options *options,
    struct aws_tls_connection_options *default_tls_connection_options,
    struct aws_http_proxy_options *proxy_options,
    struct aws_uri *proxy_uri) {
    (void)default_tls_connection_options;
    (void)proxy_uri;
    if (options->proxy_ev_settings->tls_options) {
        proxy_options->tls_options = options->proxy_ev_settings->tls_options;
    } else {
#ifdef BYO_CRYPTO
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_CONNECTION,
            "Failed making default TLS context because of BYO_CRYPTO, set up the tls_options for proxy_env_settings to "
            "make it work.");
        return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
#else
        struct aws_tls_ctx *tls_ctx = NULL;
        struct aws_tls_ctx_options tls_ctx_options;
        AWS_ZERO_STRUCT(tls_ctx_options);
        /* create a default tls options */
        aws_tls_ctx_options_init_default_client(&tls_ctx_options, options->allocator);
        struct aws_string *proxy_no_verify_peer_string = NULL;
        if (aws_get_environment_value(
                options->allocator, s_proxy_no_verify_peer_env_var, &proxy_no_verify_peer_string) == AWS_OP_SUCCESS &&
            proxy_no_verify_peer_string != NULL) {
            /* turn off the peer verification, if setup from envrionment variable. Mostly for testing. */
            aws_tls_ctx_options_set_verify_peer(&tls_ctx_options, false);
            aws_string_destroy(proxy_no_verify_peer_string);
        }
        tls_ctx = aws_tls_client_ctx_new(options->allocator, &tls_ctx_options);
        aws_tls_ctx_options_clean_up(&tls_ctx_options);
        if (!tls_ctx) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "Failed to create default TLS context.");
            return AWS_OP_ERR;
        }
        aws_tls_connection_options_init_from_ctx(default_tls_connection_options, tls_ctx);
        /* tls options hold a ref to the ctx */
        aws_tls_ctx_release(tls_ctx);
        if (aws_tls_connection_options_set_server_name(
                default_tls_connection_options, options->allocator, &proxy_uri->host_name)) {
            AWS_LOGF_ERROR(AWS_LS_HTTP_CONNECTION, "Failed set server name for TLS connection options.");
            return AWS_OP_ERR;
        }
        proxy_options->tls_options = default_tls_connection_options;
#endif
    }
    return AWS_OP_SUCCESS;
}

static int s_connect_proxy_via_env_variable(const struct aws_http_client_connection_options *options) {
    struct aws_http_proxy_options proxy_options;
    AWS_ZERO_STRUCT(proxy_options);
    struct aws_uri proxy_uri;
    AWS_ZERO_STRUCT(proxy_uri);
    struct aws_tls_connection_options default_tls_connection_options;
    AWS_ZERO_STRUCT(default_tls_connection_options);
    bool found = false;
    bool success = false;
    if (s_proxy_uri_init_from_env_variable(options->allocator, options, &proxy_uri, &found)) {
        /* Envrionment is set but failed to parse it */
        goto done;
    }
    if (found) {
        proxy_options.host = proxy_uri.host_name;
        proxy_options.port = proxy_uri.port;
        proxy_options.connection_type = options->proxy_ev_settings->connection_type;
        if (proxy_options.connection_type == AWS_HPCT_HTTP_LEGACY) {
            if (options->tls_options) {
                /* Use tunneling when main connection use TLS. */
                proxy_options.connection_type = AWS_HPCT_HTTP_TUNNEL;
            } else {
                /* Use forwarding proxy when main connection use clear text. */
                proxy_options.connection_type = AWS_HPCT_HTTP_FORWARD;
            }
        }
        if (aws_byte_cursor_eq_ignore_case(&proxy_uri.scheme, &aws_http_scheme_https)) {
            if (s_setup_proxy_tls_env_variable(options, &default_tls_connection_options, &proxy_options, &proxy_uri)) {
                goto done;
            }
        }
        /* Support basic authentication. */
        if (proxy_uri.password.len) {
            /* Has no empty password set */
            struct aws_http_proxy_strategy_basic_auth_options config = {
                .proxy_connection_type = proxy_options.connection_type,
                .user_name = proxy_uri.user,
                .password = proxy_uri.password,
            };
            proxy_options.proxy_strategy = aws_http_proxy_strategy_new_basic_auth(options->allocator, &config);
        }
    } else {
        success = true;
        goto done;
    }
    struct aws_http_client_connection_options copied_options = *options;
    copied_options.proxy_options = &proxy_options;
    if (s_connect_proxy(&copied_options)) {
        goto done;
    }
    success = true;
done:
    aws_tls_connection_options_clean_up(&default_tls_connection_options);
    aws_http_proxy_strategy_release(proxy_options.proxy_strategy);
    aws_uri_clean_up(&proxy_uri);
    if (success && !found) {
        /* Successfully, but no envrionment variable found. Connect without proxy */
        return aws_http_client_connect_internal(options, NULL);
    }
    return success ? AWS_OP_SUCCESS : AWS_OP_ERR;
}

/*
 * Dispatches a proxy-enabled connection request to the appropriate top-level connection function
 */
int aws_http_client_connect_via_proxy(const struct aws_http_client_connection_options *options) {
    if (options->proxy_options == NULL && options->proxy_ev_settings &&
        options->proxy_ev_settings->env_var_type == AWS_HPEV_ENABLE) {
        return s_connect_proxy_via_env_variable(options);
    }
    return s_connect_proxy(options);
}

static struct aws_http_proxy_config *s_aws_http_proxy_config_new(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *proxy_options,
    enum aws_http_proxy_connection_type override_proxy_connection_type) {
    AWS_FATAL_ASSERT(proxy_options != NULL);

    struct aws_http_proxy_config *config = aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_config));
    if (config == NULL) {
        return NULL;
    }

    config->allocator = allocator;
    config->connection_type = override_proxy_connection_type;

    if (aws_byte_buf_init_copy_from_cursor(&config->host, allocator, proxy_options->host)) {
        goto on_error;
    }

    if (proxy_options->tls_options) {
        config->tls_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_tls_connection_options));
        if (aws_tls_connection_options_copy(config->tls_options, proxy_options->tls_options)) {
            goto on_error;
        }
    }

    config->port = proxy_options->port;

    if (proxy_options->proxy_strategy != NULL) {
        config->proxy_strategy = aws_http_proxy_strategy_acquire(proxy_options->proxy_strategy);
    } else if (proxy_options->auth_type == AWS_HPAT_BASIC) {
        struct aws_http_proxy_strategy_basic_auth_options basic_config;
        AWS_ZERO_STRUCT(basic_config);

        basic_config.proxy_connection_type = override_proxy_connection_type;
        basic_config.user_name = proxy_options->auth_username;
        basic_config.password = proxy_options->auth_password;

        config->proxy_strategy = aws_http_proxy_strategy_new_basic_auth(allocator, &basic_config);
    }

    if (config->proxy_strategy == NULL) {
        switch (override_proxy_connection_type) {
            case AWS_HPCT_HTTP_FORWARD:
                config->proxy_strategy = aws_http_proxy_strategy_new_forwarding_identity(allocator);
                break;

            case AWS_HPCT_HTTP_TUNNEL:
                config->proxy_strategy = aws_http_proxy_strategy_new_tunneling_one_time_identity(allocator);
                break;

            default:
                break;
        }

        if (config->proxy_strategy == NULL) {
            goto on_error;
        }
    }

    return config;

on_error:

    aws_http_proxy_config_destroy(config);

    return NULL;
}

struct aws_http_proxy_config *aws_http_proxy_config_new_from_connection_options(
    struct aws_allocator *allocator,
    const struct aws_http_client_connection_options *options) {
    AWS_FATAL_ASSERT(options != NULL);
    AWS_FATAL_ASSERT(options->proxy_options != NULL);

    return s_aws_http_proxy_config_new(
        allocator,
        options->proxy_options,
        s_determine_proxy_connection_type(options->proxy_options->connection_type, options->tls_options != NULL));
}

struct aws_http_proxy_config *aws_http_proxy_config_new_from_manager_options(
    struct aws_allocator *allocator,
    const struct aws_http_connection_manager_options *options) {
    AWS_FATAL_ASSERT(options != NULL);
    AWS_FATAL_ASSERT(options->proxy_options != NULL);

    return s_aws_http_proxy_config_new(
        allocator,
        options->proxy_options,
        s_determine_proxy_connection_type(
            options->proxy_options->connection_type, options->tls_connection_options != NULL));
}

struct aws_http_proxy_config *aws_http_proxy_config_new_tunneling_from_proxy_options(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *proxy_options) {

    return s_aws_http_proxy_config_new(allocator, proxy_options, AWS_HPCT_HTTP_TUNNEL);
}

struct aws_http_proxy_config *aws_http_proxy_config_new_from_proxy_options(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *proxy_options) {
    if (proxy_options->connection_type == AWS_HPCT_HTTP_LEGACY) {
        AWS_LOGF_ERROR(AWS_LS_HTTP_PROXY_NEGOTIATION, "LEGACY type is not supported to create proxy config");
        return NULL;
    }

    return s_aws_http_proxy_config_new(allocator, proxy_options, proxy_options->connection_type);
}

struct aws_http_proxy_config *aws_http_proxy_config_new_from_proxy_options_with_tls_info(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_options *proxy_options,
    bool is_tls_connection) {
    AWS_FATAL_ASSERT(proxy_options != NULL);

    return s_aws_http_proxy_config_new(
        allocator, proxy_options, s_determine_proxy_connection_type(proxy_options->connection_type, is_tls_connection));
}

struct aws_http_proxy_config *aws_http_proxy_config_new_clone(
    struct aws_allocator *allocator,
    const struct aws_http_proxy_config *proxy_config) {

    AWS_FATAL_ASSERT(proxy_config != NULL);

    struct aws_http_proxy_config *config = aws_mem_calloc(allocator, 1, sizeof(struct aws_http_proxy_config));
    if (config == NULL) {
        return NULL;
    }

    config->connection_type = proxy_config->connection_type;

    if (aws_byte_buf_init_copy_from_cursor(&config->host, allocator, aws_byte_cursor_from_buf(&proxy_config->host))) {
        goto on_error;
    }

    if (proxy_config->tls_options) {
        config->tls_options = aws_mem_calloc(allocator, 1, sizeof(struct aws_tls_connection_options));
        if (aws_tls_connection_options_copy(config->tls_options, proxy_config->tls_options)) {
            goto on_error;
        }
    }

    config->allocator = allocator;
    config->port = proxy_config->port;
    config->proxy_strategy = aws_http_proxy_strategy_acquire(proxy_config->proxy_strategy);

    return config;

on_error:

    aws_http_proxy_config_destroy(config);

    return NULL;
}

void aws_http_proxy_config_destroy(struct aws_http_proxy_config *config) {
    if (config == NULL) {
        return;
    }

    aws_byte_buf_clean_up(&config->host);

    if (config->tls_options) {
        aws_tls_connection_options_clean_up(config->tls_options);
        aws_mem_release(config->allocator, config->tls_options);
    }

    aws_http_proxy_strategy_release(config->proxy_strategy);

    aws_mem_release(config->allocator, config);
}

void aws_http_proxy_options_init_from_config(
    struct aws_http_proxy_options *options,
    const struct aws_http_proxy_config *config) {
    AWS_FATAL_ASSERT(options && config);

    options->connection_type = config->connection_type;
    options->host = aws_byte_cursor_from_buf(&config->host);
    options->port = config->port;
    options->tls_options = config->tls_options;
    options->proxy_strategy = config->proxy_strategy;
}

int aws_http_options_validate_proxy_configuration(const struct aws_http_client_connection_options *options) {
    if (options == NULL || options->proxy_options == NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    enum aws_http_proxy_connection_type proxy_type = options->proxy_options->connection_type;
    if (proxy_type == AWS_HPCT_HTTP_FORWARD && options->tls_options != NULL) {
        return aws_raise_error(AWS_ERROR_INVALID_STATE);
    }

    struct aws_http_proxy_strategy *proxy_strategy = options->proxy_options->proxy_strategy;
    if (proxy_strategy != NULL) {
        if (proxy_strategy->proxy_connection_type != proxy_type) {
            return aws_raise_error(AWS_ERROR_INVALID_STATE);
        }
    }

    return AWS_OP_SUCCESS;
}

struct aws_proxied_socket_channel_user_data {
    struct aws_allocator *allocator;
    struct aws_client_bootstrap *bootstrap;
    struct aws_channel *channel;
    aws_client_bootstrap_on_channel_event_fn *original_setup_callback;
    aws_client_bootstrap_on_channel_event_fn *original_shutdown_callback;
    void *original_user_data;
};

static void s_proxied_socket_channel_user_data_destroy(struct aws_proxied_socket_channel_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }

    aws_client_bootstrap_release(user_data->bootstrap);

    aws_mem_release(user_data->allocator, user_data);
}

static struct aws_proxied_socket_channel_user_data *s_proxied_socket_channel_user_data_new(
    struct aws_allocator *allocator,
    struct aws_socket_channel_bootstrap_options *channel_options) {
    struct aws_proxied_socket_channel_user_data *user_data =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_proxied_socket_channel_user_data));
    if (user_data == NULL) {
        return NULL;
    }

    user_data->allocator = allocator;
    user_data->original_setup_callback = channel_options->setup_callback;
    user_data->original_shutdown_callback = channel_options->shutdown_callback;
    user_data->original_user_data = channel_options->user_data;
    user_data->bootstrap = aws_client_bootstrap_acquire(channel_options->bootstrap);

    return user_data;
}

static void s_http_proxied_socket_channel_setup(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {

    (void)bootstrap;
    struct aws_proxied_socket_channel_user_data *proxied_user_data = user_data;

    if (error_code != AWS_ERROR_SUCCESS || channel == NULL) {
        proxied_user_data->original_setup_callback(
            proxied_user_data->bootstrap, error_code, NULL, proxied_user_data->original_user_data);
        s_proxied_socket_channel_user_data_destroy(proxied_user_data);
        return;
    }

    proxied_user_data->channel = channel;

    proxied_user_data->original_setup_callback(
        proxied_user_data->bootstrap,
        AWS_ERROR_SUCCESS,
        proxied_user_data->channel,
        proxied_user_data->original_user_data);
}

static void s_http_proxied_socket_channel_shutdown(
    struct aws_client_bootstrap *bootstrap,
    int error_code,
    struct aws_channel *channel,
    void *user_data) {
    (void)bootstrap;
    (void)channel;
    struct aws_proxied_socket_channel_user_data *proxied_user_data = user_data;
    proxied_user_data->original_shutdown_callback(
        proxied_user_data->bootstrap, error_code, proxied_user_data->channel, proxied_user_data->original_user_data);

    s_proxied_socket_channel_user_data_destroy(proxied_user_data);
}

int aws_http_proxy_new_socket_channel(
    struct aws_socket_channel_bootstrap_options *channel_options,
    const struct aws_http_proxy_options *proxy_options) {

    AWS_FATAL_ASSERT(channel_options != NULL && channel_options->bootstrap != NULL);
    AWS_FATAL_ASSERT(proxy_options != NULL);

    if (proxy_options->connection_type != AWS_HPCT_HTTP_TUNNEL) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_PROXY_NEGOTIATION,
            "Creating a raw protocol channel through an http proxy requires a tunneling proxy "
            "configuration");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (channel_options->tls_options == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_HTTP_PROXY_NEGOTIATION,
            "Creating a raw protocol channel through an http proxy requires tls to the endpoint");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_allocator *allocator = channel_options->bootstrap->allocator;
    struct aws_proxied_socket_channel_user_data *user_data =
        s_proxied_socket_channel_user_data_new(allocator, channel_options);

    struct aws_http_client_connection_options http_connection_options = AWS_HTTP_CLIENT_CONNECTION_OPTIONS_INIT;
    http_connection_options.allocator = allocator;
    http_connection_options.bootstrap = channel_options->bootstrap;
    http_connection_options.host_name = aws_byte_cursor_from_c_str(channel_options->host_name);
    http_connection_options.port = channel_options->port;
    http_connection_options.socket_options = channel_options->socket_options;
    http_connection_options.tls_options = channel_options->tls_options;
    http_connection_options.proxy_options = proxy_options;
    http_connection_options.user_data = user_data;
    http_connection_options.on_setup = NULL;    /* use channel callbacks, not http callbacks */
    http_connection_options.on_shutdown = NULL; /* use channel callbacks, not http callbacks */
    http_connection_options.requested_event_loop = channel_options->requested_event_loop;
    http_connection_options.host_resolution_config = channel_options->host_resolution_override_config;

    if (s_aws_http_client_connect_via_tunneling_proxy(
            &http_connection_options, s_http_proxied_socket_channel_setup, s_http_proxied_socket_channel_shutdown)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    s_proxied_socket_channel_user_data_destroy(user_data);

    return AWS_OP_ERR;
}
