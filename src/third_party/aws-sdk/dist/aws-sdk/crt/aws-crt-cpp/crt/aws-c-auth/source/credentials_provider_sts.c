/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/auth/credentials.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/auth/signing_config.h>
#include <aws/auth/signing_result.h>
#include <aws/common/clock.h>
#include <aws/common/environment.h>
#include <aws/common/string.h>
#include <aws/common/xml_parser.h>
#include <aws/sdkutils/aws_profile.h>

#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>

#include <aws/io/channel_bootstrap.h>
#include <aws/io/retry_strategy.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#include <inttypes.h>

#ifdef _MSC_VER
/* allow non-constant declared initializers. */
#    pragma warning(disable : 4204)
/* allow passing of address of automatic variable */
#    pragma warning(disable : 4221)
/* function pointer to dll symbol */
#    pragma warning(disable : 4232)
#endif

static int s_sts_xml_on_AssumeRoleResponse_child(struct aws_xml_node *, void *);
static int s_sts_xml_on_AssumeRoleResult_child(struct aws_xml_node *, void *);
static int s_sts_xml_on_Credentials_child(struct aws_xml_node *, void *);

static struct aws_http_header s_content_type_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("content-type"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("application/x-www-form-urlencoded"),
};

static struct aws_byte_cursor s_content_length = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("content-length");
static struct aws_byte_cursor s_path = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/");
AWS_STATIC_STRING_FROM_LITERAL(s_sts_service_name, "sts");
static const int s_max_retries = 3;

const uint16_t aws_sts_assume_role_default_duration_secs = 900;

struct aws_credentials_provider_sts_impl {
    struct aws_http_connection_manager *connection_manager;
    struct aws_string *assume_role_profile;
    struct aws_string *role_session_name;
    struct aws_string *external_id;
    struct aws_string *endpoint;
    struct aws_string *region;
    uint16_t duration_seconds;
    struct aws_credentials_provider *provider;
    struct aws_credentials_provider_shutdown_options source_shutdown_options;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_retry_strategy *retry_strategy;
    aws_io_clock_fn *system_clock_fn;
};

struct sts_creds_provider_user_data {
    struct aws_allocator *allocator;
    struct aws_credentials_provider *provider;
    struct aws_credentials *credentials;
    struct aws_string *access_key_id;
    struct aws_string *secret_access_key;
    struct aws_string *session_token;
    aws_on_get_credentials_callback_fn *callback;
    struct aws_http_connection *connection;
    struct aws_byte_buf payload_body;
    struct aws_input_stream *input_stream;
    struct aws_signable *signable;
    struct aws_signing_config_aws signing_config;
    struct aws_http_message *message;
    struct aws_byte_buf output_buf;

    struct aws_retry_token *retry_token;
    int error_code;
    void *user_data;
};

static void s_reset_request_specific_data(struct sts_creds_provider_user_data *user_data) {
    if (user_data->connection) {
        struct aws_credentials_provider_sts_impl *provider_impl = user_data->provider->impl;
        provider_impl->function_table->aws_http_connection_manager_release_connection(
            provider_impl->connection_manager, user_data->connection);
        user_data->connection = NULL;
    }

    if (user_data->signable) {
        aws_signable_destroy(user_data->signable);
        user_data->signable = NULL;
    }

    if (user_data->input_stream) {
        aws_input_stream_destroy(user_data->input_stream);
        user_data->input_stream = NULL;
    }

    aws_byte_buf_clean_up(&user_data->payload_body);

    if (user_data->message) {
        aws_http_message_destroy(user_data->message);
        user_data->message = NULL;
    }

    aws_byte_buf_clean_up(&user_data->output_buf);

    aws_string_destroy(user_data->access_key_id);
    user_data->access_key_id = NULL;

    aws_string_destroy_secure(user_data->secret_access_key);
    user_data->secret_access_key = NULL;

    aws_string_destroy(user_data->session_token);
    user_data->session_token = NULL;
}
static void s_clean_up_user_data(struct sts_creds_provider_user_data *user_data) {
    user_data->callback(user_data->credentials, user_data->error_code, user_data->user_data);

    aws_credentials_release(user_data->credentials);

    s_reset_request_specific_data(user_data);
    aws_credentials_provider_release(user_data->provider);

    aws_retry_token_release(user_data->retry_token);
    aws_mem_release(user_data->allocator, user_data);
}

static int s_write_body_to_buffer(struct aws_credentials_provider *provider, struct aws_byte_buf *body) {
    struct aws_credentials_provider_sts_impl *provider_impl = provider->impl;

    struct aws_byte_cursor working_cur = aws_byte_cursor_from_c_str("Version=2011-06-15&Action=AssumeRole&RoleArn=");
    if (aws_byte_buf_append_dynamic(body, &working_cur)) {
        return AWS_OP_ERR;
    }
    struct aws_byte_cursor role_cur = aws_byte_cursor_from_string(provider_impl->assume_role_profile);
    if (aws_byte_buf_append_encoding_uri_param(body, &role_cur)) {
        return AWS_OP_ERR;
    }
    working_cur = aws_byte_cursor_from_c_str("&RoleSessionName=");
    if (aws_byte_buf_append_dynamic(body, &working_cur)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor session_cur = aws_byte_cursor_from_string(provider_impl->role_session_name);
    if (aws_byte_buf_append_encoding_uri_param(body, &session_cur)) {
        return AWS_OP_ERR;
    }

    if (provider_impl->external_id != NULL) {
        working_cur = aws_byte_cursor_from_c_str("&ExternalId=");
        if (aws_byte_buf_append_dynamic(body, &working_cur)) {
            return AWS_OP_ERR;
        }

        struct aws_byte_cursor external_id_cur = aws_byte_cursor_from_string(provider_impl->external_id);
        if (aws_byte_buf_append_encoding_uri_param(body, &external_id_cur)) {
            return AWS_OP_ERR;
        }
    }

    working_cur = aws_byte_cursor_from_c_str("&DurationSeconds=");
    if (aws_byte_buf_append_dynamic(body, &working_cur)) {
        return AWS_OP_ERR;
    }

    char duration_seconds[6];
    AWS_ZERO_ARRAY(duration_seconds);
    snprintf(duration_seconds, sizeof(duration_seconds), "%" PRIu16, provider_impl->duration_seconds);
    working_cur = aws_byte_cursor_from_c_str(duration_seconds);
    if (aws_byte_buf_append_dynamic(body, &working_cur)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_on_incoming_body_fn(struct aws_http_stream *stream, const struct aws_byte_cursor *data, void *user_data) {
    (void)stream;

    struct sts_creds_provider_user_data *provider_user_data = user_data;
    return aws_byte_buf_append_dynamic(&provider_user_data->output_buf, data);
}

/* parse doc of form
<AssumeRoleResponse>
     <AssumeRoleResult>
          <Credentials>
             <AccessKeyId>accessKeyId</AccessKeyId>
             <SecretAccessKey>secretKey</SecretAccessKey>
             <SessionToken>sessionToken</SessionToken>
          </Credentials>
         <AssumedRoleUser>
             ... more stuff we don't care about.
         </AssumedRoleUser>
         ... more stuff we don't care about
      </AssumeRoleResult>
</AssumeRoleResponse>
 */
static int s_sts_xml_on_root(struct aws_xml_node *node, void *user_data) {
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AssumeRoleResponse")) {
        return aws_xml_node_traverse(node, s_sts_xml_on_AssumeRoleResponse_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_sts_xml_on_AssumeRoleResponse_child(struct aws_xml_node *node, void *user_data) {
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AssumeRoleResult")) {
        return aws_xml_node_traverse(node, s_sts_xml_on_AssumeRoleResult_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_sts_xml_on_AssumeRoleResult_child(struct aws_xml_node *node, void *user_data) {
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Credentials")) {
        return aws_xml_node_traverse(node, s_sts_xml_on_Credentials_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_sts_xml_on_Credentials_child(struct aws_xml_node *node, void *user_data) {
    struct sts_creds_provider_user_data *provider_user_data = user_data;
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    struct aws_byte_cursor credential_data;
    AWS_ZERO_STRUCT(credential_data);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AccessKeyId")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        provider_user_data->access_key_id = aws_string_new_from_cursor(provider_user_data->allocator, &credential_data);
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): Read AccessKeyId %s",
            (void *)provider_user_data->provider,
            aws_string_c_str(provider_user_data->access_key_id));
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SecretAccessKey")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        provider_user_data->secret_access_key =
            aws_string_new_from_cursor(provider_user_data->allocator, &credential_data);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SessionToken")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        provider_user_data->session_token = aws_string_new_from_cursor(provider_user_data->allocator, &credential_data);
    }

    return AWS_OP_SUCCESS;
}

static void s_start_make_request(
    struct aws_credentials_provider *provider,
    struct sts_creds_provider_user_data *provider_user_data);

static void s_on_retry_ready(struct aws_retry_token *token, int error_code, void *user_data) {
    (void)token;
    struct sts_creds_provider_user_data *provider_user_data = user_data;

    if (!error_code) {
        s_start_make_request(provider_user_data->provider, provider_user_data);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): retry task failed: %s",
            (void *)provider_user_data->provider,
            aws_error_str(aws_last_error()));
        s_clean_up_user_data(provider_user_data);
    }
}

/* called upon completion of http request */
static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {
    int http_response_code = 0;
    struct sts_creds_provider_user_data *provider_user_data = user_data;
    struct aws_credentials_provider_sts_impl *provider_impl = provider_user_data->provider->impl;

    provider_user_data->error_code = error_code;

    if (provider_impl->function_table->aws_http_stream_get_incoming_response_status(stream, &http_response_code)) {
        goto finish;
    }

    if (http_response_code != 200) {
        provider_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE;
    }

    provider_impl->function_table->aws_http_stream_release(stream);

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): AssumeRole call completed with http status %d",
        (void *)provider_user_data->provider,
        http_response_code);

    if (error_code || http_response_code != AWS_HTTP_STATUS_CODE_200_OK) {
        /* prevent connection reuse. */
        provider_impl->function_table->aws_http_connection_close(provider_user_data->connection);

        enum aws_retry_error_type error_type =
            aws_credentials_provider_compute_retry_error_type(http_response_code, error_code);

        s_reset_request_specific_data(provider_user_data);

        /* don't retry client errors at all. */
        if (error_type != AWS_RETRY_ERROR_TYPE_CLIENT_ERROR) {
            if (aws_retry_strategy_schedule_retry(
                    provider_user_data->retry_token, error_type, s_on_retry_ready, provider_user_data)) {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "(id=%p): failed to schedule retry: %s",
                    (void *)provider_user_data->provider,
                    aws_error_str(aws_last_error()));
                goto finish;
            }
            return;
        }
    }

    if (!error_code && http_response_code == AWS_HTTP_STATUS_CODE_200_OK) {
        /* update the book keeping so we can let the retry strategy make determinations about when the service is
         * healthy after an outage. */
        if (aws_retry_token_record_success(provider_user_data->retry_token)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): failed to register operation success: %s",
                (void *)provider_user_data->provider,
                aws_error_str(aws_last_error()));
            goto finish;
        }

        uint64_t now = UINT64_MAX;
        if (provider_impl->system_clock_fn(&now) != AWS_OP_SUCCESS) {
            goto finish;
        }

        uint64_t now_seconds = aws_timestamp_convert(now, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);

        struct aws_xml_parser_options options = {
            .doc = aws_byte_cursor_from_buf(&provider_user_data->output_buf),
            .on_root_encountered = s_sts_xml_on_root,
            .user_data = provider_user_data,
        };
        if (aws_xml_parse(provider_user_data->provider->allocator, &options)) {
            provider_user_data->error_code = aws_last_error();
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): credentials parsing failed with error %s",
                (void *)provider_user_data->credentials,
                aws_error_debug_str(provider_user_data->error_code));

            provider_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_STS_SOURCE_FAILURE;
            goto finish;
        }

        if (provider_user_data->access_key_id && provider_user_data->secret_access_key &&
            provider_user_data->session_token) {

            provider_user_data->credentials = aws_credentials_new_from_string(
                provider_user_data->allocator,
                provider_user_data->access_key_id,
                provider_user_data->secret_access_key,
                provider_user_data->session_token,
                now_seconds + provider_impl->duration_seconds);
        }

        if (provider_user_data->credentials == NULL) {
            provider_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_STS_SOURCE_FAILURE;
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): credentials document was corrupted, treating as an error.",
                (void *)provider_user_data->provider);
        }
    }

finish:

    s_clean_up_user_data(provider_user_data);
}

/* called upon acquiring a connection from the pool */
static void s_on_connection_setup_fn(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct sts_creds_provider_user_data *provider_user_data = user_data;
    struct aws_credentials_provider_sts_impl *provider_impl = provider_user_data->provider->impl;
    struct aws_http_stream *stream = NULL;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): connection returned with error code %d",
        (void *)provider_user_data->provider,
        error_code);

    if (error_code) {
        aws_raise_error(error_code);
        goto error;
    }
    provider_user_data->connection = connection;

    if (aws_byte_buf_init(&provider_user_data->output_buf, provider_impl->provider->allocator, 2048)) {
        goto error;
    }

    struct aws_http_make_request_options options = {
        .user_data = user_data,
        .request = provider_user_data->message,
        .self_size = sizeof(struct aws_http_make_request_options),
        .on_response_headers = NULL,
        .on_response_header_block_done = NULL,
        .on_response_body = s_on_incoming_body_fn,
        .on_complete = s_on_stream_complete_fn,
    };

    stream = provider_impl->function_table->aws_http_connection_make_request(connection, &options);

    if (!stream) {
        goto error;
    }

    if (provider_impl->function_table->aws_http_stream_activate(stream)) {
        goto error;
    }

    return;
error:
    provider_impl->function_table->aws_http_stream_release(stream);
    s_clean_up_user_data(provider_user_data);
}

/* called once sigv4 signing is complete. */
void s_on_signing_complete(struct aws_signing_result *result, int error_code, void *userdata) {
    struct sts_creds_provider_user_data *provider_user_data = userdata;
    struct aws_credentials_provider_sts_impl *sts_impl = provider_user_data->provider->impl;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): signing completed with error code %d",
        (void *)provider_user_data->provider,
        error_code);

    if (error_code) {
        provider_user_data->error_code = error_code;
        aws_raise_error(error_code);
        goto error;
    }

    if (aws_apply_signing_result_to_http_request(
            provider_user_data->message, provider_user_data->provider->allocator, result)) {
        goto error;
    }

    sts_impl->function_table->aws_http_connection_manager_acquire_connection(
        sts_impl->connection_manager, s_on_connection_setup_fn, provider_user_data);
    return;

error:
    s_clean_up_user_data(provider_user_data);
}

static void s_start_make_request(
    struct aws_credentials_provider *provider,
    struct sts_creds_provider_user_data *provider_user_data) {
    provider_user_data->message = aws_http_message_new_request(provider->allocator);
    struct aws_credentials_provider_sts_impl *impl = provider->impl;

    if (!provider_user_data->message) {
        goto error;
    }
    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_string(impl->endpoint),
    };

    if (aws_http_message_add_header(provider_user_data->message, host_header)) {
        goto error;
    }

    if (aws_http_message_add_header(provider_user_data->message, s_content_type_header)) {
        goto error;
    }

    if (aws_byte_buf_init(&provider_user_data->payload_body, provider->allocator, 256)) {
        goto error;
    }

    if (s_write_body_to_buffer(provider, &provider_user_data->payload_body)) {
        goto error;
    }

    char content_length[21];
    AWS_ZERO_ARRAY(content_length);
    snprintf(content_length, sizeof(content_length), "%" PRIu64, (uint64_t)provider_user_data->payload_body.len);

    struct aws_http_header content_len_header = {
        .name = s_content_length,
        .value = aws_byte_cursor_from_c_str(content_length),
    };

    if (aws_http_message_add_header(provider_user_data->message, content_len_header)) {
        goto error;
    }

    struct aws_byte_cursor payload_cur = aws_byte_cursor_from_buf(&provider_user_data->payload_body);
    provider_user_data->input_stream =
        aws_input_stream_new_from_cursor(provider_user_data->provider->allocator, &payload_cur);

    if (!provider_user_data->input_stream) {
        goto error;
    }

    aws_http_message_set_body_stream(provider_user_data->message, provider_user_data->input_stream);

    if (aws_http_message_set_request_method(provider_user_data->message, aws_http_method_post)) {
        goto error;
    }

    if (aws_http_message_set_request_path(provider_user_data->message, s_path)) {
        goto error;
    }

    provider_user_data->signable = aws_signable_new_http_request(provider->allocator, provider_user_data->message);

    if (!provider_user_data->signable) {
        goto error;
    }

    provider_user_data->signing_config.algorithm = AWS_SIGNING_ALGORITHM_V4;
    provider_user_data->signing_config.signature_type = AWS_ST_HTTP_REQUEST_HEADERS;
    provider_user_data->signing_config.signed_body_header = AWS_SBHT_NONE;
    provider_user_data->signing_config.config_type = AWS_SIGNING_CONFIG_AWS;
    provider_user_data->signing_config.credentials_provider = impl->provider;
    uint64_t now = UINT64_MAX;
    if (impl->system_clock_fn(&now) != AWS_OP_SUCCESS) {
        goto error;
    }
    uint64_t now_millis = aws_timestamp_convert(now, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_MILLIS, NULL);
    aws_date_time_init_epoch_millis(&provider_user_data->signing_config.date, now_millis);
    provider_user_data->signing_config.region = aws_byte_cursor_from_string(impl->region);
    provider_user_data->signing_config.service = aws_byte_cursor_from_string(s_sts_service_name);
    provider_user_data->signing_config.flags.use_double_uri_encode = false;

    if (aws_sign_request_aws(
            provider->allocator,
            provider_user_data->signable,
            (struct aws_signing_config_base *)&provider_user_data->signing_config,
            s_on_signing_complete,
            provider_user_data)) {
        goto error;
    }

    return;

error:
    AWS_LOGF_ERROR(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): error occurred while creating an http request for signing: %s",
        (void *)provider_user_data->provider,
        aws_error_debug_str(aws_last_error()));
    if (provider_user_data) {
        s_clean_up_user_data(provider_user_data);
    } else {
        provider_user_data->callback(NULL, provider_user_data->error_code, provider_user_data->user_data);
    }
}

static void s_on_retry_token_acquired(
    struct aws_retry_strategy *strategy,
    int error_code,
    struct aws_retry_token *token,
    void *user_data) {
    (void)strategy;
    struct sts_creds_provider_user_data *provider_user_data = user_data;

    if (!error_code) {
        provider_user_data->retry_token = token;
        s_start_make_request(provider_user_data->provider, provider_user_data);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to acquire retry token: %s",
            (void *)provider_user_data->provider,
            aws_error_debug_str(error_code));
        s_clean_up_user_data(provider_user_data);
    }
}

static int s_sts_get_creds(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_sts_impl *impl = provider->impl;

    AWS_LOGF_DEBUG(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): fetching credentials", (void *)provider);

    struct sts_creds_provider_user_data *provider_user_data =
        aws_mem_calloc(provider->allocator, 1, sizeof(struct sts_creds_provider_user_data));

    if (!provider_user_data) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): error occurred while allocating memory: %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        callback(NULL, aws_last_error(), user_data);
        return AWS_OP_ERR;
    }

    provider_user_data->allocator = provider->allocator;
    provider_user_data->provider = provider;
    aws_credentials_provider_acquire(provider);
    provider_user_data->callback = callback;
    provider_user_data->user_data = user_data;

    if (aws_retry_strategy_acquire_retry_token(
            impl->retry_strategy, NULL, s_on_retry_token_acquired, provider_user_data, 100)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to acquire retry token: %s",
            (void *)provider_user_data->provider,
            aws_error_debug_str(aws_last_error()));
        callback(NULL, aws_last_error(), user_data);
        s_clean_up_user_data(user_data);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static void s_on_credentials_provider_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;
    if (provider == NULL) {
        return;
    }

    struct aws_credentials_provider_sts_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    /* The wrapped provider has shut down, invoke its shutdown callback if there was one */
    if (impl->source_shutdown_options.shutdown_callback != NULL) {
        impl->source_shutdown_options.shutdown_callback(impl->source_shutdown_options.shutdown_user_data);
    }

    /* Invoke our own shutdown callback */
    aws_credentials_provider_invoke_shutdown_callback(provider);

    aws_string_destroy(impl->role_session_name);
    aws_string_destroy(impl->assume_role_profile);
    aws_string_destroy(impl->external_id);
    aws_string_destroy(impl->endpoint);
    aws_string_destroy(impl->region);
    aws_mem_release(provider->allocator, provider);
}

void s_destroy(struct aws_credentials_provider *provider) {
    AWS_LOGF_TRACE(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): cleaning up credentials provider", (void *)provider);

    struct aws_credentials_provider_sts_impl *sts_impl = provider->impl;

    if (sts_impl->connection_manager) {
        sts_impl->function_table->aws_http_connection_manager_release(sts_impl->connection_manager);
    }

    aws_retry_strategy_release(sts_impl->retry_strategy);
    aws_credentials_provider_release(sts_impl->provider);
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_sts_vtable = {
    .get_credentials = s_sts_get_creds,
    .destroy = s_destroy,
};

AWS_STATIC_STRING_FROM_LITERAL(s_region_config, "region");

/*
 * Try to resolve the region in the following order
 * 1. Check `AWS_REGION` environment variable
 * 1. Check `AWS_DEFAULT_REGION` environment variable
 * 2. check `region` config file property.
 */
static struct aws_string *s_resolve_region(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sts_options *options) {
    /* check environment variable first */
    struct aws_string *region = aws_credentials_provider_resolve_region_from_env(allocator);
    if (region != NULL && region->len > 0) {
        return region;
    }

    struct aws_profile_collection *profile_collection = NULL;
    struct aws_string *profile_name = NULL;

    /* check the config file */
    if (options->profile_collection_cached) {
        profile_collection = aws_profile_collection_acquire(options->profile_collection_cached);
    } else {
        profile_collection =
            aws_load_profile_collection_from_config_file(allocator, options->config_file_name_override);
    }
    if (!profile_collection) {
        goto cleanup;
    }
    profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
    if (!profile_name) {
        goto cleanup;
    }
    const struct aws_profile *profile = aws_profile_collection_get_profile(profile_collection, profile_name);
    if (!profile) {
        goto cleanup;
    }
    const struct aws_profile_property *property = aws_profile_get_property(profile, s_region_config);
    if (property) {
        region = aws_string_new_from_string(allocator, aws_profile_property_get_value(property));
    }
cleanup:
    aws_string_destroy(profile_name);
    aws_profile_collection_release(profile_collection);
    return region;
}

struct aws_credentials_provider *aws_credentials_provider_new_sts(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sts_options *options) {

    if (!options->bootstrap) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "a client bootstrap is necessary for quering STS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (!options->tls_ctx) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "a TLS context is necessary for querying STS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (options->role_arn.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "role_arn is necessary for querying STS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (options->session_name.len == 0) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "role_session_name is necessary for querying STS");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_sts_impl *impl = NULL;
    int result = AWS_OP_ERR;
    struct aws_string *region = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_sts_impl));

    AWS_LOGF_DEBUG(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "static: creating STS credentials provider");
    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_sts_vtable, impl);

    impl->function_table = g_aws_credentials_provider_http_function_table;

    if (options->function_table) {
        impl->function_table = options->function_table;
    }

    struct aws_tls_connection_options tls_connection_options;
    AWS_ZERO_STRUCT(tls_connection_options);

    if (!options->creds_provider) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "(id=%p): A credentials provider must be specified", (void *)provider);
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        goto on_done;
    }

    impl->role_session_name =
        aws_string_new_from_array(allocator, options->session_name.ptr, options->session_name.len);
    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): using role_session_name '%s'",
        (void *)provider,
        aws_string_c_str(impl->role_session_name));

    impl->assume_role_profile = aws_string_new_from_array(allocator, options->role_arn.ptr, options->role_arn.len);
    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): using role_arn '%s'",
        (void *)provider,
        aws_string_c_str(impl->assume_role_profile));

    if (options->external_id.len > 0) {
        impl->external_id = aws_string_new_from_cursor(allocator, &options->external_id);
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): using external_id '%s'",
            (void *)provider,
            aws_string_c_str(impl->external_id));
    }

    impl->duration_seconds = options->duration_seconds;

    if (options->system_clock_fn != NULL) {
        impl->system_clock_fn = options->system_clock_fn;
    } else {
        impl->system_clock_fn = aws_sys_clock_get_ticks;
    }

    /* minimum for STS is 900 seconds*/
    if (impl->duration_seconds < aws_sts_assume_role_default_duration_secs) {
        impl->duration_seconds = aws_sts_assume_role_default_duration_secs;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p): using credentials duration %" PRIu16,
        (void *)provider,
        impl->duration_seconds);

    impl->provider = options->creds_provider;
    aws_credentials_provider_acquire(impl->provider);

    /*
     * Construct a regional endpoint if we can resolve region from envrionment variable or the config file. Otherwise,
     * use the global endpoint.
     */
    region = s_resolve_region(allocator, options);
    if (region != NULL) {
        if (aws_credentials_provider_construct_regional_endpoint(
                allocator, &impl->endpoint, region, s_sts_service_name)) {
            goto on_done;
        }
        impl->region = aws_string_new_from_string(allocator, region);
    } else {
        /* use the global endpoint */
        impl->endpoint = aws_string_new_from_c_str(allocator, "sts.amazonaws.com");
        impl->region = aws_string_new_from_c_str(allocator, "us-east-1");
    }
    struct aws_byte_cursor endpoint_cursor = aws_byte_cursor_from_string(impl->endpoint);

    aws_tls_connection_options_init_from_ctx(&tls_connection_options, options->tls_ctx);

    if (aws_tls_connection_options_set_server_name(&tls_connection_options, allocator, &endpoint_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to create a tls connection options with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_done;
    }

    struct aws_socket_options socket_options = {
        .type = AWS_SOCKET_STREAM,
        .domain = AWS_SOCKET_IPV6,
        .connect_timeout_ms = 3000,
    };

    struct aws_http_connection_manager_options connection_manager_options = {
        .bootstrap = options->bootstrap,
        .host = endpoint_cursor,
        .initial_window_size = SIZE_MAX,
        .max_connections = 2,
        .port = 443,
        .socket_options = &socket_options,
        .tls_connection_options = &tls_connection_options,
        .proxy_options = options->http_proxy_options,
    };

    impl->connection_manager =
        impl->function_table->aws_http_connection_manager_new(allocator, &connection_manager_options);

    if (!impl->connection_manager) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to create a connection manager with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_done;
    }

    /*
     * Save the wrapped provider's shutdown callback and then swap it with our own.
     */
    impl->source_shutdown_options = impl->provider->shutdown_options;
    impl->provider->shutdown_options.shutdown_callback = s_on_credentials_provider_shutdown;
    impl->provider->shutdown_options.shutdown_user_data = provider;

    provider->shutdown_options = options->shutdown_options;

    struct aws_standard_retry_options retry_options = {
        .backoff_retry_options =
            {
                .el_group = options->bootstrap->event_loop_group,
                .max_retries = s_max_retries,
            },
    };

    impl->retry_strategy = aws_retry_strategy_new_standard(allocator, &retry_options);

    if (!impl->retry_strategy) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p): failed to create a retry strategy with error %s",
            (void *)provider,
            aws_error_debug_str(aws_last_error()));
        goto on_done;
    }
    result = AWS_OP_SUCCESS;

on_done:
    aws_tls_connection_options_clean_up(&tls_connection_options);
    aws_string_destroy(region);
    if (result != AWS_OP_SUCCESS) {
        provider = aws_credentials_provider_release(provider);
    }
    return provider;
}
