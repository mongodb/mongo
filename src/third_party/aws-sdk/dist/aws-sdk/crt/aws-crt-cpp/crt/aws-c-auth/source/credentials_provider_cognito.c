/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/json.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/retry_strategy.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>

#include <inttypes.h>

#define COGNITO_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 5
#define COGNITO_MAX_RETRIES 3
#define HTTP_REQUEST_BODY_INITIAL_SIZE 1024
#define HTTP_RESPONSE_BODY_INITIAL_SIZE 4096

static void s_on_connection_manager_shutdown(void *user_data);
static void s_on_connection_setup_fn(struct aws_http_connection *connection, int error_code, void *user_data);

struct aws_cognito_login {
    struct aws_byte_cursor identity_provider_name;
    struct aws_byte_cursor identity_provider_token;
    struct aws_byte_buf login_buffer;
};

static int s_aws_cognito_login_init(
    struct aws_cognito_login *login,
    struct aws_allocator *allocator,
    struct aws_byte_cursor identity_provider_name,
    struct aws_byte_cursor identity_provider_token) {
    AWS_ZERO_STRUCT(*login);

    login->identity_provider_name = identity_provider_name;
    login->identity_provider_token = identity_provider_token;

    return aws_byte_buf_init_cache_and_update_cursors(
        &login->login_buffer, allocator, &login->identity_provider_name, &login->identity_provider_token, NULL);
}

static void s_aws_cognito_login_clean_up(struct aws_cognito_login *login) {
    aws_byte_buf_clean_up(&login->login_buffer);

    AWS_ZERO_STRUCT(*login);
}

struct aws_credentials_provider_cognito_impl {
    struct aws_http_connection_manager *connection_manager;
    struct aws_retry_strategy *retry_strategy;
    const struct aws_auth_http_system_vtable *function_table;

    struct aws_string *endpoint;

    struct aws_string *identity;

    struct aws_array_list logins;

    struct aws_string *custom_role_arn;
};

struct cognito_user_data {
    struct aws_allocator *allocator;

    struct aws_credentials_provider *provider;

    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;

    struct aws_http_connection *connection;
    struct aws_http_message *get_credentials_request;
    struct aws_byte_buf request_body_buffer;
    struct aws_input_stream *request_body_stream;

    struct aws_retry_token *retry_token;
    struct aws_credentials *credentials;
    struct aws_byte_buf response_body;
};

static void s_user_data_reset(struct cognito_user_data *user_data) {
    aws_byte_buf_clean_up(&user_data->request_body_buffer);

    user_data->request_body_stream = aws_input_stream_release(user_data->request_body_stream);
    user_data->get_credentials_request = aws_http_message_release(user_data->get_credentials_request);

    struct aws_credentials_provider_cognito_impl *impl = user_data->provider->impl;
    if (user_data->connection != NULL) {
        impl->function_table->aws_http_connection_manager_release_connection(
            impl->connection_manager, user_data->connection);
        user_data->connection = NULL;
    }

    aws_byte_buf_reset(&user_data->response_body, false);
}

static void s_user_data_destroy(struct cognito_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }

    s_user_data_reset(user_data);

    aws_byte_buf_clean_up(&user_data->response_body);
    aws_retry_token_release(user_data->retry_token);
    aws_credentials_provider_release(user_data->provider);
    aws_credentials_release(user_data->credentials);

    aws_mem_release(user_data->allocator, user_data);
}

static struct cognito_user_data *s_user_data_new(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_allocator *allocator = provider->allocator;
    struct cognito_user_data *cognito_user_data = aws_mem_calloc(allocator, 1, sizeof(struct cognito_user_data));
    cognito_user_data->allocator = allocator;

    aws_byte_buf_init(&cognito_user_data->response_body, cognito_user_data->allocator, HTTP_RESPONSE_BODY_INITIAL_SIZE);

    cognito_user_data->provider = aws_credentials_provider_acquire(provider);
    cognito_user_data->original_callback = callback;
    cognito_user_data->original_user_data = user_data;

    return cognito_user_data;
}

static void s_finalize_credentials_query(struct cognito_user_data *user_data, int error_code) {
    AWS_FATAL_ASSERT(user_data != NULL);

    if (user_data->credentials == NULL && error_code == AWS_ERROR_SUCCESS) {
        error_code = AWS_AUTH_CREDENTIALS_PROVIDER_COGNITO_SOURCE_FAILURE;
    }

    (user_data->original_callback)(user_data->credentials, error_code, user_data->original_user_data);

    s_user_data_destroy(user_data);
}

/* Keys per Cognito-Identity service model */
AWS_STATIC_STRING_FROM_LITERAL(s_credentials_key, "Credentials");
AWS_STATIC_STRING_FROM_LITERAL(s_access_key_id_name, "AccessKeyId");
AWS_STATIC_STRING_FROM_LITERAL(s_secret_access_key_name, "SecretKey");
AWS_STATIC_STRING_FROM_LITERAL(s_session_token_name, "SessionToken");
AWS_STATIC_STRING_FROM_LITERAL(s_expiration_name, "Expiration");

static int s_parse_credentials_from_response(struct cognito_user_data *user_data) {

    int result = AWS_OP_ERR;

    struct aws_json_value *response_document =
        aws_json_value_new_from_string(user_data->allocator, aws_byte_cursor_from_buf(&user_data->response_body));
    if (response_document == NULL) {
        goto done;
    }

    struct aws_json_value *credentials_entry =
        aws_json_value_get_from_object(response_document, aws_byte_cursor_from_string(s_credentials_key));
    if (credentials_entry == NULL) {
        goto done;
    }

    struct aws_parse_credentials_from_json_doc_options credentials_parse_options = {
        .access_key_id_name = aws_string_c_str(s_access_key_id_name),
        .secret_access_key_name = aws_string_c_str(s_secret_access_key_name),
        .token_name = aws_string_c_str(s_session_token_name),
        .expiration_name = aws_string_c_str(s_expiration_name),
        .expiration_format = AWS_PCEF_NUMBER_UNIX_EPOCH,
        .token_required = true,
        .expiration_required = true,
    };

    user_data->credentials =
        aws_parse_credentials_from_aws_json_object(user_data->allocator, credentials_entry, &credentials_parse_options);
    if (user_data->credentials == NULL) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_json_value_destroy(response_document);

    if (result != AWS_OP_SUCCESS) {
        aws_raise_error(AWS_AUTH_PROVIDER_PARSER_UNEXPECTED_RESPONSE);
    }

    return result;
}

static void s_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    (void)token;
    struct cognito_user_data *provider_user_data = user_data;

    if (error_code != AWS_ERROR_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider retry task failed: %s",
            (void *)provider_user_data->provider,
            aws_error_str(error_code));
        s_finalize_credentials_query(user_data, error_code);
        return;
    }

    s_user_data_reset(provider_user_data);

    struct aws_credentials_provider_cognito_impl *impl = provider_user_data->provider->impl;

    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_on_connection_setup_fn, provider_user_data);
}

static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {

    struct cognito_user_data *provider_user_data = user_data;
    struct aws_credentials_provider_cognito_impl *impl = provider_user_data->provider->impl;

    int http_response_code = 0;
    impl->function_table->aws_http_stream_get_incoming_response_status(stream, &http_response_code);

    if (http_response_code != 200) {
        error_code = AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE;
    }

    impl->function_table->aws_http_stream_release(stream);

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): GetCredentialsForIdentity call completed with http status %d",
        (void *)provider_user_data->provider,
        http_response_code);

    if (http_response_code == AWS_HTTP_STATUS_CODE_200_OK) {
        aws_retry_token_record_success(provider_user_data->retry_token);

        if (s_parse_credentials_from_response(provider_user_data) == AWS_OP_SUCCESS) {
            s_finalize_credentials_query(user_data, AWS_ERROR_SUCCESS);
            return;
        }

        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to parse GetCredentialsForIdentity response",
            (void *)provider_user_data->provider);

        error_code = AWS_AUTH_PROVIDER_PARSER_UNEXPECTED_RESPONSE;
    }

    /* Success path is done, error-only from here on out */

    /* Unsure if this should be unconditional or a function of status code. STS does this unconditionally. */
    impl->function_table->aws_http_connection_close(provider_user_data->connection);

    enum aws_retry_error_type error_type =
        aws_credentials_provider_compute_retry_error_type(http_response_code, error_code);
    bool can_retry = http_response_code == 0 || error_type != AWS_RETRY_ERROR_TYPE_CLIENT_ERROR;
    if (!can_retry) {
        s_finalize_credentials_query(user_data, error_code);
        return;
    }

    if (aws_retry_strategy_schedule_retry(
            provider_user_data->retry_token, error_type, s_on_retry_ready, provider_user_data)) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to schedule retry: %s",
            (void *)provider_user_data->provider,
            aws_error_str(error_code));
        s_finalize_credentials_query(user_data, error_code);
        return;
    }
}

static int s_on_incoming_body_fn(struct aws_http_stream *stream, const struct aws_byte_cursor *data, void *user_data) {
    (void)stream;

    struct cognito_user_data *provider_user_data = user_data;
    return aws_byte_buf_append_dynamic(&provider_user_data->response_body, data);
}

AWS_STATIC_STRING_FROM_LITERAL(s_identity_id_key, "IdentityId");
AWS_STATIC_STRING_FROM_LITERAL(s_custom_role_arn_key, "CustomRoleArn");
AWS_STATIC_STRING_FROM_LITERAL(s_logins_key, "Logins");

int s_create_get_credentials_for_identity_body_buffer(
    struct aws_byte_buf *buffer,
    struct cognito_user_data *provider_user_data) {
    struct aws_allocator *allocator = provider_user_data->allocator;
    struct aws_credentials_provider_cognito_impl *impl = provider_user_data->provider->impl;

    int result = AWS_OP_ERR;

    struct aws_json_value *json_body = aws_json_value_new_object(allocator);
    if (json_body == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_json_value *identity_string =
        aws_json_value_new_string(allocator, aws_byte_cursor_from_string(impl->identity));
    if (identity_string == NULL) {
        goto done;
    }

    if (aws_json_value_add_to_object(json_body, aws_byte_cursor_from_string(s_identity_id_key), identity_string)) {
        aws_json_value_destroy(identity_string);
        goto done;
    }

    if (impl->custom_role_arn != NULL) {
        struct aws_json_value *custom_role_arn_string =
            aws_json_value_new_string(allocator, aws_byte_cursor_from_string(impl->custom_role_arn));
        if (custom_role_arn_string == NULL) {
            goto done;
        }

        if (aws_json_value_add_to_object(
                json_body, aws_byte_cursor_from_string(s_custom_role_arn_key), custom_role_arn_string)) {
            aws_json_value_destroy(custom_role_arn_string);
            goto done;
        }
    }

    size_t login_count = aws_array_list_length(&impl->logins);
    if (login_count > 0) {
        struct aws_json_value *logins = aws_json_value_new_object(allocator);
        if (logins == NULL) {
            goto done;
        }

        if (aws_json_value_add_to_object(json_body, aws_byte_cursor_from_string(s_logins_key), logins)) {
            aws_json_value_destroy(logins);
            goto done;
        }

        for (size_t i = 0; i < login_count; ++i) {
            struct aws_cognito_login login;
            if (aws_array_list_get_at(&impl->logins, &login, i)) {
                goto done;
            }

            struct aws_json_value *login_value_string =
                aws_json_value_new_string(allocator, login.identity_provider_token);
            if (login_value_string == NULL) {
                goto done;
            }

            if (aws_json_value_add_to_object(logins, login.identity_provider_name, login_value_string)) {
                aws_json_value_destroy(login_value_string);
                goto done;
            }
        }
    }

    if (aws_byte_buf_append_json_string(json_body, buffer)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_json_value_destroy(json_body);

    return result;
}

static struct aws_http_header s_content_type_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("content-type"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("application/x-amz-json-1.1"),
};

AWS_STATIC_STRING_FROM_LITERAL(s_get_credentials_for_identity_path, "/");

static struct aws_http_header s_x_amz_target_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("X-Amz-Target"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("AWSCognitoIdentityService.GetCredentialsForIdentity"),
};

static int s_create_get_credentials_for_identity_request(struct cognito_user_data *provider_user_data) {
    struct aws_credentials_provider_cognito_impl *impl = provider_user_data->provider->impl;

    struct aws_byte_buf body_buffer;
    AWS_ZERO_STRUCT(body_buffer);

    struct aws_input_stream *body_stream = NULL;
    struct aws_http_message *request = aws_http_message_new_request(provider_user_data->allocator);
    if (request == NULL) {
        return AWS_OP_ERR;
    }

    if (aws_http_message_set_request_method(request, aws_http_method_post)) {
        goto on_error;
    }

    if (aws_http_message_set_request_path(request, aws_byte_cursor_from_string(s_get_credentials_for_identity_path))) {
        goto on_error;
    }

    struct aws_http_header host_header = {
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("host"),
        .value = aws_byte_cursor_from_string(impl->endpoint),
    };

    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_content_type_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_x_amz_target_header)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&body_buffer, provider_user_data->allocator, HTTP_REQUEST_BODY_INITIAL_SIZE)) {
        goto on_error;
    }

    if (s_create_get_credentials_for_identity_body_buffer(&body_buffer, provider_user_data)) {
        goto on_error;
    }

    char content_length[21];
    AWS_ZERO_ARRAY(content_length);
    snprintf(content_length, sizeof(content_length), "%" PRIu64, (uint64_t)body_buffer.len);

    struct aws_http_header content_length_header = {
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
        .value = aws_byte_cursor_from_c_str(content_length),
    };

    if (aws_http_message_add_header(request, content_length_header)) {
        goto on_error;
    }

    struct aws_byte_cursor payload_cur = aws_byte_cursor_from_buf(&body_buffer);
    body_stream = aws_input_stream_new_from_cursor(provider_user_data->allocator, &payload_cur);
    if (body_stream == NULL) {
        goto on_error;
    }

    aws_http_message_set_body_stream(request, body_stream);

    provider_user_data->get_credentials_request = request;
    provider_user_data->request_body_buffer = body_buffer;
    provider_user_data->request_body_stream = body_stream;

    return AWS_OP_SUCCESS;

on_error:

    aws_byte_buf_clean_up(&body_buffer);
    aws_input_stream_release(body_stream);
    aws_http_message_release(request);

    return AWS_OP_ERR;
}

static void s_on_connection_setup_fn(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct cognito_user_data *wrapped_user_data = user_data;
    struct aws_http_stream *stream = NULL;
    struct aws_credentials_provider_cognito_impl *impl = wrapped_user_data->provider->impl;

    if (connection == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to acquire http connection: %s",
            (void *)wrapped_user_data->provider,
            aws_error_debug_str(error_code));
        goto on_error;
    }

    wrapped_user_data->connection = connection;
    if (s_create_get_credentials_for_identity_request(wrapped_user_data)) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to create http request: %s",
            (void *)wrapped_user_data->provider,
            aws_error_debug_str(error_code));
        goto on_error;
    }

    struct aws_http_make_request_options options = {
        .user_data = user_data,
        .request = wrapped_user_data->get_credentials_request,
        .self_size = sizeof(struct aws_http_make_request_options),
        .on_response_headers = NULL,
        .on_response_header_block_done = NULL,
        .on_response_body = s_on_incoming_body_fn,
        .on_complete = s_on_stream_complete_fn,
    };

    stream = impl->function_table->aws_http_connection_make_request(connection, &options);
    if (!stream) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to create http stream: %s",
            (void *)wrapped_user_data->provider,
            aws_error_debug_str(error_code));
        goto on_error;
    }

    if (impl->function_table->aws_http_stream_activate(stream)) {
        error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to activate http stream: %s",
            (void *)wrapped_user_data->provider,
            aws_error_debug_str(error_code));
        goto on_error;
    }

    return;

on_error:

    impl->function_table->aws_http_stream_release(stream);
    s_finalize_credentials_query(wrapped_user_data, error_code);
}

static void s_on_retry_token_acquired(
    struct aws_retry_strategy *strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {
    (void)strategy;
    struct cognito_user_data *wrapped_user_data = user_data;

    if (token == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to acquire retry token: %s",
            (void *)wrapped_user_data->provider,
            aws_error_debug_str(error_code));
        s_finalize_credentials_query(wrapped_user_data, error_code);
        return;
    }

    wrapped_user_data->retry_token = token;

    struct aws_credentials_provider_cognito_impl *impl = wrapped_user_data->provider->impl;

    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_on_connection_setup_fn, wrapped_user_data);
}

static int s_credentials_provider_cognito_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_cognito_impl *impl = provider->impl;

    struct cognito_user_data *wrapped_user_data = s_user_data_new(provider, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto on_error;
    }

    if (aws_retry_strategy_acquire_retry_token(
            impl->retry_strategy, NULL, s_on_retry_token_acquired, wrapped_user_data, 100)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to acquire retry token with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:

    s_user_data_destroy(wrapped_user_data);

    return AWS_OP_ERR;
}

static void s_credentials_provider_cognito_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_cognito_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    /* aws_http_connection_manager_release will eventually leads to call of s_on_connection_manager_shutdown,
     * which will do memory release for provider and impl.
     */
    if (impl->connection_manager) {
        impl->function_table->aws_http_connection_manager_release(impl->connection_manager);
    } else {
        /* If provider setup failed halfway through, connection_manager might not exist.
         * In this case invoke shutdown completion callback directly to finish cleanup */
        s_on_connection_manager_shutdown(provider);
    }

    /* freeing the provider takes place in the shutdown callback below */
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_cognito_vtable = {
    .get_credentials = s_credentials_provider_cognito_get_credentials_async,
    .destroy = s_credentials_provider_cognito_destroy,
};

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;

    aws_credentials_provider_invoke_shutdown_callback(provider);

    struct aws_credentials_provider_cognito_impl *impl = provider->impl;

    aws_retry_strategy_release(impl->retry_strategy);

    aws_string_destroy(impl->endpoint);
    aws_string_destroy(impl->identity);
    aws_string_destroy(impl->custom_role_arn);

    for (size_t i = 0; i < aws_array_list_length(&impl->logins); ++i) {
        struct aws_cognito_login login;
        if (aws_array_list_get_at(&impl->logins, &login, i)) {
            continue;
        }

        s_aws_cognito_login_clean_up(&login);
    }

    aws_array_list_clean_up(&impl->logins);

    aws_mem_release(provider->allocator, provider);
}

static int s_validate_options(const struct aws_credentials_provider_cognito_options *options) {
    if (options == NULL) {
        return AWS_OP_ERR;
    }

    if (options->tls_ctx == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(static) Cognito credentials provider options must include a TLS context");
        return AWS_OP_ERR;
    }

    if (options->bootstrap == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(static) Cognito credentials provider options must include a client bootstrap");
        return AWS_OP_ERR;
    }

    if (options->endpoint.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(static) Cognito credentials provider options must have a non-empty endpoint");
        return AWS_OP_ERR;
    }

    if (options->identity.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(static) Cognito credentials provider options must have a non-empty identity");
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

struct aws_credentials_provider *aws_credentials_provider_new_cognito(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_cognito_options *options) {

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_cognito_impl *impl = NULL;

    if (s_validate_options(options)) {
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_cognito_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_cognito_vtable, impl);

    struct aws_tls_connection_options tls_connection_options;
    AWS_ZERO_STRUCT(tls_connection_options);
    aws_tls_connection_options_init_from_ctx(&tls_connection_options, options->tls_ctx);
    struct aws_byte_cursor host = options->endpoint;
    if (aws_tls_connection_options_set_server_name(&tls_connection_options, allocator, &host)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to create tls connection options with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = (uint32_t)aws_timestamp_convert(
        COGNITO_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = SIZE_MAX;
    manager_options.socket_options = &socket_options;
    manager_options.host = options->endpoint;
    manager_options.port = 443;
    manager_options.max_connections = 2;
    manager_options.shutdown_complete_callback = s_on_connection_manager_shutdown;
    manager_options.shutdown_complete_user_data = provider;
    manager_options.tls_connection_options = &tls_connection_options;
    manager_options.proxy_options = options->http_proxy_options;

    impl->function_table = options->function_table;
    if (impl->function_table == NULL) {
        impl->function_table = g_aws_credentials_provider_http_function_table;
    }

    impl->connection_manager = impl->function_table->aws_http_connection_manager_new(allocator, &manager_options);
    if (impl->connection_manager == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to create http connection manager with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    impl->endpoint = aws_string_new_from_cursor(allocator, &options->endpoint);
    impl->identity = aws_string_new_from_cursor(allocator, &options->identity);

    if (options->custom_role_arn != NULL) {
        impl->custom_role_arn = aws_string_new_from_cursor(allocator, options->custom_role_arn);
    }

    aws_array_list_init_dynamic(&impl->logins, allocator, options->login_count, sizeof(struct aws_cognito_login));

    for (size_t i = 0; i < options->login_count; ++i) {
        struct aws_cognito_identity_provider_token_pair *login_token_pair = &options->logins[i];

        struct aws_cognito_login login;
        if (s_aws_cognito_login_init(
                &login,
                allocator,
                login_token_pair->identity_provider_name,
                login_token_pair->identity_provider_token)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): Cognito credentials provider failed to initialize login entry with error %s",
                (void *)provider,
                aws_error_debug_str(aws_last_error()));
            goto on_error;
        }

        aws_array_list_push_back(&impl->logins, &login);
    }

    struct aws_standard_retry_options retry_options = {
        .backoff_retry_options =
            {
                .el_group = options->bootstrap->event_loop_group,
                .max_retries = COGNITO_MAX_RETRIES,
            },
    };

    impl->retry_strategy = aws_retry_strategy_new_standard(allocator, &retry_options);
    if (!impl->retry_strategy) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Cognito credentials provider failed to create a retry strategy with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    provider->shutdown_options = options->shutdown_options;

    aws_tls_connection_options_clean_up(&tls_connection_options);

    return provider;

on_error:

    aws_tls_connection_options_clean_up(&tls_connection_options);
    aws_credentials_provider_destroy(provider);

    return NULL;
}

/*************************************************************************/

#define DEFAULT_CREDENTIAL_PROVIDER_REFRESH_MS (15 * 60 * 1000)

/*
 * Cognito provider with caching implementation
 */
struct aws_credentials_provider *aws_credentials_provider_new_cognito_caching(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_cognito_options *options) {

    struct aws_credentials_provider *cognito_provider = NULL;
    struct aws_credentials_provider *caching_provider = NULL;

    cognito_provider = aws_credentials_provider_new_cognito(allocator, options);
    if (cognito_provider == NULL) {
        goto on_error;
    }

    struct aws_credentials_provider_cached_options cached_options = {
        .source = cognito_provider,
        .refresh_time_in_milliseconds = DEFAULT_CREDENTIAL_PROVIDER_REFRESH_MS,
    };

    caching_provider = aws_credentials_provider_new_cached(allocator, &cached_options);
    if (caching_provider == NULL) {
        goto on_error;
    }

    aws_credentials_provider_release(cognito_provider);

    return caching_provider;

on_error:

    aws_credentials_provider_release(caching_provider);
    aws_credentials_provider_release(cognito_provider);

    return NULL;
}
