/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/aws_imds_client.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/condition_variable.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/socket.h>
#include <ctype.h>

#include <aws/common/json.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#    pragma warning(disable : 4232)
#endif /* _MSC_VER */

/* instance role credentials body response is currently ~ 1300 characters + name length */
#define IMDS_RESPONSE_SIZE_INITIAL 2048
#define IMDS_RESPONSE_TOKEN_SIZE_INITIAL 64
#define IMDS_RESPONSE_SIZE_LIMIT 65535
#define IMDS_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 2
#define IMDS_DEFAULT_RETRIES 1

AWS_STATIC_STRING_FROM_LITERAL(s_imds_host, "169.254.169.254");

enum imds_token_state {
    AWS_IMDS_TS_INVALID,
    AWS_IMDS_TS_VALID,
    AWS_IMDS_TS_UPDATE_IN_PROGRESS,
};

enum imds_token_copy_result {
    /* Token is valid and copied to requester */
    AWS_IMDS_TCR_SUCCESS,
    /* Token is updating, so requester is added in waiting queue */
    AWS_IMDS_TCR_WAITING_IN_QUEUE,
    /* unexpected error,like mem allocation error */
    AWS_IMDS_TCR_UNEXPECTED_ERROR,
};

struct imds_token_query {
    struct aws_linked_list_node node;
    void *user_data;
};

struct aws_imds_client {
    struct aws_allocator *allocator;
    struct aws_http_connection_manager *connection_manager;
    struct aws_retry_strategy *retry_strategy;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_imds_client_shutdown_options shutdown_options;

    /* will be set to true by default, means using IMDS V2 */
    bool token_required;
    struct aws_byte_buf cached_token;
    uint64_t cached_token_expiration_timestamp;
    enum imds_token_state token_state;
    struct aws_linked_list pending_queries;
    struct aws_mutex token_lock;
    struct aws_condition_variable token_signal;
    bool ec2_metadata_v1_disabled;

    struct aws_atomic_var ref_count;
};

static void s_aws_imds_client_destroy(struct aws_imds_client *client) {
    if (!client) {
        return;
    }
    /**
     * s_aws_imds_client_destroy is only called after all in-flight requests are finished,
     * thus nothing is going to try and access retry_strategy again at this point.
     */
    aws_retry_strategy_release(client->retry_strategy);
    aws_condition_variable_clean_up(&client->token_signal);
    aws_mutex_clean_up(&client->token_lock);
    aws_byte_buf_clean_up(&client->cached_token);
    client->function_table->aws_http_connection_manager_release(client->connection_manager);
    /* freeing the client takes place in the shutdown callback below */
}

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_imds_client *client = user_data;

    if (client && client->shutdown_options.shutdown_callback) {
        client->shutdown_options.shutdown_callback(client->shutdown_options.shutdown_user_data);
    }

    aws_mem_release(client->allocator, client);
}

void aws_imds_client_release(struct aws_imds_client *client) {
    if (!client) {
        return;
    }

    size_t old_value = aws_atomic_fetch_sub(&client->ref_count, 1);
    if (old_value == 1) {
        s_aws_imds_client_destroy(client);
    }
}

void aws_imds_client_acquire(struct aws_imds_client *client) {
    aws_atomic_fetch_add(&client->ref_count, 1);
}

struct aws_imds_client *aws_imds_client_new(
    struct aws_allocator *allocator,
    const struct aws_imds_client_options *options) {

    if (!options->bootstrap) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Client bootstrap is required for querying IMDS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_imds_client *client = aws_mem_calloc(allocator, 1, sizeof(struct aws_imds_client));
    if (!client) {
        return NULL;
    }

    if (aws_mutex_init(&client->token_lock)) {
        goto on_error;
    }

    if (aws_condition_variable_init(&client->token_signal)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&client->cached_token, allocator, IMDS_RESPONSE_TOKEN_SIZE_INITIAL)) {
        goto on_error;
    }

    aws_linked_list_init(&client->pending_queries);

    aws_atomic_store_int(&client->ref_count, 1);
    client->allocator = allocator;
    client->function_table =
        options->function_table ? options->function_table : g_aws_credentials_provider_http_function_table;
    client->token_required = options->imds_version == IMDS_PROTOCOL_V1 ? false : true;
    client->ec2_metadata_v1_disabled = options->ec2_metadata_v1_disabled;
    client->shutdown_options = options->shutdown_options;

    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = (uint32_t)aws_timestamp_convert(
        IMDS_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = IMDS_RESPONSE_SIZE_LIMIT;
    manager_options.socket_options = &socket_options;
    manager_options.tls_connection_options = NULL;
    manager_options.host = aws_byte_cursor_from_string(s_imds_host);
    manager_options.port = 80;
    manager_options.max_connections = 10;
    manager_options.shutdown_complete_callback = s_on_connection_manager_shutdown;
    manager_options.shutdown_complete_user_data = client;

    client->connection_manager = client->function_table->aws_http_connection_manager_new(allocator, &manager_options);
    if (!client->connection_manager) {
        goto on_error;
    }

    if (options->retry_strategy) {
        client->retry_strategy = options->retry_strategy;
        aws_retry_strategy_acquire(client->retry_strategy);
    } else {
        struct aws_exponential_backoff_retry_options retry_options = {
            .el_group = options->bootstrap->event_loop_group,
            .max_retries = IMDS_DEFAULT_RETRIES,
        };
        /* exponential backoff is plenty here. We're hitting a local endpoint and do not run the risk of bringing
         * down more than the local VM. */
        client->retry_strategy = aws_retry_strategy_new_exponential_backoff(allocator, &retry_options);
    }
    if (!client->retry_strategy) {
        goto on_error;
    }

    return client;

on_error:
    s_aws_imds_client_destroy(client);
    return NULL;
}

/*
 * Tracking structure for each outstanding async query to an imds client
 */
struct imds_user_data {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_imds_client *client;
    aws_imds_client_on_get_resource_callback_fn *original_callback;
    void *original_user_data;

    /* mutable */
    struct aws_http_connection *connection;
    struct aws_http_message *request;
    struct aws_byte_buf current_result;
    struct aws_byte_buf imds_token;
    struct aws_string *resource_path;
    struct aws_retry_token *retry_token;
    /*
     * initial value is copy of client->token_required,
     * will be adapted according to response.
     */
    bool imds_token_required;
    /* Indicate the request is a fallback from a failure call. */
    bool is_fallback_request;
    bool is_imds_token_request;
    bool ec2_metadata_v1_disabled;
    int status_code;
    int error_code;

    struct aws_atomic_var ref_count;
};

static void s_user_data_destroy(struct imds_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }
    struct aws_imds_client *client = user_data->client;

    if (user_data->connection) {
        client->function_table->aws_http_connection_manager_release_connection(
            client->connection_manager, user_data->connection);
    }

    aws_byte_buf_clean_up(&user_data->current_result);
    aws_byte_buf_clean_up(&user_data->imds_token);
    aws_string_destroy(user_data->resource_path);

    if (user_data->request) {
        aws_http_message_destroy(user_data->request);
    }
    aws_retry_token_release(user_data->retry_token);
    aws_imds_client_release(client);
    aws_mem_release(user_data->allocator, user_data);
}

static struct imds_user_data *s_user_data_new(
    struct aws_imds_client *client,
    struct aws_byte_cursor resource_path,
    aws_imds_client_on_get_resource_callback_fn *callback,
    void *user_data) {

    struct imds_user_data *wrapped_user_data = aws_mem_calloc(client->allocator, 1, sizeof(struct imds_user_data));
    if (!wrapped_user_data) {
        goto on_error;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->client = client;
    aws_imds_client_acquire(client);
    wrapped_user_data->original_user_data = user_data;
    wrapped_user_data->original_callback = callback;

    if (aws_byte_buf_init(&wrapped_user_data->current_result, client->allocator, IMDS_RESPONSE_SIZE_INITIAL)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&wrapped_user_data->imds_token, client->allocator, IMDS_RESPONSE_TOKEN_SIZE_INITIAL)) {
        goto on_error;
    }

    wrapped_user_data->resource_path =
        aws_string_new_from_array(client->allocator, resource_path.ptr, resource_path.len);
    if (!wrapped_user_data->resource_path) {
        goto on_error;
    }

    wrapped_user_data->imds_token_required = client->token_required;
    wrapped_user_data->ec2_metadata_v1_disabled = client->ec2_metadata_v1_disabled;
    aws_atomic_store_int(&wrapped_user_data->ref_count, 1);
    return wrapped_user_data;

on_error:
    s_user_data_destroy(wrapped_user_data);
    return NULL;
}

static void s_user_data_acquire(struct imds_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }
    aws_atomic_fetch_add(&user_data->ref_count, 1);
}

static void s_user_data_release(struct imds_user_data *user_data) {
    if (!user_data) {
        return;
    }
    size_t old_value = aws_atomic_fetch_sub(&user_data->ref_count, 1);
    if (old_value == 1) {
        s_user_data_destroy(user_data);
    }
}

static void s_reset_scratch_user_data(struct imds_user_data *user_data) {
    user_data->current_result.len = 0;
    user_data->status_code = 0;

    if (user_data->request) {
        aws_http_message_destroy(user_data->request);
        user_data->request = NULL;
    }
}

static enum imds_token_copy_result s_copy_token_safely(struct imds_user_data *user_data);
static void s_update_token_safely(
    struct aws_imds_client *client,
    struct aws_byte_buf *token,
    bool token_required,
    uint64_t expire_timestamp);
static void s_query_complete(struct imds_user_data *user_data);
static void s_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data);
static void s_on_retry_token_acquired(struct aws_retry_strategy *, int, struct aws_retry_token *, void *);

static int s_on_incoming_body_fn(struct aws_http_stream *stream, const struct aws_byte_cursor *data, void *user_data) {
    (void)stream;
    (void)data;

    struct imds_user_data *imds_user_data = user_data;
    struct aws_imds_client *client = imds_user_data->client;

    if (data->len + imds_user_data->current_result.len > IMDS_RESPONSE_SIZE_LIMIT) {
        client->function_table->aws_http_connection_close(imds_user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client query response exceeded maximum allowed length", (void *)client);

        return aws_raise_error(AWS_AUTH_IMDS_CLIENT_SOURCE_FAILURE);
    }

    if (aws_byte_buf_append_dynamic(&imds_user_data->current_result, data)) {
        client->function_table->aws_http_connection_close(imds_user_data->connection);
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client query error appending response", (void *)client);

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_on_incoming_headers_fn(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *user_data) {

    (void)header_array;
    (void)num_headers;

    if (header_block != AWS_HTTP_HEADER_BLOCK_MAIN) {
        return AWS_OP_SUCCESS;
    }

    struct imds_user_data *imds_user_data = user_data;
    struct aws_imds_client *client = imds_user_data->client;
    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        if (imds_user_data->status_code == 0) {
            if (client->function_table->aws_http_stream_get_incoming_response_status(
                    stream, &imds_user_data->status_code)) {
                AWS_LOGF_ERROR(
                    AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client failed to get http status code", (void *)client);
                return AWS_OP_ERR;
            }
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client query received http status code %d for requester %p.",
                (void *)client,
                imds_user_data->status_code,
                user_data);
        }
    }

    return AWS_OP_SUCCESS;
}

AWS_STATIC_STRING_FROM_LITERAL(s_imds_host_header, "Host");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_accept_header, "Accept");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_accept_header_value, "*/*");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_user_agent_header, "User-Agent");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_user_agent_header_value, "aws-sdk-crt/aws-imds-client");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_h1_0_keep_alive_header, "Connection");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_h1_0_keep_alive_header_value, "keep-alive");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_token_resource_path, "/latest/api/token");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_token_ttl_header, "x-aws-ec2-metadata-token-ttl-seconds");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_token_header, "x-aws-ec2-metadata-token");
AWS_STATIC_STRING_FROM_LITERAL(s_imds_token_ttl_default_value, "21600");
/* s_imds_token_ttl_default_value - 5secs for refreshing the cached token */
static const uint64_t s_imds_token_ttl_secs = 21595;

static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data);

static int s_make_imds_http_query(
    struct imds_user_data *user_data,
    const struct aws_byte_cursor *verb,
    const struct aws_byte_cursor *uri,
    const struct aws_http_header *headers,
    size_t header_count) {

    AWS_FATAL_ASSERT(user_data->connection);
    struct aws_imds_client *client = user_data->client;
    struct aws_http_stream *stream = NULL;
    struct aws_http_message *request = aws_http_message_new_request(user_data->allocator);

    if (request == NULL) {
        return AWS_OP_ERR;
    }

    if (headers && aws_http_message_add_header_array(request, headers, header_count)) {
        goto on_error;
    }

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_string(s_imds_host_header),
        .value = aws_byte_cursor_from_string(s_imds_host),
    };
    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    struct aws_http_header accept_header = {
        .name = aws_byte_cursor_from_string(s_imds_accept_header),
        .value = aws_byte_cursor_from_string(s_imds_accept_header_value),
    };
    if (aws_http_message_add_header(request, accept_header)) {
        goto on_error;
    }

    struct aws_http_header user_agent_header = {
        .name = aws_byte_cursor_from_string(s_imds_user_agent_header),
        .value = aws_byte_cursor_from_string(s_imds_user_agent_header_value),
    };
    if (aws_http_message_add_header(request, user_agent_header)) {
        goto on_error;
    }

    struct aws_http_header keep_alive_header = {
        .name = aws_byte_cursor_from_string(s_imds_h1_0_keep_alive_header),
        .value = aws_byte_cursor_from_string(s_imds_h1_0_keep_alive_header_value),
    };
    if (aws_http_message_add_header(request, keep_alive_header)) {
        goto on_error;
    }

    if (aws_http_message_set_request_method(request, *verb)) {
        goto on_error;
    }

    if (aws_http_message_set_request_path(request, *uri)) {
        goto on_error;
    }

    user_data->request = request;

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .on_response_headers = s_on_incoming_headers_fn,
        .on_response_header_block_done = NULL,
        .on_response_body = s_on_incoming_body_fn,
        .on_complete = s_on_stream_complete_fn,
        .response_first_byte_timeout_ms = 1000,
        .user_data = user_data,
        .request = request,
    };

    /* for test with mocking http stack where make request finishes
    immediately and releases client before stream activate call */
    s_user_data_acquire(user_data);
    stream = client->function_table->aws_http_connection_make_request(user_data->connection, &request_options);
    if (!stream || client->function_table->aws_http_stream_activate(stream)) {
        goto on_error;
    }
    s_user_data_release(user_data);

    return AWS_OP_SUCCESS;

on_error:
    user_data->client->function_table->aws_http_stream_release(stream);
    aws_http_message_destroy(request);
    user_data->request = NULL;
    s_user_data_release(user_data);
    return AWS_OP_ERR;
}

/*
 * Process the http response from the token put request.
 */
static void s_client_on_token_response(struct imds_user_data *user_data) {
    /* Gets 400 means token is required but the request itself failed. */
    if (user_data->status_code == AWS_HTTP_STATUS_CODE_400_BAD_REQUEST) {
        s_update_token_safely(user_data->client, NULL, true, 0 /*expire_timestamp*/);
        return;
    }

    if (user_data->status_code == AWS_HTTP_STATUS_CODE_200_OK && user_data->current_result.len != 0) {
        AWS_LOGF_DEBUG(AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client has fetched the token", (void *)user_data->client);

        struct aws_byte_cursor cursor = aws_byte_cursor_from_buf(&(user_data->current_result));
        aws_byte_cursor_trim_pred(&cursor, aws_char_is_space);
        aws_byte_buf_reset(&user_data->imds_token, true /*zero contents*/);
        if (aws_byte_buf_append_and_update(&user_data->imds_token, &cursor)) {
            s_update_token_safely(user_data->client, NULL /*token*/, true /*token_required*/, 0 /*expire_timestamp*/);
            return;
        }
        /* The token was ALWAYS last for 6 hours, 21600 secs. Use current timestamp plus 21595 secs as the expiration
         * timestamp for current token */
        uint64_t current = 0;
        user_data->client->function_table->aws_high_res_clock_get_ticks(&current);
        uint64_t expire_timestamp = aws_add_u64_saturating(
            current, aws_timestamp_convert(s_imds_token_ttl_secs, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_NANOS, NULL));

        AWS_ASSERT(cursor.len != 0);
        s_update_token_safely(user_data->client, &user_data->imds_token, true /*token_required*/, expire_timestamp);
    } else if (user_data->ec2_metadata_v1_disabled) {
        AWS_LOGF_DEBUG(
            AWS_LS_IMDS_CLIENT,
            "(id=%p) IMDS client failed to fetch token for requester %p, and fall back to v1 is disabled."
            "Received response status code: %d",
            (void *)user_data->client,
            (void *)user_data,
            user_data->status_code);
        s_update_token_safely(user_data->client, NULL /*token*/, true /*token_required*/, 0 /*expire_timestamp*/);
    } else {
        /* Request failed; falling back to insecure request.
         * TODO: The retryable error (503 throttle) will also fall back to v1. Instead, we should just resend the token
         * request.
         */
        AWS_LOGF_DEBUG(
            AWS_LS_IMDS_CLIENT,
            "(id=%p) IMDS client failed to fetch token for requester %p, fall back to v1 for the same "
            "requester. Received response status code: %d",
            (void *)user_data->client,
            (void *)user_data,
            user_data->status_code);
        s_update_token_safely(user_data->client, NULL /*token*/, false /* token_required*/, 0 /*expire_timestamp*/);
    }
}

static int s_client_start_query_token(struct aws_imds_client *client) {
    struct imds_user_data *user_data = s_user_data_new(client, aws_byte_cursor_from_c_str(""), NULL, (void *)client);
    if (!user_data) {
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT,
            "(id=%p) IMDS client failed to query token with error: %s.",
            (void *)client,
            aws_error_str(aws_last_error()));
        return AWS_OP_ERR;
    }
    user_data->is_imds_token_request = true;
    if (aws_retry_strategy_acquire_retry_token(
            client->retry_strategy, NULL, s_on_retry_token_acquired, user_data, 100)) {
        s_user_data_release(user_data);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/* Make an http request to put a ttl and hopefully get a token back. */
static void s_client_do_query_token(struct imds_user_data *user_data) {
    /* start query token for imds client */
    struct aws_byte_cursor uri = aws_byte_cursor_from_string(s_imds_token_resource_path);

    /* Hard-coded 6 hour TTL for the token. */
    struct aws_http_header token_ttl_header = {
        .name = aws_byte_cursor_from_string(s_imds_token_ttl_header),
        .value = aws_byte_cursor_from_string(s_imds_token_ttl_default_value),
    };

    struct aws_http_header headers[] = {
        token_ttl_header,
    };

    struct aws_byte_cursor verb = aws_byte_cursor_from_c_str("PUT");

    if (s_make_imds_http_query(user_data, &verb, &uri, headers, AWS_ARRAY_SIZE(headers))) {
        user_data->error_code = aws_last_error();
        if (user_data->error_code == AWS_ERROR_SUCCESS) {
            user_data->error_code = AWS_ERROR_UNKNOWN;
        }
        s_query_complete(user_data);
    }
}

/*
 * Make the http request to fetch the resource
 */
static void s_do_query_resource(struct imds_user_data *user_data) {

    struct aws_http_header token_header = {
        .name = aws_byte_cursor_from_string(s_imds_token_header),
        .value = aws_byte_cursor_from_buf(&user_data->imds_token),
    };

    struct aws_http_header headers[] = {
        token_header,
    };

    size_t headers_count = 0;
    struct aws_http_header *headers_array_ptr = NULL;

    if (user_data->imds_token_required) {
        headers_count = 1;
        headers_array_ptr = headers;
    }

    struct aws_byte_cursor verb = aws_byte_cursor_from_c_str("GET");

    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_string(user_data->resource_path);
    if (s_make_imds_http_query(user_data, &verb, &path_cursor, headers_array_ptr, headers_count)) {
        user_data->error_code = aws_last_error();
        if (user_data->error_code == AWS_ERROR_SUCCESS) {
            user_data->error_code = AWS_ERROR_UNKNOWN;
        }
        s_query_complete(user_data);
    }
}

int s_get_resource_async_with_imds_token(struct imds_user_data *user_data);

static void s_query_complete(struct imds_user_data *user_data) {
    if (user_data->is_imds_token_request) {
        s_client_on_token_response(user_data);
        s_user_data_release(user_data);
        return;
    }

    if (user_data->status_code == AWS_HTTP_STATUS_CODE_401_UNAUTHORIZED) {
        struct aws_imds_client *client = user_data->client;
        aws_mutex_lock(&client->token_lock);
        if (aws_byte_buf_eq(&user_data->imds_token, &client->cached_token)) {
            /* If the token used matches the cached token, that means the cached token is invalid. */
            client->token_state = AWS_IMDS_TS_INVALID;
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client's cached token is invalidated by requester %p.",
                (void *)client,
                (void *)user_data);
        }
        /* let following requests use token as it's required. */
        client->token_required = true;
        aws_mutex_unlock(&client->token_lock);

        if (!user_data->imds_token_required && !user_data->is_fallback_request) {
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client failed to fetch resource via V1, try to use V2. requester %p.",
                (void *)user_data->client,
                (void *)user_data);
            /* V1 request, fallback to V2 and try again. */
            s_reset_scratch_user_data(user_data);
            user_data->is_fallback_request = true;
            aws_retry_token_release(user_data->retry_token);
            /* Try V2 now. */
            if (s_get_resource_async_with_imds_token(user_data)) {
                s_user_data_release(user_data);
            }
            return;
        } else {
            /* Not retirable error. */
            AWS_LOGF_ERROR(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client failed to fetch resource. Server response 401 UNAUTHORIZED. requester %p.",
                (void *)user_data->client,
                (void *)user_data);
            user_data->error_code = AWS_AUTH_IMDS_CLIENT_SOURCE_FAILURE;
        }
    }

    /* TODO: if server sent out error, we will still report as succeed with the error body received from server. */
    /* TODO: retry for 503 throttle. */
    user_data->original_callback(
        user_data->error_code ? NULL : &user_data->current_result,
        user_data->error_code,
        user_data->original_user_data);

    s_user_data_release(user_data);
}

static void s_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct imds_user_data *imds_user_data = user_data;
    imds_user_data->connection = connection;

    if (!connection) {
        AWS_LOGF_WARN(
            AWS_LS_IMDS_CLIENT,
            "id=%p: IMDS Client failed to acquire a connection, error code %d(%s)",
            (void *)imds_user_data->client,
            error_code,
            aws_error_str(error_code));
        imds_user_data->error_code = error_code;
        s_query_complete(imds_user_data);
        return;
    }

    if (imds_user_data->is_imds_token_request) {
        s_client_do_query_token(imds_user_data);
    } else {
        s_do_query_resource(imds_user_data);
    }
}

static void s_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    (void)token;

    struct imds_user_data *imds_user_data = user_data;
    struct aws_imds_client *client = imds_user_data->client;

    if (!error_code) {
        client->function_table->aws_http_connection_manager_acquire_connection(
            client->connection_manager, s_on_acquire_connection, user_data);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_IMDS_CLIENT,
            "id=%p: IMDS Client failed to retry the request with error code %d(%s)",
            (void *)client,
            error_code,
            aws_error_str(error_code));
        imds_user_data->error_code = error_code;
        s_query_complete(imds_user_data);
    }
}

static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct imds_user_data *imds_user_data = user_data;
    struct aws_imds_client *client = imds_user_data->client;

    aws_http_message_destroy(imds_user_data->request);
    imds_user_data->request = NULL;
    imds_user_data->connection = NULL;

    struct aws_http_connection *connection = client->function_table->aws_http_stream_get_connection(stream);
    client->function_table->aws_http_stream_release(stream);
    client->function_table->aws_http_connection_manager_release_connection(client->connection_manager, connection);

    /* on encountering error, see if we could try again */
    /* TODO: check the status code as well? */
    if (error_code) {
        AWS_LOGF_WARN(
            AWS_LS_IMDS_CLIENT,
            "id=%p: Stream completed with error code %d(%s)",
            (void *)client,
            error_code,
            aws_error_str(error_code));

        if (!aws_retry_strategy_schedule_retry(
                imds_user_data->retry_token, AWS_RETRY_ERROR_TYPE_TRANSIENT, s_on_retry_ready, user_data)) {
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "id=%p: Stream completed, retrying the last request on a new connection.",
                (void *)client);
            return;
        } else {
            AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "id=%p: Stream completed, retries have been exhausted.", (void *)client);
            imds_user_data->error_code = error_code;
        }
    } else if (aws_retry_token_record_success(imds_user_data->retry_token)) {
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT,
            "id=%p: Error while recording successful retry: %s",
            (void *)client,
            aws_error_str(aws_last_error()));
    }

    s_query_complete(imds_user_data);
}

static void s_on_retry_token_acquired(
    struct aws_retry_strategy *strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {
    (void)strategy;

    struct imds_user_data *imds_user_data = user_data;
    struct aws_imds_client *client = imds_user_data->client;

    if (!error_code) {
        AWS_LOGF_DEBUG(AWS_LS_IMDS_CLIENT, "id=%p: IMDS Client successfully acquired retry token.", (void *)client);
        imds_user_data->retry_token = token;
        client->function_table->aws_http_connection_manager_acquire_connection(
            client->connection_manager, s_on_acquire_connection, imds_user_data);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_IMDS_CLIENT,
            "id=%p: IMDS Client failed to acquire retry token, error code %d(%s)",
            (void *)client,
            error_code,
            aws_error_str(error_code));
        imds_user_data->error_code = error_code;
        s_query_complete(imds_user_data);
    }
}

static void s_complete_pending_queries(
    struct aws_imds_client *client,
    struct aws_linked_list *queries,
    bool token_required,
    struct aws_byte_buf *token) {

    /* poll swapped out pending queries if there is any */
    while (!aws_linked_list_empty(queries)) {
        struct aws_linked_list_node *node = aws_linked_list_pop_back(queries);
        struct imds_token_query *query = AWS_CONTAINER_OF(node, struct imds_token_query, node);
        struct imds_user_data *requester = query->user_data;
        aws_mem_release(client->allocator, query);

        bool should_continue = true;
        if (requester->imds_token_required && !token_required) {
            if (requester->is_fallback_request) {
                AWS_LOGF_ERROR(
                    AWS_LS_IMDS_CLIENT,
                    "(id=%p) IMDS client failed to fetch resource without token, and also failed to fetch token. "
                    "requester %p.",
                    (void *)requester->client,
                    (void *)requester);
                requester->error_code = AWS_AUTH_IMDS_CLIENT_SOURCE_FAILURE;
                should_continue = false;
            } else {
                AWS_LOGF_DEBUG(
                    AWS_LS_IMDS_CLIENT,
                    "(id=%p) IMDS client failed to fetch token, fallback to v1. requester %p.",
                    (void *)requester->client,
                    (void *)requester);
                requester->is_fallback_request = true;
            }
        }
        requester->imds_token_required = token_required;
        if (token) {
            aws_byte_buf_reset(&requester->imds_token, true);
            struct aws_byte_cursor cursor = aws_byte_cursor_from_buf(token);
            if (aws_byte_buf_append_dynamic(&requester->imds_token, &cursor)) {
                AWS_LOGF_ERROR(
                    AWS_LS_IMDS_CLIENT,
                    "(id=%p) IMDS client failed to copy IMDS token for requester %p.",
                    (void *)client,
                    (void *)requester);
                should_continue = false;
            }
        } else if (token_required) {
            requester->error_code = AWS_AUTH_IMDS_CLIENT_SOURCE_FAILURE;
            should_continue = false;
        }

        if (should_continue && aws_retry_strategy_acquire_retry_token(
                                   client->retry_strategy, NULL, s_on_retry_token_acquired, requester, 100)) {
            AWS_LOGF_ERROR(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client failed to allocate retry token for requester %p to send resource request.",
                (void *)client,
                (void *)requester);
            should_continue = false;
        }

        if (!should_continue) {
            if (requester->error_code == AWS_ERROR_SUCCESS) {
                requester->error_code = aws_last_error() == AWS_ERROR_SUCCESS ? AWS_ERROR_UNKNOWN : aws_last_error();
            }
            s_query_complete(requester);
        }
    }
}

static enum imds_token_copy_result s_copy_token_safely(struct imds_user_data *user_data) {
    struct aws_imds_client *client = user_data->client;
    enum imds_token_copy_result ret = AWS_IMDS_TCR_UNEXPECTED_ERROR;

    struct aws_linked_list pending_queries;
    aws_linked_list_init(&pending_queries);
    uint64_t current = 0;
    user_data->client->function_table->aws_high_res_clock_get_ticks(&current);

    aws_mutex_lock(&client->token_lock);
    if (client->token_state == AWS_IMDS_TS_VALID) {
        if (current > client->cached_token_expiration_timestamp) {
            /* The cached token expired. Switch the state */
            client->token_state = AWS_IMDS_TS_INVALID;
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client's cached token expired. Fetching new token for requester %p.",
                (void *)client,
                (void *)user_data);
        } else {
            aws_byte_buf_reset(&user_data->imds_token, true);
            struct aws_byte_cursor cursor = aws_byte_cursor_from_buf(&client->cached_token);
            if (aws_byte_buf_append_dynamic(&user_data->imds_token, &cursor)) {
                ret = AWS_IMDS_TCR_UNEXPECTED_ERROR;
            } else {
                ret = AWS_IMDS_TCR_SUCCESS;
            }
        }
    }

    if (client->token_state != AWS_IMDS_TS_VALID) {
        ret = AWS_IMDS_TCR_WAITING_IN_QUEUE;
        struct imds_token_query *query = aws_mem_calloc(client->allocator, 1, sizeof(struct imds_token_query));
        query->user_data = user_data;
        aws_linked_list_push_back(&client->pending_queries, &query->node);

        if (client->token_state == AWS_IMDS_TS_INVALID) {
            if (s_client_start_query_token(client)) {
                ret = AWS_IMDS_TCR_UNEXPECTED_ERROR;
                aws_linked_list_swap_contents(&pending_queries, &client->pending_queries);
            } else {
                client->token_state = AWS_IMDS_TS_UPDATE_IN_PROGRESS;
            }
        }
    }
    aws_mutex_unlock(&client->token_lock);

    s_complete_pending_queries(client, &pending_queries, true, NULL);

    switch (ret) {
        case AWS_IMDS_TCR_SUCCESS:
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client copied token to requester %p successfully.",
                (void *)client,
                (void *)user_data);
            break;

        case AWS_IMDS_TCR_WAITING_IN_QUEUE:
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client's token is invalid and is now updating.", (void *)client);
            break;

        case AWS_IMDS_TCR_UNEXPECTED_ERROR:
            AWS_LOGF_DEBUG(
                AWS_LS_IMDS_CLIENT,
                "(id=%p) IMDS client encountered unexpected error when processing token query for requester %p, error: "
                "%s.",
                (void *)client,
                (void *)user_data,
                aws_error_str(aws_last_error()));
            break;
    }
    return ret;
}
/**
 * Once a requseter returns from token request, it should call this function to unblock all other
 * waiting requesters. When the token parameter is NULL, means the token request failed. Now we need
 * a new requester to acquire the token again.
 */
static void s_update_token_safely(
    struct aws_imds_client *client,
    struct aws_byte_buf *token,
    bool token_required,
    uint64_t expire_timestamp) {
    AWS_FATAL_ASSERT(client);
    bool updated = false;

    struct aws_linked_list pending_queries;
    aws_linked_list_init(&pending_queries);

    aws_mutex_lock(&client->token_lock);
    client->token_required = token_required;
    if (token) {
        aws_byte_buf_reset(&client->cached_token, true);
        struct aws_byte_cursor cursor = aws_byte_cursor_from_buf(token);
        if (aws_byte_buf_append_dynamic(&client->cached_token, &cursor) == AWS_OP_SUCCESS) {
            client->token_state = AWS_IMDS_TS_VALID;
            client->cached_token_expiration_timestamp = expire_timestamp;
            updated = true;
        }
    } else {
        client->token_state = AWS_IMDS_TS_INVALID;
    }
    aws_linked_list_swap_contents(&pending_queries, &client->pending_queries);
    aws_mutex_unlock(&client->token_lock);

    s_complete_pending_queries(client, &pending_queries, token_required, token);

    if (updated) {
        AWS_LOGF_DEBUG(
            AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client updated the cached token successfully.", (void *)client);
    } else {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "(id=%p) IMDS client failed to update the token from IMDS.", (void *)client);
    }
}

int s_get_resource_async_with_imds_token(struct imds_user_data *user_data) {
    enum imds_token_copy_result res = s_copy_token_safely(user_data);
    if (res == AWS_IMDS_TCR_UNEXPECTED_ERROR) {
        return AWS_OP_ERR;
    }

    if (res == AWS_IMDS_TCR_WAITING_IN_QUEUE) {
        return AWS_OP_SUCCESS;
    }

    if (aws_retry_strategy_acquire_retry_token(
            user_data->client->retry_strategy, NULL, s_on_retry_token_acquired, user_data, 100)) {
        return AWS_OP_ERR;
    }
    return AWS_OP_SUCCESS;
}

int aws_imds_client_get_resource_async(
    struct aws_imds_client *client,
    struct aws_byte_cursor resource_path,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {

    struct imds_user_data *wrapped_user_data = s_user_data_new(client, resource_path, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto error;
    }

    if (!wrapped_user_data->imds_token_required) {
        if (aws_retry_strategy_acquire_retry_token(
                client->retry_strategy, NULL, s_on_retry_token_acquired, wrapped_user_data, 100)) {
            goto error;
        }
    } else if (s_get_resource_async_with_imds_token(wrapped_user_data)) {
        goto error;
    }
    return AWS_OP_SUCCESS;

error:
    s_user_data_release(wrapped_user_data);

    return AWS_OP_ERR;
}

/**
 * Higher level API definitions to get specific IMDS info
 * Reference:
 * https://docs.aws.amazon.com/AWSEC2/latest/UserGuide/instancedata-data-categories.html
 * https://docs.aws.amazon.com/AWSJavaSDK/latest/javadoc/com/amazonaws/util/EC2MetadataUtils.html
 * https://github.com/aws/aws-sdk-java-v2/blob/25f640c3b4f2e339c93a7da1494ab3310e128248/core/regions/src/main/java/software/amazon/awssdk/regions/internal/util/EC2MetadataUtils.java
 * IMDS client only implements resource acquisition that needs one resource request.
 * Complicated resource like network interface information defined in Java V2 SDK is not implemented here.
 * To get a full map of network interface information, we need more than ten requests, but sometimes we only care about
 * one or two of them.
 */
static struct aws_byte_cursor s_instance_identity_document =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("instance-identity/document");
static struct aws_byte_cursor s_instance_identity_signature =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("instance-identity/signature");
static struct aws_byte_cursor s_ec2_metadata_root = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/latest/meta-data");
static struct aws_byte_cursor s_ec2_credentials_root =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/latest/meta-data/iam/security-credentials/");
static struct aws_byte_cursor s_ec2_userdata_root = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/latest/user-data/");
static struct aws_byte_cursor s_ec2_dynamicdata_root = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/latest/dynamic/");

struct imds_get_array_user_data {
    struct aws_allocator *allocator;
    aws_imds_client_on_get_array_callback_fn *callback;
    void *user_data;
};

struct imds_get_credentials_user_data {
    struct aws_allocator *allocator;
    aws_imds_client_on_get_credentials_callback_fn *callback;
    void *user_data;
};

struct imds_get_iam_user_data {
    struct aws_allocator *allocator;
    aws_imds_client_on_get_iam_profile_callback_fn *callback;
    void *user_data;
};

struct imds_get_instance_user_data {
    struct aws_allocator *allocator;
    aws_imds_client_on_get_instance_info_callback_fn *callback;
    void *user_data;
};

static void s_process_array_resource(const struct aws_byte_buf *resource, int error_code, void *user_data) {
    struct imds_get_array_user_data *wrapped_user_data = user_data;
    struct aws_array_list resource_array;
    AWS_ZERO_STRUCT(resource_array);

    if (resource && !error_code) {
        struct aws_byte_cursor resource_cursor = aws_byte_cursor_from_buf(resource);
        if (aws_array_list_init_dynamic(
                &resource_array, wrapped_user_data->allocator, 10, sizeof(struct aws_byte_cursor))) {
            goto on_finish;
        }
        aws_byte_cursor_split_on_char(&resource_cursor, '\n', &resource_array);
    }

on_finish:
    wrapped_user_data->callback(&resource_array, error_code, wrapped_user_data->user_data);
    aws_array_list_clean_up_secure(&resource_array);
    aws_mem_release(wrapped_user_data->allocator, wrapped_user_data);
}

static void s_process_credentials_resource(const struct aws_byte_buf *resource, int error_code, void *user_data) {
    struct imds_get_credentials_user_data *wrapped_user_data = user_data;
    struct aws_credentials *credentials = NULL;

    struct aws_byte_buf json_data;
    AWS_ZERO_STRUCT(json_data);

    if (!resource || error_code) {
        goto on_finish;
    }

    if (aws_byte_buf_init_copy(&json_data, wrapped_user_data->allocator, resource)) {
        goto on_finish;
    }

    if (aws_byte_buf_append_null_terminator(&json_data)) {
        goto on_finish;
    }

    struct aws_parse_credentials_from_json_doc_options parse_options = {
        .access_key_id_name = "AccessKeyId",
        .secret_access_key_name = "SecretAccessKey",
        .token_name = "Token",
        .expiration_name = "Expiration",
        .token_required = true,
        .expiration_required = true,
    };

    credentials = aws_parse_credentials_from_json_document(
        wrapped_user_data->allocator, aws_byte_cursor_from_buf(&json_data), &parse_options);

on_finish:
    wrapped_user_data->callback(credentials, error_code, wrapped_user_data->user_data);
    aws_credentials_release(credentials);
    aws_byte_buf_clean_up_secure(&json_data);
    aws_mem_release(wrapped_user_data->allocator, wrapped_user_data);
}

/**
 * {
  "LastUpdated" : "2020-06-03T20:42:19Z",
  "InstanceProfileArn" : "arn:aws:iam::030535792909:instance-profile/CloudWatchAgentServerRole",
  "InstanceProfileId" : "AIPAQOHATHEGTGNQ5THQB"
}
 */
static int s_parse_iam_profile(struct aws_json_value *document_root, struct aws_imds_iam_profile *dest) {

    bool success = false;

    struct aws_byte_cursor last_updated_cursor;
    struct aws_json_value *last_updated =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("LastUpdated"));
    if (last_updated == NULL) {
        last_updated = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("lastupdated"));
    }
    if (!aws_json_value_is_string(last_updated) ||
        (aws_json_value_get_string(last_updated, &last_updated_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse LastUpdated from Json document for iam profile.");
        goto done;
    }

    struct aws_byte_cursor profile_arn_cursor;
    struct aws_json_value *profile_arn =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("InstanceProfileArn"));
    if (profile_arn == NULL) {
        profile_arn = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instanceprofilearn"));
    }
    if (!aws_json_value_is_string(profile_arn) ||
        (aws_json_value_get_string(profile_arn, &profile_arn_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse InstanceProfileArn from Json document for iam profile.");
        goto done;
    }

    struct aws_byte_cursor profile_id_cursor;
    struct aws_json_value *profile_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("InstanceProfileId"));
    if (profile_id == NULL) {
        profile_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instanceprofileid"));
    }
    if (!aws_json_value_is_string(profile_id) ||
        (aws_json_value_get_string(profile_id, &profile_id_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse InstanceProfileId from Json document for iam profile.");
        goto done;
    }

    if (last_updated_cursor.len == 0 || profile_arn_cursor.len == 0 || profile_id_cursor.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Parsed an unexpected Json document fro iam profile.");
        goto done;
    }

    if (aws_date_time_init_from_str_cursor(&dest->last_updated, &last_updated_cursor, AWS_DATE_FORMAT_ISO_8601)) {
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT, "LastUpdate in iam profile Json document is not a valid ISO_8601 date string.");
        goto done;
    }

    dest->instance_profile_arn = profile_arn_cursor;
    dest->instance_profile_id = profile_id_cursor;

    success = true;

done:
    return success ? AWS_OP_ERR : AWS_OP_SUCCESS;
}

static void s_process_iam_profile(const struct aws_byte_buf *resource, int error_code, void *user_data) {
    struct imds_get_iam_user_data *wrapped_user_data = user_data;
    struct aws_json_value *document_root = NULL;
    struct aws_imds_iam_profile iam;
    AWS_ZERO_STRUCT(iam);

    struct aws_byte_buf json_data;
    AWS_ZERO_STRUCT(json_data);

    if (!resource || error_code) {
        goto on_finish;
    }

    if (aws_byte_buf_init_copy(&json_data, wrapped_user_data->allocator, resource)) {
        goto on_finish;
    }

    if (aws_byte_buf_append_null_terminator(&json_data)) {
        goto on_finish;
    }

    struct aws_byte_cursor json_data_cursor = aws_byte_cursor_from_buf(&json_data);
    document_root = aws_json_value_new_from_string(aws_default_allocator(), json_data_cursor);
    if (document_root == NULL) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse document as Json document for iam profile.");
        goto on_finish;
    }

    if (s_parse_iam_profile(document_root, &iam)) {
        goto on_finish;
    }

on_finish:
    wrapped_user_data->callback(&iam, error_code, wrapped_user_data->user_data);
    aws_byte_buf_clean_up_secure(&json_data);
    aws_mem_release(wrapped_user_data->allocator, wrapped_user_data);
    if (document_root != NULL) {
        aws_json_value_destroy(document_root);
    }
}

/**
 * {
  "accountId" : "030535792909",
  "architecture" : "x86_64",
  "availabilityZone" : "us-west-2a",
  "billingProducts" : null, ------------>array
  "devpayProductCodes" : null, ----------->deprecated
  "marketplaceProductCodes" : null, -------->array
  "imageId" : "ami-5b70e323",
  "instanceId" : "i-022a93b5e640c0248",
  "instanceType" : "c4.8xlarge",
  "kernelId" : null,
  "pendingTime" : "2020-05-27T08:41:17Z",
  "privateIp" : "172.31.22.164",
  "ramdiskId" : null,
  "region" : "us-west-2",
  "version" : "2017-09-30"
  }
 */
static int s_parse_instance_info(struct aws_json_value *document_root, struct aws_imds_instance_info *dest) {

    bool success = false;

    struct aws_byte_cursor account_id_cursor;
    struct aws_json_value *account_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("accountId"));
    if (account_id == NULL) {
        account_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("accountid"));
        if (account_id == NULL) {
            account_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("AccountId"));
        }
    }
    if (!aws_json_value_is_string(account_id) ||
        (aws_json_value_get_string(account_id, &account_id_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse accountId from Json document for ec2 instance info.");
        goto done;
    }
    dest->account_id = account_id_cursor;

    struct aws_byte_cursor architecture_cursor;
    struct aws_json_value *architecture =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("architecture"));
    if (architecture == NULL) {
        architecture = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("Architecture"));
    }
    if (!aws_json_value_is_string(architecture) ||
        (aws_json_value_get_string(architecture, &architecture_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse architecture from Json document for ec2 instance info.");
        goto done;
    }
    dest->architecture = architecture_cursor;

    struct aws_byte_cursor availability_zone_cursor;
    struct aws_json_value *availability_zone =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("availabilityZone"));
    if (availability_zone == NULL) {
        availability_zone =
            aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("availabilityzone"));
        if (availability_zone == NULL) {
            availability_zone =
                aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("AvailabilityZone"));
        }
    }
    if (!aws_json_value_is_string(availability_zone) ||
        (aws_json_value_get_string(availability_zone, &availability_zone_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT, "Failed to parse availabilityZone from Json document for ec2 instance info.");
        goto done;
    }
    dest->availability_zone = availability_zone_cursor;

    struct aws_byte_cursor billing_products_cursor;
    struct aws_json_value *billing_products =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("billingProducts"));
    if (billing_products == NULL) {
        billing_products = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("billingproducts"));
        if (billing_products == NULL) {
            billing_products =
                aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("BillingProducts"));
        }
    }
    if (aws_json_value_is_array(billing_products)) {
        struct aws_json_value *element;
        for (size_t i = 0; i < aws_json_get_array_size(billing_products); i++) {
            element = aws_json_get_array_element(billing_products, i);
            if (aws_json_value_is_string(element) &&
                aws_json_value_get_string(element, &billing_products_cursor) != AWS_OP_ERR) {
                struct aws_byte_cursor item = billing_products_cursor;
                aws_array_list_push_back(&dest->billing_products, (const void *)&item);
            }
        }
    }

    struct aws_byte_cursor marketplace_product_codes_cursor;
    struct aws_json_value *marketplace_product_codes =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("marketplaceProductCodes"));
    if (marketplace_product_codes == NULL) {
        marketplace_product_codes =
            aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("marketplaceproductcodes"));
        if (marketplace_product_codes == NULL) {
            marketplace_product_codes =
                aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("MarketplaceProductCodes"));
        }
    }
    if (aws_json_value_is_array(marketplace_product_codes)) {
        struct aws_json_value *element;
        for (size_t i = 0; i < aws_json_get_array_size(marketplace_product_codes); i++) {
            element = aws_json_get_array_element(marketplace_product_codes, i);
            if (aws_json_value_is_string(element) &&
                aws_json_value_get_string(element, &marketplace_product_codes_cursor) != AWS_OP_ERR) {
                struct aws_byte_cursor item = marketplace_product_codes_cursor;
                aws_array_list_push_back(&dest->billing_products, (const void *)&item);
            }
        }
    }

    struct aws_byte_cursor image_id_cursor;
    struct aws_json_value *image_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("imageId"));
    if (image_id == NULL) {
        image_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("imageid"));
        if (image_id == NULL) {
            image_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("ImageId"));
        }
    }
    if (aws_json_value_is_string(image_id) && (aws_json_value_get_string(image_id, &image_id_cursor) != AWS_OP_ERR)) {
        dest->image_id = image_id_cursor;
    }

    struct aws_byte_cursor instance_id_cursor;
    struct aws_json_value *instance_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instanceId"));
    if (instance_id == NULL) {
        instance_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instanceid"));
        if (instance_id == NULL) {
            instance_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("InstanceId"));
        }
    }
    if (!aws_json_value_is_string(instance_id) ||
        (aws_json_value_get_string(instance_id, &instance_id_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse instanceId from Json document for ec2 instance info.");
        goto done;
    }
    dest->instance_id = instance_id_cursor;

    struct aws_byte_cursor instance_type_cursor;
    struct aws_json_value *instance_type =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instanceType"));
    if (instance_type == NULL) {
        instance_type = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("instancetype"));
        if (instance_type == NULL) {
            instance_type = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("InstanceType"));
        }
    }
    if (!aws_json_value_is_string(instance_type) ||
        (aws_json_value_get_string(instance_type, &instance_type_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse instanceType from Json document for ec2 instance info.");
        goto done;
    }
    dest->instance_type = instance_type_cursor;

    struct aws_byte_cursor kernel_id_cursor;
    struct aws_json_value *kernel_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("kernelId"));
    if (kernel_id == NULL) {
        kernel_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("kernelid"));
        if (kernel_id == NULL) {
            kernel_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("KernelId"));
        }
    }
    if (aws_json_value_is_string(kernel_id) &&
        (aws_json_value_get_string(kernel_id, &kernel_id_cursor) != AWS_OP_ERR)) {
        dest->kernel_id = kernel_id_cursor;
    }

    struct aws_byte_cursor private_ip_cursor;
    struct aws_json_value *private_ip =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("privateIp"));
    if (private_ip == NULL) {
        private_ip = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("privateip"));
        if (private_ip == NULL) {
            private_ip = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("PrivateIp"));
        }
    }
    if (aws_json_value_is_string(private_ip) &&
        (aws_json_value_get_string(private_ip, &private_ip_cursor) != AWS_OP_ERR)) {
        dest->private_ip = private_ip_cursor;
    }

    struct aws_byte_cursor ramdisk_id_cursor;
    struct aws_json_value *ramdisk_id =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("ramdiskId"));
    if (ramdisk_id == NULL) {
        ramdisk_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("ramdiskid"));
        if (ramdisk_id == NULL) {
            ramdisk_id = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("RamdiskId"));
        }
    }
    if (aws_json_value_is_string(ramdisk_id) &&
        (aws_json_value_get_string(ramdisk_id, &ramdisk_id_cursor) != AWS_OP_ERR)) {
        dest->ramdisk_id = ramdisk_id_cursor;
    }

    struct aws_byte_cursor region_cursor;
    struct aws_json_value *region = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("region"));
    if (region == NULL) {
        region = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("Region"));
    }
    if (!aws_json_value_is_string(region) || (aws_json_value_get_string(region, &region_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse region from Json document for ec2 instance info.");
        goto done;
    }
    dest->region = region_cursor;

    struct aws_byte_cursor version_cursor;
    struct aws_json_value *version =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("version"));
    if (version == NULL) {
        version = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("Version"));
    }
    if (!aws_json_value_is_string(version) || (aws_json_value_get_string(version, &version_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse version from Json document for ec2 instance info.");
        goto done;
    }
    dest->version = version_cursor;

    struct aws_byte_cursor pending_time_cursor;
    struct aws_json_value *pending_time =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("pendingTime"));
    if (pending_time == NULL) {
        pending_time = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("pendingtime"));
        if (pending_time == NULL) {
            pending_time = aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("PendingTime"));
        }
    }
    if (!aws_json_value_is_string(pending_time) ||
        (aws_json_value_get_string(pending_time, &pending_time_cursor) == AWS_OP_ERR)) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse pendingTime from Json document for ec2 instance info.");
        goto done;
    }

    if (aws_date_time_init_from_str_cursor(&dest->pending_time, &pending_time_cursor, AWS_DATE_FORMAT_ISO_8601)) {
        AWS_LOGF_ERROR(
            AWS_LS_IMDS_CLIENT, "pendingTime in instance info Json document is not a valid ISO_8601 date string.");
        goto done;
    }

    success = true;

done:
    return success ? AWS_OP_ERR : AWS_OP_SUCCESS;
}

static void s_process_instance_info(const struct aws_byte_buf *resource, int error_code, void *user_data) {
    struct imds_get_instance_user_data *wrapped_user_data = user_data;
    struct aws_imds_instance_info instance_info;
    AWS_ZERO_STRUCT(instance_info);
    struct aws_byte_buf json_data;
    AWS_ZERO_STRUCT(json_data);

    struct aws_json_value *document_root = NULL;

    if (aws_array_list_init_dynamic(
            &instance_info.billing_products, wrapped_user_data->allocator, 10, sizeof(struct aws_byte_cursor))) {
        goto on_finish;
    }

    if (aws_array_list_init_dynamic(
            &instance_info.marketplace_product_codes,
            wrapped_user_data->allocator,
            10,
            sizeof(struct aws_byte_cursor))) {
        goto on_finish;
    }

    if (!resource || error_code) {
        goto on_finish;
    }

    if (aws_byte_buf_init_copy(&json_data, wrapped_user_data->allocator, resource)) {
        goto on_finish;
    }

    if (aws_byte_buf_append_null_terminator(&json_data)) {
        goto on_finish;
    }

    struct aws_byte_cursor json_data_cursor = aws_byte_cursor_from_buf(&json_data);
    document_root = aws_json_value_new_from_string(aws_default_allocator(), json_data_cursor);
    if (document_root == NULL) {
        AWS_LOGF_ERROR(AWS_LS_IMDS_CLIENT, "Failed to parse document as Json document for ec2 instance info.");
        goto on_finish;
    }

    if (s_parse_instance_info(document_root, &instance_info)) {
        goto on_finish;
    }

on_finish:
    wrapped_user_data->callback(&instance_info, error_code, wrapped_user_data->user_data);
    aws_array_list_clean_up_secure(&instance_info.billing_products);
    aws_array_list_clean_up_secure(&instance_info.marketplace_product_codes);
    aws_byte_buf_clean_up_secure(&json_data);
    aws_mem_release(wrapped_user_data->allocator, wrapped_user_data);
    if (document_root != NULL) {
        aws_json_value_destroy(document_root);
    }
}

static int s_aws_imds_get_resource(
    struct aws_imds_client *client,
    struct aws_byte_cursor path,
    struct aws_byte_cursor name,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {

    struct aws_byte_buf resource;
    if (aws_byte_buf_init_copy_from_cursor(&resource, client->allocator, path)) {
        return AWS_OP_ERR;
    }
    if (aws_byte_buf_append_dynamic(&resource, &name)) {
        goto error;
    }
    if (aws_imds_client_get_resource_async(client, aws_byte_cursor_from_buf(&resource), callback, user_data)) {
        goto error;
    }
    aws_byte_buf_clean_up(&resource);
    return AWS_OP_SUCCESS;

error:
    aws_byte_buf_clean_up(&resource);
    return AWS_OP_ERR;
}

int s_aws_imds_get_converted_resource(
    struct aws_imds_client *client,
    struct aws_byte_cursor path,
    struct aws_byte_cursor name,
    aws_imds_client_on_get_resource_callback_fn conversion_fn,
    void *user_data) {
    return s_aws_imds_get_resource(client, path, name, conversion_fn, user_data);
}

int aws_imds_client_get_ami_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/ami-id"), callback, user_data);
}

int aws_imds_client_get_ami_launch_index(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/ami-launch-index"), callback, user_data);
}

int aws_imds_client_get_ami_manifest_path(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/ami-manifest-path"), callback, user_data);
}

int aws_imds_client_get_ancestor_ami_ids(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data) {

    struct imds_get_array_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_array_user_data));
    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;

    return s_aws_imds_get_converted_resource(
        client,
        s_ec2_metadata_root,
        aws_byte_cursor_from_c_str("/ancestor-ami-ids"),
        s_process_array_resource,
        wrapped_user_data);
}

int aws_imds_client_get_instance_action(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/instance-action"), callback, user_data);
}

int aws_imds_client_get_instance_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/instance-id"), callback, user_data);
}

int aws_imds_client_get_instance_type(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/instance-type"), callback, user_data);
}

int aws_imds_client_get_mac_address(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/mac"), callback, user_data);
}

int aws_imds_client_get_private_ip_address(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/local-ipv4"), callback, user_data);
}

int aws_imds_client_get_availability_zone(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/placement/availability-zone"), callback, user_data);
}

int aws_imds_client_get_product_codes(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/product-codes"), callback, user_data);
}

int aws_imds_client_get_public_key(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/public-keys/0/openssh-key"), callback, user_data);
}

int aws_imds_client_get_ramdisk_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/ramdisk-id"), callback, user_data);
}

int aws_imds_client_get_reservation_id(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/reservation-id"), callback, user_data);
}

int aws_imds_client_get_security_groups(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data) {

    struct imds_get_array_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_array_user_data));
    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;

    return s_aws_imds_get_converted_resource(
        client,
        s_ec2_metadata_root,
        aws_byte_cursor_from_c_str("/security-groups"),
        s_process_array_resource,
        wrapped_user_data);
}

int aws_imds_client_get_block_device_mapping(
    struct aws_imds_client *client,
    aws_imds_client_on_get_array_callback_fn callback,
    void *user_data) {

    struct imds_get_array_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_array_user_data));

    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;
    return s_aws_imds_get_converted_resource(
        client,
        s_ec2_metadata_root,
        aws_byte_cursor_from_c_str("/block-device-mapping"),
        s_process_array_resource,
        wrapped_user_data);
}

int aws_imds_client_get_attached_iam_role(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/iam/security-credentials/"), callback, user_data);
}

int aws_imds_client_get_credentials(
    struct aws_imds_client *client,
    struct aws_byte_cursor iam_role_name,
    aws_imds_client_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct imds_get_credentials_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_credentials_user_data));
    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;

    return s_aws_imds_get_converted_resource(
        client, s_ec2_credentials_root, iam_role_name, s_process_credentials_resource, wrapped_user_data);
}

int aws_imds_client_get_iam_profile(
    struct aws_imds_client *client,
    aws_imds_client_on_get_iam_profile_callback_fn callback,
    void *user_data) {

    struct imds_get_iam_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_iam_user_data));
    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;

    return s_aws_imds_get_converted_resource(
        client, s_ec2_metadata_root, aws_byte_cursor_from_c_str("/iam/info"), s_process_iam_profile, wrapped_user_data);
}

int aws_imds_client_get_user_data(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(client, s_ec2_userdata_root, aws_byte_cursor_from_c_str(""), callback, user_data);
}

int aws_imds_client_get_instance_signature(
    struct aws_imds_client *client,
    aws_imds_client_on_get_resource_callback_fn callback,
    void *user_data) {
    return s_aws_imds_get_resource(client, s_ec2_dynamicdata_root, s_instance_identity_signature, callback, user_data);
}

int aws_imds_client_get_instance_info(
    struct aws_imds_client *client,
    aws_imds_client_on_get_instance_info_callback_fn callback,
    void *user_data) {

    struct imds_get_instance_user_data *wrapped_user_data =
        aws_mem_calloc(client->allocator, 1, sizeof(struct imds_get_instance_user_data));
    if (!wrapped_user_data) {
        return AWS_OP_ERR;
    }

    wrapped_user_data->allocator = client->allocator;
    wrapped_user_data->callback = callback;
    wrapped_user_data->user_data = user_data;

    return s_aws_imds_get_converted_resource(
        client, s_ec2_dynamicdata_root, s_instance_identity_document, s_process_instance_info, wrapped_user_data);
}
