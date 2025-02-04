/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/date_time.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#include <aws/common/json.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#    pragma warning(disable : 4232)
#endif /* _MSC_VER */

/* IoT Core credentials body response is currently ~ 1100 Bytes*/
#define X509_RESPONSE_SIZE_INITIAL 1024
#define X509_RESPONSE_SIZE_LIMIT 2048
#define X509_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 2

struct aws_credentials_provider_x509_impl {
    struct aws_http_connection_manager *connection_manager;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_byte_buf thing_name;
    struct aws_byte_buf role_alias_path;
    struct aws_byte_buf endpoint;
    struct aws_tls_connection_options tls_connection_options;
};

/*
 * Tracking structure for each outstanding async query to an x509 provider
 */
struct aws_credentials_provider_x509_user_data {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_credentials_provider *x509_provider;
    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;

    /* mutable */
    struct aws_http_connection *connection;
    struct aws_http_message *request;
    struct aws_byte_buf response;
    int status_code;
    int error_code;
};

static void s_aws_credentials_provider_x509_user_data_destroy(
    struct aws_credentials_provider_x509_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }

    struct aws_credentials_provider_x509_impl *impl = user_data->x509_provider->impl;

    if (user_data->connection) {
        impl->function_table->aws_http_connection_manager_release_connection(
            impl->connection_manager, user_data->connection);
    }

    aws_byte_buf_clean_up(&user_data->response);

    if (user_data->request) {
        aws_http_message_destroy(user_data->request);
    }
    aws_credentials_provider_release(user_data->x509_provider);
    aws_mem_release(user_data->allocator, user_data);
}

static struct aws_credentials_provider_x509_user_data *s_aws_credentials_provider_x509_user_data_new(
    struct aws_credentials_provider *x509_provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_x509_user_data *wrapped_user_data =
        aws_mem_calloc(x509_provider->allocator, 1, sizeof(struct aws_credentials_provider_x509_user_data));
    if (wrapped_user_data == NULL) {
        goto on_error;
    }

    wrapped_user_data->allocator = x509_provider->allocator;
    wrapped_user_data->x509_provider = x509_provider;
    aws_credentials_provider_acquire(x509_provider);
    wrapped_user_data->original_user_data = user_data;
    wrapped_user_data->original_callback = callback;

    if (aws_byte_buf_init(&wrapped_user_data->response, x509_provider->allocator, X509_RESPONSE_SIZE_INITIAL)) {
        goto on_error;
    }

    return wrapped_user_data;

on_error:

    s_aws_credentials_provider_x509_user_data_destroy(wrapped_user_data);

    return NULL;
}

static void s_aws_credentials_provider_x509_user_data_reset_response(
    struct aws_credentials_provider_x509_user_data *x509_user_data) {
    x509_user_data->response.len = 0;
    x509_user_data->status_code = 0;

    if (x509_user_data->request) {
        aws_http_message_destroy(x509_user_data->request);
        x509_user_data->request = NULL;
    }
}

/*
 * In general, the returned json document looks something like:
{
    "credentials": {
        "accessKeyId" : "...",
        "secretAccessKey" : "...",
        "sessionToken" : "...",
        "expiration" : "2019-05-29T00:21:43Z"
    }
}
 */
static struct aws_credentials *s_parse_credentials_from_iot_core_document(
    struct aws_allocator *allocator,
    struct aws_byte_buf *document) {

    struct aws_credentials *credentials = NULL;
    struct aws_json_value *document_root = NULL;

    if (aws_byte_buf_append_null_terminator(document)) {
        goto done;
    }

    struct aws_byte_cursor document_cursor = aws_byte_cursor_from_buf(document);
    document_root = aws_json_value_new_from_string(allocator, document_cursor);
    if (document_root == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to parse IoT Core response as Json document.");
        goto done;
    }

    /*
     * pull out the root "Credentials" components
     */
    struct aws_json_value *creds =
        aws_json_value_get_from_object(document_root, aws_byte_cursor_from_c_str("credentials"));
    if (!aws_json_value_is_object(creds)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to parse credentials from IoT Core response.");
        goto done;
    }

    struct aws_parse_credentials_from_json_doc_options parse_options = {
        .access_key_id_name = "accessKeyId",
        .secret_access_key_name = "secretAccessKey",
        .token_name = "sessionToken",
        .expiration_name = "expiration",
        .expiration_format = AWS_PCEF_STRING_ISO_8601_DATE,
        .token_required = true,
        .expiration_required = false,
    };

    credentials = aws_parse_credentials_from_aws_json_object(allocator, creds, &parse_options);
    if (!credentials) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "X509 credentials provider failed to parse credentials");
    }

done:

    if (document_root != NULL) {
        aws_json_value_destroy(document_root);
    }

    return credentials;
}

/*
 * No matter the result, this always gets called assuming that x509_user_data is successfully allocated
 */
static void s_x509_finalize_get_credentials_query(struct aws_credentials_provider_x509_user_data *x509_user_data) {
    /* Try to build credentials from whatever, if anything, was in the result */
    struct aws_credentials *credentials =
        s_parse_credentials_from_iot_core_document(x509_user_data->allocator, &x509_user_data->response);

    if (credentials != NULL) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) X509 credentials provider successfully queried credentials",
            (void *)x509_user_data->x509_provider);
    } else {
        if (x509_user_data->error_code == AWS_ERROR_SUCCESS) {
            x509_user_data->error_code = aws_last_error();
            if (x509_user_data->error_code == AWS_ERROR_SUCCESS) {
                x509_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_X509_SOURCE_FAILURE;
            }
        }

        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) X509 credentials provider failed to query credentials with error %d(%s)",
            (void *)x509_user_data->x509_provider,
            x509_user_data->error_code,
            aws_error_str(x509_user_data->error_code));
    }

    /* pass the credentials back */
    x509_user_data->original_callback(credentials, x509_user_data->error_code, x509_user_data->original_user_data);

    /* clean up */
    s_aws_credentials_provider_x509_user_data_destroy(x509_user_data);
    aws_credentials_release(credentials);
}

static int s_x509_on_incoming_body_fn(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    (void)stream;

    struct aws_credentials_provider_x509_user_data *x509_user_data = user_data;
    struct aws_credentials_provider_x509_impl *impl = x509_user_data->x509_provider->impl;

    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) X509 credentials provider received %zu response bytes",
        (void *)x509_user_data->x509_provider,
        data->len);

    if (data->len + x509_user_data->response.len > X509_RESPONSE_SIZE_LIMIT) {
        impl->function_table->aws_http_connection_close(x509_user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) X509 credentials provider query response exceeded maximum allowed length",
            (void *)x509_user_data->x509_provider);

        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (aws_byte_buf_append_dynamic(&x509_user_data->response, data)) {
        impl->function_table->aws_http_connection_close(x509_user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) X509 credentials provider query error appending response",
            (void *)x509_user_data->x509_provider);

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_x509_on_incoming_headers_fn(
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

    struct aws_credentials_provider_x509_user_data *x509_user_data = user_data;
    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        if (x509_user_data->status_code == 0) {
            struct aws_credentials_provider_x509_impl *impl = x509_user_data->x509_provider->impl;
            if (impl->function_table->aws_http_stream_get_incoming_response_status(
                    stream, &x509_user_data->status_code)) {

                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "(id=%p) X509 credentials provider failed to get http status code",
                    (void *)x509_user_data->x509_provider);

                return AWS_OP_ERR;
            }
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) X509 credentials provider query received http status code %d",
                (void *)x509_user_data->x509_provider,
                x509_user_data->status_code);
        }
    }

    return AWS_OP_SUCCESS;
}

static void s_x509_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct aws_credentials_provider_x509_user_data *x509_user_data = user_data;

    aws_http_message_destroy(x509_user_data->request);
    x509_user_data->request = NULL;

    struct aws_credentials_provider_x509_impl *impl = x509_user_data->x509_provider->impl;
    impl->function_table->aws_http_stream_release(stream);

    /*
     * On anything other than a 200, nullify the response and pretend there was
     * an error
     */
    if (x509_user_data->status_code != AWS_HTTP_STATUS_CODE_200_OK || error_code != AWS_OP_SUCCESS) {
        x509_user_data->response.len = 0;

        if (error_code != AWS_OP_SUCCESS) {
            x509_user_data->error_code = error_code;
        } else {
            x509_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE;
        }
    }

    s_x509_finalize_get_credentials_query(x509_user_data);
}

AWS_STATIC_STRING_FROM_LITERAL(s_x509_accept_header, "Accept");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_accept_header_value, "*/*");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_user_agent_header, "User-Agent");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_user_agent_header_value, "aws-sdk-crt/x509-credentials-provider");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_h1_0_keep_alive_header, "Connection");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_h1_0_keep_alive_header_value, "keep-alive");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_thing_name_header, "x-amzn-iot-thingname");
AWS_STATIC_STRING_FROM_LITERAL(s_x509_host_header, "Host");

static int s_make_x509_http_query(
    struct aws_credentials_provider_x509_user_data *x509_user_data,
    struct aws_byte_cursor *request_path) {
    AWS_FATAL_ASSERT(x509_user_data->connection);

    struct aws_http_stream *stream = NULL;
    struct aws_http_message *request = aws_http_message_new_request(x509_user_data->allocator);
    if (request == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_credentials_provider_x509_impl *impl = x509_user_data->x509_provider->impl;

    struct aws_http_header thing_name_header = {
        .name = aws_byte_cursor_from_string(s_x509_thing_name_header),
        .value = aws_byte_cursor_from_buf(&impl->thing_name),
    };
    if (aws_http_message_add_header(request, thing_name_header)) {
        goto on_error;
    }

    struct aws_http_header accept_header = {
        .name = aws_byte_cursor_from_string(s_x509_accept_header),
        .value = aws_byte_cursor_from_string(s_x509_accept_header_value),
    };
    if (aws_http_message_add_header(request, accept_header)) {
        goto on_error;
    }

    struct aws_http_header user_agent_header = {
        .name = aws_byte_cursor_from_string(s_x509_user_agent_header),
        .value = aws_byte_cursor_from_string(s_x509_user_agent_header_value),
    };
    if (aws_http_message_add_header(request, user_agent_header)) {
        goto on_error;
    }

    struct aws_http_header keep_alive_header = {
        .name = aws_byte_cursor_from_string(s_x509_h1_0_keep_alive_header),
        .value = aws_byte_cursor_from_string(s_x509_h1_0_keep_alive_header_value),
    };
    if (aws_http_message_add_header(request, keep_alive_header)) {
        goto on_error;
    }

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_string(s_x509_host_header),
        .value = aws_byte_cursor_from_buf(&impl->endpoint),
    };
    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    if (aws_http_message_set_request_path(request, *request_path)) {
        goto on_error;
    }

    if (aws_http_message_set_request_method(request, aws_byte_cursor_from_c_str("GET"))) {
        goto on_error;
    }

    x509_user_data->request = request;

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .on_response_headers = s_x509_on_incoming_headers_fn,
        .on_response_header_block_done = NULL,
        .on_response_body = s_x509_on_incoming_body_fn,
        .on_complete = s_x509_on_stream_complete_fn,
        .user_data = x509_user_data,
        .request = request,
    };

    stream = impl->function_table->aws_http_connection_make_request(x509_user_data->connection, &request_options);

    if (!stream) {
        goto on_error;
    }

    if (impl->function_table->aws_http_stream_activate(stream)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    impl->function_table->aws_http_stream_release(stream);
    aws_http_message_destroy(request);
    x509_user_data->request = NULL;
    return AWS_OP_ERR;
}

static void s_x509_query_credentials(struct aws_credentials_provider_x509_user_data *x509_user_data) {
    AWS_FATAL_ASSERT(x509_user_data->connection);

    struct aws_credentials_provider_x509_impl *impl = x509_user_data->x509_provider->impl;

    /* "Clear" the result */
    s_aws_credentials_provider_x509_user_data_reset_response(x509_user_data);

    struct aws_byte_cursor request_path_cursor = aws_byte_cursor_from_buf(&impl->role_alias_path);
    if (s_make_x509_http_query(x509_user_data, &request_path_cursor) == AWS_OP_ERR) {
        s_x509_finalize_get_credentials_query(x509_user_data);
    }
}

static void s_x509_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct aws_credentials_provider_x509_user_data *x509_user_data = user_data;

    if (connection == NULL) {
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: X509 provider failed to acquire a connection, error code %d(%s)",
            (void *)x509_user_data->x509_provider,
            error_code,
            aws_error_str(error_code));

        x509_user_data->error_code = error_code;

        s_x509_finalize_get_credentials_query(x509_user_data);
        return;
    }

    x509_user_data->connection = connection;

    s_x509_query_credentials(x509_user_data);
}

static int s_credentials_provider_x509_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_x509_impl *impl = provider->impl;

    struct aws_credentials_provider_x509_user_data *wrapped_user_data =
        s_aws_credentials_provider_x509_user_data_new(provider, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto error;
    }

    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_x509_on_acquire_connection, wrapped_user_data);

    return AWS_OP_SUCCESS;

error:

    s_aws_credentials_provider_x509_user_data_destroy(wrapped_user_data);

    return AWS_OP_ERR;
}

static void s_credentials_provider_x509_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_x509_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_byte_buf_clean_up(&impl->thing_name);
    aws_byte_buf_clean_up(&impl->role_alias_path);
    aws_byte_buf_clean_up(&impl->endpoint);
    aws_tls_connection_options_clean_up(&impl->tls_connection_options);
    /* aws_http_connection_manager_release will eventually leads to call of s_on_connection_manager_shutdown,
     * which will do memory release for provider and impl. So We should be freeing impl
     * related memory first, then call aws_http_connection_manager_release.
     */
    impl->function_table->aws_http_connection_manager_release(impl->connection_manager);

    /* freeing the provider takes place in the shutdown callback below */
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_x509_vtable = {
    .get_credentials = s_credentials_provider_x509_get_credentials_async,
    .destroy = s_credentials_provider_x509_destroy,
};

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;

    aws_credentials_provider_invoke_shutdown_callback(provider);

    aws_mem_release(provider->allocator, provider);
}

struct aws_credentials_provider *aws_credentials_provider_new_x509(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_x509_options *options) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_x509_impl *impl = NULL;

    if (options->tls_connection_options == NULL || options->thing_name.len == 0 || options->role_alias.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "To create an X.509 creds provider, a tls_connection_options, an IoT thing name and an IAM role alias are "
            "required.");
        goto on_error;
    }

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_x509_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_x509_vtable, impl);

    if (aws_tls_connection_options_copy(&impl->tls_connection_options, options->tls_connection_options)) {
        goto on_error;
    }

    struct aws_byte_cursor server_name = options->endpoint;
    if (aws_tls_connection_options_set_server_name(&impl->tls_connection_options, allocator, &(server_name))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to set tls connection options's server name with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = (uint32_t)aws_timestamp_convert(
        X509_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = X509_RESPONSE_SIZE_LIMIT;
    manager_options.socket_options = &socket_options;
    manager_options.host = options->endpoint;
    manager_options.port = 443;
    manager_options.max_connections = 2;
    manager_options.shutdown_complete_callback = s_on_connection_manager_shutdown;
    manager_options.shutdown_complete_user_data = provider;
    manager_options.tls_connection_options = &impl->tls_connection_options;
    manager_options.proxy_options = options->proxy_options;

    impl->function_table = options->function_table;
    if (impl->function_table == NULL) {
        impl->function_table = g_aws_credentials_provider_http_function_table;
    }

    impl->connection_manager = impl->function_table->aws_http_connection_manager_new(allocator, &manager_options);
    if (impl->connection_manager == NULL) {
        goto on_error;
    }

    if (aws_byte_buf_init_copy_from_cursor(&impl->thing_name, allocator, options->thing_name)) {
        goto on_error;
    }

    if (aws_byte_buf_init_copy_from_cursor(&impl->endpoint, allocator, options->endpoint)) {
        goto on_error;
    }

    /* the expected path is "/role-aliases/<your role alias>/credentials" */
    struct aws_byte_cursor prefix_cursor = aws_byte_cursor_from_c_str("/role-aliases/");
    if (aws_byte_buf_init_copy_from_cursor(&impl->role_alias_path, allocator, prefix_cursor)) {
        goto on_error;
    }

    if (aws_byte_buf_append_dynamic(&impl->role_alias_path, &options->role_alias)) {
        goto on_error;
    }

    struct aws_byte_cursor creds_cursor = aws_byte_cursor_from_c_str("/credentials");
    if (aws_byte_buf_append_dynamic(&impl->role_alias_path, &creds_cursor)) {
        goto on_error;
    }

    provider->shutdown_options = options->shutdown_options;

    return provider;

on_error:

    aws_credentials_provider_destroy(provider);

    return NULL;
}
