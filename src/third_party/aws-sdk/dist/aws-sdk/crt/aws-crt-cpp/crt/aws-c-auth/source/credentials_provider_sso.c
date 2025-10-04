/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/auth/private/sso_token_providers.h>
#include <aws/auth/private/sso_token_utils.h>

#include <aws/common/clock.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#endif /* _MSC_VER */

#define SSO_RESPONSE_SIZE_INITIAL 2048
#define SSO_RESPONSE_SIZE_LIMIT 10000
#define SSO_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 2
#define SSO_MAX_ATTEMPTS 3
#define SSO_RETRY_TIMEOUT_MS 100

struct aws_credentials_provider_sso_impl {
    struct aws_http_connection_manager *connection_manager;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_string *endpoint;
    struct aws_string *sso_account_id;
    struct aws_string *sso_role_name;
    struct aws_credentials_provider *token_provider;
    struct aws_retry_strategy *retry_strategy;
};

/**
 * aws_sso_query_context - context for each outstanding SSO query.
 */
struct aws_sso_query_context {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_credentials_provider *provider;
    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;

    /* mutable */
    struct aws_http_connection *connection;
    struct aws_http_message *request;
    struct aws_byte_buf payload;
    struct aws_retry_token *retry_token;
    struct aws_byte_buf path_and_query;
    struct aws_string *token;

    int status_code;
    int error_code;
};

/* called in between retries. */
static void s_sso_query_context_reset_request_specific_data(struct aws_sso_query_context *sso_query_context) {
    if (sso_query_context->request) {
        aws_http_message_release(sso_query_context->request);
        sso_query_context->request = NULL;
    }
    if (sso_query_context->connection) {
        struct aws_credentials_provider_sso_impl *provider_impl = sso_query_context->provider->impl;
        int result = provider_impl->function_table->aws_http_connection_manager_release_connection(
            provider_impl->connection_manager, sso_query_context->connection);
        (void)result;
        AWS_ASSERT(result == AWS_OP_SUCCESS);
        sso_query_context->connection = NULL;
    }
    if (sso_query_context->token) {
        aws_string_destroy_secure(sso_query_context->token);
        sso_query_context->token = NULL;
    }
    sso_query_context->status_code = 0;
    sso_query_context->error_code = 0;
}

static void s_sso_query_context_destroy(struct aws_sso_query_context *sso_query_context) {
    if (sso_query_context == NULL) {
        return;
    }

    s_sso_query_context_reset_request_specific_data(sso_query_context);
    aws_byte_buf_clean_up(&sso_query_context->payload);
    aws_byte_buf_clean_up(&sso_query_context->path_and_query);
    aws_credentials_provider_release(sso_query_context->provider);
    aws_retry_token_release(sso_query_context->retry_token);
    aws_mem_release(sso_query_context->allocator, sso_query_context);
}

static struct aws_sso_query_context *s_sso_query_context_new(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {
    struct aws_credentials_provider_sso_impl *impl = provider->impl;

    struct aws_sso_query_context *sso_query_context =
        aws_mem_calloc(provider->allocator, 1, sizeof(struct aws_sso_query_context));
    sso_query_context->allocator = provider->allocator;
    sso_query_context->provider = aws_credentials_provider_acquire(provider);
    sso_query_context->original_user_data = user_data;
    sso_query_context->original_callback = callback;

    /* construct path and query */
    struct aws_byte_cursor account_id_cursor = aws_byte_cursor_from_string(impl->sso_account_id);
    struct aws_byte_cursor role_name_cursor = aws_byte_cursor_from_string(impl->sso_role_name);
    struct aws_byte_cursor path_cursor = aws_byte_cursor_from_c_str("/federation/credentials?account_id=");
    struct aws_byte_cursor role_name_param_cursor = aws_byte_cursor_from_c_str("&role_name=");

    if (aws_byte_buf_init_copy_from_cursor(&sso_query_context->path_and_query, provider->allocator, path_cursor) ||
        aws_byte_buf_append_encoding_uri_param(&sso_query_context->path_and_query, &account_id_cursor) ||
        aws_byte_buf_append_dynamic(&sso_query_context->path_and_query, &role_name_param_cursor) ||
        aws_byte_buf_append_encoding_uri_param(&sso_query_context->path_and_query, &role_name_cursor)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&sso_query_context->payload, provider->allocator, SSO_RESPONSE_SIZE_INITIAL)) {
        goto on_error;
    }

    return sso_query_context;

on_error:
    s_sso_query_context_destroy(sso_query_context);

    return NULL;
}

/*
 * No matter the result, this always gets called assuming that sso_query_context is successfully allocated
 */
static void s_finalize_get_credentials_query(struct aws_sso_query_context *sso_query_context) {
    struct aws_credentials *credentials = NULL;

    if (sso_query_context->error_code == AWS_ERROR_SUCCESS) {
        /* parse credentials */
        struct aws_parse_credentials_from_json_doc_options parse_options = {
            .access_key_id_name = "accessKeyId",
            .secret_access_key_name = "secretAccessKey",
            .token_name = "sessionToken",
            .expiration_name = "expiration",
            .top_level_object_name = "roleCredentials",
            .token_required = true,
            .expiration_required = true,
            .expiration_format = AWS_PCEF_NUMBER_UNIX_EPOCH_MS,
        };

        credentials = aws_parse_credentials_from_json_document(
            sso_query_context->allocator, aws_byte_cursor_from_buf(&sso_query_context->payload), &parse_options);
    }

    if (credentials) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) successfully queried credentials",
            (void *)sso_query_context->provider);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to query credentials",
            (void *)sso_query_context->provider);

        if (sso_query_context->error_code == AWS_ERROR_SUCCESS) {
            sso_query_context->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE;
        }
    }

    /* pass the credentials back */
    sso_query_context->original_callback(
        credentials, sso_query_context->error_code, sso_query_context->original_user_data);

    /* clean up */
    s_sso_query_context_destroy(sso_query_context);
    aws_credentials_release(credentials);
}
static void s_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data);

static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct aws_sso_query_context *sso_query_context = user_data;

    struct aws_credentials_provider_sso_impl *impl = sso_query_context->provider->impl;
    impl->function_table->aws_http_stream_release(stream);

    /* set error code */
    sso_query_context->error_code = error_code;
    impl->function_table->aws_http_stream_get_incoming_response_status(stream, &sso_query_context->status_code);
    if (error_code == AWS_OP_SUCCESS && sso_query_context->status_code != AWS_HTTP_STATUS_CODE_200_OK) {
        sso_query_context->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE;
    }

    /*
     * If we can retry the request based on error response or http status code failure, retry it, otherwise, call the
     * finalize function.
     */
    if (error_code || sso_query_context->status_code != AWS_HTTP_STATUS_CODE_200_OK) {
        enum aws_retry_error_type error_type =
            aws_credentials_provider_compute_retry_error_type(sso_query_context->status_code, error_code);

        /* don't retry client errors at all. */
        if (error_type != AWS_RETRY_ERROR_TYPE_CLIENT_ERROR) {
            if (aws_retry_strategy_schedule_retry(
                    sso_query_context->retry_token, error_type, s_on_retry_ready, sso_query_context) ==
                AWS_OP_SUCCESS) {
                AWS_LOGF_INFO(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "(id=%p): successfully scheduled a retry",
                    (void *)sso_query_context->provider);
                return;
            }
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): failed to schedule retry: %s",
                (void *)sso_query_context->provider,
                aws_error_str(aws_last_error()));
            sso_query_context->error_code = aws_last_error();
        }
    } else {
        int result = aws_retry_token_record_success(sso_query_context->retry_token);
        (void)result;
        AWS_ASSERT(result == AWS_ERROR_SUCCESS);
    }

    s_finalize_get_credentials_query(sso_query_context);
}

static int s_on_incoming_body_fn(struct aws_http_stream *stream, const struct aws_byte_cursor *body, void *user_data) {

    (void)stream;

    struct aws_sso_query_context *sso_query_context = user_data;

    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) received %zu response bytes",
        (void *)sso_query_context->provider,
        body->len);

    if (body->len + sso_query_context->payload.len > SSO_RESPONSE_SIZE_LIMIT) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) response exceeded maximum allowed length",
            (void *)sso_query_context->provider);

        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (aws_byte_buf_append_dynamic(&sso_query_context->payload, body)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) error appending response payload: %s",
            (void *)sso_query_context->provider,
            aws_error_str(aws_last_error()));

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/* Request headers. */
AWS_STATIC_STRING_FROM_LITERAL(s_sso_token_header, "x-amz-sso_bearer_token");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_user_agent_header, "User-Agent");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_user_agent_header_value, "aws-sdk-crt/sso-credentials-provider");

static void s_query_credentials(struct aws_sso_query_context *sso_query_context) {
    AWS_FATAL_ASSERT(sso_query_context->connection);
    struct aws_http_stream *stream = NULL;
    struct aws_credentials_provider_sso_impl *impl = sso_query_context->provider->impl;

    sso_query_context->request = aws_http_message_new_request(sso_query_context->allocator);
    if (sso_query_context->request == NULL) {
        goto on_error;
    }

    struct aws_http_header auth_header = {
        .name = aws_byte_cursor_from_string(s_sso_token_header),
        .value = aws_byte_cursor_from_string(sso_query_context->token),
    };
    struct aws_http_header host_header = {
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Host"),
        .value = aws_byte_cursor_from_string(impl->endpoint),
    };
    struct aws_http_header user_agent_header = {
        .name = aws_byte_cursor_from_string(s_sso_user_agent_header),
        .value = aws_byte_cursor_from_string(s_sso_user_agent_header_value),
    };

    if (aws_http_message_add_header(sso_query_context->request, auth_header) ||
        aws_http_message_add_header(sso_query_context->request, host_header) ||
        aws_http_message_add_header(sso_query_context->request, user_agent_header)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to add http header with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    if (aws_http_message_set_request_method(sso_query_context->request, aws_http_method_get)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to set request method with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    if (aws_http_message_set_request_path(
            sso_query_context->request, aws_byte_cursor_from_buf(&sso_query_context->path_and_query))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to set request path with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .on_response_headers = NULL,
        .on_response_header_block_done = NULL,
        .on_response_body = s_on_incoming_body_fn,
        .on_complete = s_on_stream_complete_fn,
        .user_data = sso_query_context,
        .request = sso_query_context->request,
    };

    stream = impl->function_table->aws_http_connection_make_request(sso_query_context->connection, &request_options);
    if (!stream) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to make request with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    if (impl->function_table->aws_http_stream_activate(stream)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) failed to activate the stream with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    return;

on_error:
    sso_query_context->error_code = aws_last_error();
    impl->function_table->aws_http_stream_release(stream);
    s_finalize_get_credentials_query(sso_query_context);
}

static void s_on_get_token_callback(struct aws_credentials *credentials, int error_code, void *user_data) {
    struct aws_sso_query_context *sso_query_context = user_data;

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: failed to acquire a token, error code %d(%s)",
            (void *)sso_query_context->provider,
            error_code,
            aws_error_str(error_code));
        sso_query_context->error_code = error_code;
        s_finalize_get_credentials_query(sso_query_context);
        return;
    }

    struct aws_byte_cursor token = aws_credentials_get_token(credentials);
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): successfully accquired a token",
        (void *)sso_query_context->provider);

    sso_query_context->token = aws_string_new_from_cursor(sso_query_context->allocator, &token);
    s_query_credentials(sso_query_context);
}

static void s_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct aws_sso_query_context *sso_query_context = user_data;

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: failed to acquire a connection, error code %d(%s)",
            (void *)sso_query_context->provider,
            error_code,
            aws_error_str(error_code));
        sso_query_context->error_code = error_code;
        s_finalize_get_credentials_query(sso_query_context);
        return;
    }
    AWS_LOGF_INFO(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): successfully accquired a connection",
        (void *)sso_query_context->provider);
    sso_query_context->connection = connection;

    struct aws_credentials_provider_sso_impl *impl = sso_query_context->provider->impl;
    if (aws_credentials_provider_get_credentials(impl->token_provider, s_on_get_token_callback, user_data)) {
        int last_error_code = aws_last_error();
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: failed to get a token, error code %d(%s)",
            (void *)sso_query_context->provider,
            last_error_code,
            aws_error_str(last_error_code));

        sso_query_context->error_code = last_error_code;
        s_finalize_get_credentials_query(sso_query_context);
    }
}

/* called for each retry. */
static void s_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    (void)token;
    struct aws_sso_query_context *sso_query_context = user_data;
    struct aws_credentials_provider_sso_impl *impl = sso_query_context->provider->impl;

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to schedule retry with error: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(error_code));
        sso_query_context->error_code = error_code;
        s_finalize_get_credentials_query(sso_query_context);
        return;
    }

    /* clear the result from previous attempt */
    s_sso_query_context_reset_request_specific_data(sso_query_context);

    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_on_acquire_connection, sso_query_context);
}

static void s_on_retry_token_acquired(
    struct aws_retry_strategy *strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {
    struct aws_sso_query_context *sso_query_context = user_data;
    (void)strategy;

    if (error_code) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to acquire retry token: %s",
            (void *)sso_query_context->provider,
            aws_error_debug_str(error_code));
        sso_query_context->error_code = error_code;
        s_finalize_get_credentials_query(sso_query_context);
        return;
    }

    sso_query_context->retry_token = token;
    struct aws_credentials_provider_sso_impl *impl = sso_query_context->provider->impl;
    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_on_acquire_connection, user_data);
}

static int s_credentials_provider_sso_get_credentials(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_sso_impl *impl = provider->impl;

    struct aws_sso_query_context *sso_query_context = s_sso_query_context_new(provider, callback, user_data);
    if (sso_query_context == NULL) {
        return AWS_OP_ERR;
    }

    if (aws_retry_strategy_acquire_retry_token(
            impl->retry_strategy, NULL, s_on_retry_token_acquired, sso_query_context, SSO_RETRY_TIMEOUT_MS)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to acquire retry token: %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    s_sso_query_context_destroy(sso_query_context);
    return AWS_OP_ERR;
}

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;

    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

static void s_credentials_provider_sso_destroy(struct aws_credentials_provider *provider) {

    struct aws_credentials_provider_sso_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }
    aws_string_destroy(impl->endpoint);
    aws_string_destroy(impl->sso_account_id);
    aws_string_destroy(impl->sso_role_name);
    aws_retry_strategy_release(impl->retry_strategy);
    aws_credentials_provider_release(impl->token_provider);

    /* aws_http_connection_manager_release will eventually leads to call of s_on_connection_manager_shutdown,
     * which will do memory release for provider and impl. So We should be freeing impl
     * related memory first, then call aws_http_connection_manager_release.
     */
    if (impl->connection_manager) {
        impl->function_table->aws_http_connection_manager_release(impl->connection_manager);
    } else {
        /* If provider setup failed halfway through, connection_manager might not exist.
         * In this case invoke shutdown completion callback directly to finish cleanup */
        s_on_connection_manager_shutdown(provider);
    }
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_sso_vtable = {
    .get_credentials = s_credentials_provider_sso_get_credentials,
    .destroy = s_credentials_provider_sso_destroy,
};
AWS_STATIC_STRING_FROM_LITERAL(s_sso_service_name, "portal.sso");

AWS_STATIC_STRING_FROM_LITERAL(s_sso_account_id, "sso_account_id");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_region, "sso_region");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_role_name, "sso_role_name");
AWS_STATIC_STRING_FROM_LITERAL(s_sso_session, "sso_session");

struct sso_parameters {
    struct aws_allocator *allocator;
    struct aws_string *endpoint;
    struct aws_string *sso_account_id;
    struct aws_string *sso_role_name;
    struct aws_credentials_provider *token_provider;
};

static void s_parameters_destroy(struct sso_parameters *parameters) {
    if (!parameters) {
        return;
    }
    aws_string_destroy(parameters->endpoint);
    aws_string_destroy(parameters->sso_account_id);
    aws_string_destroy(parameters->sso_role_name);
    aws_credentials_provider_release(parameters->token_provider);
    aws_mem_release(parameters->allocator, parameters);
}

/**
 * Read the config file and construct profile or sso_session token provider based on sso_session property.
 *
 * If the profile contains sso_session property, a valid config example is as follow.
 * [profile sso-profile]
 *   sso_session = dev
 *   sso_account_id = 012345678901
 *   sso_role_name = SampleRole
 *
 * [sso-session dev]
 *   sso_region = us-east-1
 *   sso_start_url = https://d-abc123.awsapps.com/start
 *
 * If the profile does't contains sso_session, the legacy valid config example is as follow.
 * [profile sso-profile]
 *  sso_account_id = 012345678901
 *  sso_region = us-east-1
 *  sso_role_name = SampleRole
 *  sso_start_url = https://d-abc123.awsapps.com/start-beta
 */
static struct sso_parameters *s_parameters_new(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sso_options *options) {

    struct sso_parameters *parameters = aws_mem_calloc(allocator, 1, sizeof(struct sso_parameters));
    parameters->allocator = allocator;

    struct aws_profile_collection *config_profile_collection = NULL;
    struct aws_string *profile_name = NULL;
    bool success = false;

    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: failed to resolve profile name");
        goto on_finish;
    }
    if (options->config_file_cached) {
        /* Use cached config file */
        config_profile_collection = aws_profile_collection_acquire(options->config_file_cached);
    } else {
        /* load config file */
        config_profile_collection =
            aws_load_profile_collection_from_config_file(allocator, options->config_file_name_override);
    }

    if (!config_profile_collection) {
        goto on_finish;
    }

    const struct aws_profile *profile = aws_profile_collection_get_profile(config_profile_collection, profile_name);
    if (!profile) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: failed to load \"%s\" profile", aws_string_c_str(profile_name));
        goto on_finish;
    }

    const struct aws_profile_property *sso_account_id = aws_profile_get_property(profile, s_sso_account_id);
    const struct aws_profile_property *sso_role_name = aws_profile_get_property(profile, s_sso_role_name);
    const struct aws_profile_property *sso_region = NULL;

    if (!sso_account_id) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: sso_account_id is missing");
        aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE);
        goto on_finish;
    }
    if (!sso_role_name) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: sso_role_name is missing");
        aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE);
        goto on_finish;
    }

    const struct aws_profile_property *sso_session_property = aws_profile_get_property(profile, s_sso_session);
    /* create the appropriate token provider based on sso_session property is available or not */
    if (sso_session_property) {
        /* construct sso_session token provider */
        struct aws_token_provider_sso_session_options token_provider_options = {
            .config_file_name_override = options->config_file_name_override,
            .config_file_cached = config_profile_collection,
            .profile_name_override = options->profile_name_override,
            .bootstrap = options->bootstrap,
            .tls_ctx = options->tls_ctx,
            .system_clock_fn = options->system_clock_fn,
        };
        parameters->token_provider = aws_token_provider_new_sso_session(allocator, &token_provider_options);
        if (!parameters->token_provider) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: unable to create a sso token provider");
            aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE);
            goto on_finish;
        }
        sso_region = aws_profile_get_property(
            aws_profile_collection_get_section(
                config_profile_collection,
                AWS_PROFILE_SECTION_TYPE_SSO_SESSION,
                aws_profile_property_get_value(sso_session_property)),
            s_sso_region);
    } else {
        /* construct profile token provider */
        struct aws_token_provider_sso_profile_options token_provider_options = {
            .config_file_name_override = options->config_file_name_override,
            .config_file_cached = config_profile_collection,
            .profile_name_override = options->profile_name_override,
            .system_clock_fn = options->system_clock_fn,
        };

        parameters->token_provider = aws_token_provider_new_sso_profile(allocator, &token_provider_options);
        if (!parameters->token_provider) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: unable to create a profile token provider");
            aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE);
            goto on_finish;
        }
        sso_region = aws_profile_get_property(profile, s_sso_region);
    }

    if (!sso_region) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "sso: sso_region is missing");
        aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_SSO_SOURCE_FAILURE);
        goto on_finish;
    }

    parameters->sso_account_id = aws_string_new_from_string(allocator, aws_profile_property_get_value(sso_account_id));
    parameters->sso_role_name = aws_string_new_from_string(allocator, aws_profile_property_get_value(sso_role_name));
    /* determine endpoint */
    if (aws_credentials_provider_construct_regional_endpoint(
            allocator, &parameters->endpoint, aws_profile_property_get_value(sso_region), s_sso_service_name)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to construct sso endpoint");
        goto on_finish;
    }
    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Successfully loaded all required parameters for sso credentials provider.");
    success = true;

on_finish:
    if (!success) {
        s_parameters_destroy(parameters);
        parameters = NULL;
    }
    aws_string_destroy(profile_name);
    aws_profile_collection_release(config_profile_collection);

    return parameters;
}

struct aws_credentials_provider *aws_credentials_provider_new_sso(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sso_options *options) {

    struct sso_parameters *parameters = s_parameters_new(allocator, options);
    if (!parameters) {
        return NULL;
    }

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_sso_impl *impl = NULL;
    struct aws_tls_connection_options tls_connection_options;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_sso_impl));

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);
    AWS_ZERO_STRUCT(tls_connection_options);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_sso_vtable, impl);

    if (!options->tls_ctx) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): a TLS context must be provided", (void *)provider);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto on_error;
    }

    if (!options->bootstrap) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): a bootstrap instance must be provided", (void *)provider);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto on_error;
    }

    aws_tls_connection_options_init_from_ctx(&tls_connection_options, options->tls_ctx);
    struct aws_byte_cursor host = aws_byte_cursor_from_string(parameters->endpoint);
    if (aws_tls_connection_options_set_server_name(&tls_connection_options, allocator, &host)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to create a tls connection options with error %s",
            (void *)provider,
            aws_error_str(aws_last_error()));
        goto on_error;
    }

    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = (uint32_t)aws_timestamp_convert(
        SSO_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = SSO_RESPONSE_SIZE_LIMIT;
    manager_options.socket_options = &socket_options;
    manager_options.host = host;
    manager_options.port = 443;
    manager_options.max_connections = 2;
    manager_options.shutdown_complete_callback = s_on_connection_manager_shutdown;
    manager_options.shutdown_complete_user_data = provider;
    manager_options.tls_connection_options = &tls_connection_options;

    impl->function_table = options->function_table;
    if (impl->function_table == NULL) {
        impl->function_table = g_aws_credentials_provider_http_function_table;
    }

    impl->connection_manager = impl->function_table->aws_http_connection_manager_new(allocator, &manager_options);
    if (impl->connection_manager == NULL) {
        goto on_error;
    }

    impl->token_provider = aws_credentials_provider_acquire(parameters->token_provider);
    impl->endpoint = aws_string_new_from_string(allocator, parameters->endpoint);
    impl->sso_account_id = aws_string_new_from_string(allocator, parameters->sso_account_id);
    impl->sso_role_name = aws_string_new_from_string(allocator, parameters->sso_role_name);

    provider->shutdown_options = options->shutdown_options;

    struct aws_standard_retry_options retry_options = {
        .backoff_retry_options =
            {
                .el_group = options->bootstrap->event_loop_group,
                .max_retries = SSO_MAX_ATTEMPTS,
            },
    };
    impl->retry_strategy = aws_retry_strategy_new_standard(allocator, &retry_options);
    if (!impl->retry_strategy) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to create a retry strategy with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_error;
    }

    s_parameters_destroy(parameters);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    return provider;

on_error:
    aws_credentials_provider_destroy(provider);
    s_parameters_destroy(parameters);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    return NULL;
}
