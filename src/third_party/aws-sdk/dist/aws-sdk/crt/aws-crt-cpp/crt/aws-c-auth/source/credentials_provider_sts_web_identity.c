/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/credentials.h>

#include <aws/auth/private/aws_profile.h>
#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/date_time.h>
#include <aws/common/environment.h>
#include <aws/common/string.h>
#include <aws/common/uuid.h>
#include <aws/common/xml_parser.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/file_utils.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/stream.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>
#include <inttypes.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#    pragma warning(disable : 4232)
#endif /* _MSC_VER */

#define STS_WEB_IDENTITY_RESPONSE_SIZE_INITIAL 2048
#define STS_WEB_IDENTITY_RESPONSE_SIZE_LIMIT 10000
#define STS_WEB_IDENTITY_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 2
#define STS_WEB_IDENTITY_CREDS_DEFAULT_DURATION_SECONDS 900
#define STS_WEB_IDENTITY_MAX_ATTEMPTS 3

static void s_on_connection_manager_shutdown(void *user_data);
static int s_stswebid_error_xml_on_Error_child(struct aws_xml_node *, void *);
static int s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResponse_child(struct aws_xml_node *, void *);
static int s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResult_child(struct aws_xml_node *, void *);
static int s_stswebid_200_xml_on_Credentials_child(struct aws_xml_node *, void *);

struct aws_credentials_provider_sts_web_identity_impl {
    struct aws_http_connection_manager *connection_manager;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_string *role_arn;
    struct aws_string *role_session_name;
    struct aws_string *token_file_path;
    struct aws_string *endpoint;
};

/*
 * Tracking structure for each outstanding async query to an sts_web_identity provider
 */
struct sts_web_identity_user_data {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_credentials_provider *sts_web_identity_provider;
    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;

    /* mutable */
    struct aws_http_connection *connection;
    struct aws_http_message *request;
    struct aws_byte_buf response;

    struct aws_string *access_key_id;
    struct aws_string *secret_access_key;
    struct aws_string *session_token;
    uint64_t expiration_timepoint_in_seconds;

    struct aws_byte_buf payload_buf;

    int status_code;
    int error_code;
    int attempt_count;
};

static void s_user_data_reset_request_and_response(struct sts_web_identity_user_data *user_data) {
    aws_byte_buf_reset(&user_data->response, true /*zero out*/);
    aws_byte_buf_reset(&user_data->payload_buf, true /*zero out*/);
    user_data->status_code = 0;
    if (user_data->request) {
        aws_input_stream_destroy(aws_http_message_get_body_stream(user_data->request));
    }
    aws_http_message_destroy(user_data->request);
    user_data->request = NULL;

    aws_string_destroy(user_data->access_key_id);
    user_data->access_key_id = NULL;

    aws_string_destroy_secure(user_data->secret_access_key);
    user_data->secret_access_key = NULL;

    aws_string_destroy_secure(user_data->session_token);
    user_data->session_token = NULL;
}

static void s_user_data_destroy(struct sts_web_identity_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }

    struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;

    if (user_data->connection) {
        impl->function_table->aws_http_connection_manager_release_connection(
            impl->connection_manager, user_data->connection);
    }
    s_user_data_reset_request_and_response(user_data);
    aws_byte_buf_clean_up(&user_data->response);

    aws_string_destroy(user_data->access_key_id);
    aws_string_destroy_secure(user_data->secret_access_key);
    aws_string_destroy_secure(user_data->session_token);

    aws_byte_buf_clean_up(&user_data->payload_buf);

    aws_credentials_provider_release(user_data->sts_web_identity_provider);
    aws_mem_release(user_data->allocator, user_data);
}

static struct sts_web_identity_user_data *s_user_data_new(
    struct aws_credentials_provider *sts_web_identity_provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct sts_web_identity_user_data *wrapped_user_data =
        aws_mem_calloc(sts_web_identity_provider->allocator, 1, sizeof(struct sts_web_identity_user_data));
    if (wrapped_user_data == NULL) {
        goto on_error;
    }

    wrapped_user_data->allocator = sts_web_identity_provider->allocator;
    wrapped_user_data->sts_web_identity_provider = sts_web_identity_provider;
    aws_credentials_provider_acquire(sts_web_identity_provider);
    wrapped_user_data->original_user_data = user_data;
    wrapped_user_data->original_callback = callback;

    if (aws_byte_buf_init(
            &wrapped_user_data->response,
            sts_web_identity_provider->allocator,
            STS_WEB_IDENTITY_RESPONSE_SIZE_INITIAL)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&wrapped_user_data->payload_buf, sts_web_identity_provider->allocator, 1024)) {
        goto on_error;
    }

    return wrapped_user_data;

on_error:

    s_user_data_destroy(wrapped_user_data);

    return NULL;
}

/*
 * In general, the STS_WEB_IDENTITY response document looks something like:
<AssumeRoleWithWebIdentityResponse xmlns="https://sts.amazonaws.com/doc/2011-06-15/">
  <AssumeRoleWithWebIdentityResult>
    <SubjectFromWebIdentityToken>amzn1.account.AF6RHO7KZU5XRVQJGXK6HB56KR2A</SubjectFromWebIdentityToken>
    <Audience>client.5498841531868486423.1548@apps.example.com</Audience>
    <AssumedRoleUser>
      <Arn>arn:aws:sts::123456789012:assumed-role/FederatedWebIdentityRole/app1</Arn>
      <AssumedRoleId>AROACLKWSDQRAOEXAMPLE:app1</AssumedRoleId>
    </AssumedRoleUser>
    <Credentials>
      <SessionToken>AQoDYXdzEE0a8ANXXXXXXXXNO1ewxE5TijQyp+IEXAMPLE</SessionToken>
      <SecretAccessKey>wJalrXUtnFEMI/K7MDENG/bPxRfiCYzEXAMPLEKEY</SecretAccessKey>
      <Expiration>2014-10-24T23:00:23Z</Expiration>
      <AccessKeyId>ASgeIAIOSFODNN7EXAMPLE</AccessKeyId>
    </Credentials>
    <Provider>www.amazon.com</Provider>
  </AssumeRoleWithWebIdentityResult>
  <ResponseMetadata>
    <RequestId>ad4156e9-bce1-11e2-82e6-6b6efEXAMPLE</RequestId>
  </ResponseMetadata>
</AssumeRoleWithWebIdentityResponse>

Error Response looks like:
<?xml version="1.0" encoding="UTF-8"?>
<Error>
  <Code>ExceptionName</Code>
  <Message>XXX</Message>
  <Resource>YYY</Resource>
  <RequestId>4442587FB7D0A2F9</RequestId>
</Error>
*/

static int s_stswebid_error_xml_on_root(struct aws_xml_node *node, void *user_data) {
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Error")) {
        return aws_xml_node_traverse(node, s_stswebid_error_xml_on_Error_child, user_data);
    }

    return AWS_OP_SUCCESS;
}

static int s_stswebid_error_xml_on_Error_child(struct aws_xml_node *node, void *user_data) {
    bool *get_retryable_error = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Code")) {

        struct aws_byte_cursor data_cursor = {0};
        if (aws_xml_node_as_body(node, &data_cursor)) {
            return AWS_OP_ERR;
        }

        if (aws_byte_cursor_eq_c_str_ignore_case(&data_cursor, "IDPCommunicationError") ||
            aws_byte_cursor_eq_c_str_ignore_case(&data_cursor, "InvalidIdentityToken")) {
            *get_retryable_error = true;
        }
    }

    return AWS_OP_SUCCESS;
}

static bool s_parse_retryable_error_from_response(struct aws_allocator *allocator, struct aws_byte_buf *response) {

    bool get_retryable_error = false;
    struct aws_xml_parser_options options = {
        .doc = aws_byte_cursor_from_buf(response),
        .on_root_encountered = s_stswebid_error_xml_on_root,
        .user_data = &get_retryable_error,
    };

    if (aws_xml_parse(allocator, &options)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to parse xml error response for sts web identity with error %s",
            aws_error_str(aws_last_error()));
        return false;
    }

    return get_retryable_error;
}

static int s_stswebid_200_xml_on_root(struct aws_xml_node *node, void *user_data) {
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AssumeRoleWithWebIdentityResponse")) {
        return aws_xml_node_traverse(node, s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResponse_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResponse_child(
    struct aws_xml_node *node,

    void *user_data) {

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AssumeRoleWithWebIdentityResult")) {
        return aws_xml_node_traverse(node, s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResult_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_stswebid_200_xml_on_AssumeRoleWithWebIdentityResult_child(
    struct aws_xml_node *node,

    void *user_data) {

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Credentials")) {
        return aws_xml_node_traverse(node, s_stswebid_200_xml_on_Credentials_child, user_data);
    }
    return AWS_OP_SUCCESS;
}

static int s_stswebid_200_xml_on_Credentials_child(struct aws_xml_node *node, void *user_data) {
    struct sts_web_identity_user_data *query_user_data = user_data;

    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    struct aws_byte_cursor credential_data;
    AWS_ZERO_STRUCT(credential_data);

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "AccessKeyId")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        query_user_data->access_key_id = aws_string_new_from_cursor(query_user_data->allocator, &credential_data);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SecretAccessKey")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        query_user_data->secret_access_key = aws_string_new_from_cursor(query_user_data->allocator, &credential_data);
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "SessionToken")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        query_user_data->session_token = aws_string_new_from_cursor(query_user_data->allocator, &credential_data);
    }

    /* As long as we parsed an usable expiration, use it, otherwise use
     * the existing one: now + 900s, initialized before parsing.
     */
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, "Expiration")) {
        if (aws_xml_node_as_body(node, &credential_data)) {
            return AWS_OP_ERR;
        }
        if (credential_data.len != 0) {
            struct aws_date_time expiration;
            if (aws_date_time_init_from_str_cursor(&expiration, &credential_data, AWS_DATE_FORMAT_ISO_8601) ==
                AWS_OP_SUCCESS) {
                query_user_data->expiration_timepoint_in_seconds = (uint64_t)aws_date_time_as_epoch_secs(&expiration);
            } else {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "Failed to parse time string from sts web identity xml response: %s",
                    aws_error_str(aws_last_error()));
                return AWS_OP_ERR;
            }
        }
    }

    return AWS_OP_SUCCESS;
}

static struct aws_credentials *s_parse_credentials_from_response(
    struct sts_web_identity_user_data *query_user_data,
    struct aws_byte_buf *response) {

    struct aws_credentials *credentials = NULL;

    if (!response || response->len == 0) {
        goto on_finish;
    }

    uint64_t now = UINT64_MAX;
    if (aws_sys_clock_get_ticks(&now) != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to get sys clock for sts web identity credentials provider to parse error information.");
        goto on_finish;
    }
    uint64_t now_seconds = aws_timestamp_convert(now, AWS_TIMESTAMP_NANOS, AWS_TIMESTAMP_SECS, NULL);
    query_user_data->expiration_timepoint_in_seconds = now_seconds + STS_WEB_IDENTITY_CREDS_DEFAULT_DURATION_SECONDS;

    struct aws_xml_parser_options options = {
        .doc = aws_byte_cursor_from_buf(response),
        .on_root_encountered = s_stswebid_200_xml_on_root,
        .user_data = query_user_data,
    };
    if (aws_xml_parse(query_user_data->allocator, &options)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to parse xml response for sts web identity with error: %s",
            aws_error_str(aws_last_error()));
        goto on_finish;
    }

    if (!query_user_data->access_key_id || !query_user_data->secret_access_key) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "STS web identity not found in XML response.");
        goto on_finish;
    }

    credentials = aws_credentials_new(
        query_user_data->allocator,
        aws_byte_cursor_from_string(query_user_data->access_key_id),
        aws_byte_cursor_from_string(query_user_data->secret_access_key),
        aws_byte_cursor_from_string(query_user_data->session_token),
        query_user_data->expiration_timepoint_in_seconds);

    if (credentials == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to create credentials for sts web identity");
        goto on_finish;
    }

on_finish:

    if (credentials == NULL) {
        /* Give a useful error (aws_last_error() might be AWS_ERROR_INVALID_ARGUMENT, which isn't too helpful) */
        query_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_STS_WEB_IDENTITY_SOURCE_FAILURE;
    }

    return credentials;
}

/*
 * No matter the result, this always gets called assuming that user_data is successfully allocated
 */
static void s_finalize_get_credentials_query(struct sts_web_identity_user_data *user_data) {
    /* Try to build credentials from whatever, if anything, was in the result */
    struct aws_credentials *credentials = NULL;
    if (user_data->status_code == AWS_HTTP_STATUS_CODE_200_OK) {
        credentials = s_parse_credentials_from_response(user_data, &user_data->response);
    }

    if (credentials != NULL) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) STS_WEB_IDENTITY credentials provider successfully queried credentials",
            (void *)user_data->sts_web_identity_provider);
    } else {
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) STS_WEB_IDENTITY credentials provider failed to query credentials",
            (void *)user_data->sts_web_identity_provider);

        if (user_data->error_code == AWS_ERROR_SUCCESS) {
            user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_STS_WEB_IDENTITY_SOURCE_FAILURE;
        }
    }

    /* pass the credentials back */
    user_data->original_callback(credentials, user_data->error_code, user_data->original_user_data);

    /* clean up */
    s_user_data_destroy(user_data);
    aws_credentials_release(credentials);
}

static int s_on_incoming_body_fn(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *body,
    void *wrapped_user_data) {

    (void)stream;

    struct sts_web_identity_user_data *user_data = wrapped_user_data;
    struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;

    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) STS_WEB_IDENTITY credentials provider received %zu response bytes",
        (void *)user_data->sts_web_identity_provider,
        body->len);

    if (body->len + user_data->response.len > STS_WEB_IDENTITY_RESPONSE_SIZE_LIMIT) {
        impl->function_table->aws_http_connection_close(user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) STS_WEB_IDENTITY credentials provider query response exceeded maximum allowed length",
            (void *)user_data->sts_web_identity_provider);

        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (aws_byte_buf_append_dynamic(&user_data->response, body)) {
        impl->function_table->aws_http_connection_close(user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) STS_WEB_IDENTITY credentials provider query error appending response: %s",
            (void *)user_data->sts_web_identity_provider,
            aws_error_str(aws_last_error()));

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_on_incoming_headers_fn(
    struct aws_http_stream *stream,
    enum aws_http_header_block header_block,
    const struct aws_http_header *header_array,
    size_t num_headers,
    void *wrapped_user_data) {

    (void)header_array;
    (void)num_headers;

    if (header_block != AWS_HTTP_HEADER_BLOCK_MAIN) {
        return AWS_OP_SUCCESS;
    }

    struct sts_web_identity_user_data *user_data = wrapped_user_data;
    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        if (user_data->status_code == 0) {
            struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;
            if (impl->function_table->aws_http_stream_get_incoming_response_status(stream, &user_data->status_code)) {
                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "(id=%p) STS_WEB_IDENTITY credentials provider failed to get http status code: %s",
                    (void *)user_data->sts_web_identity_provider,
                    aws_error_str(aws_last_error()));

                return AWS_OP_ERR;
            }
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) STS_WEB_IDENTITY credentials provider query received http status code %d",
                (void *)user_data->sts_web_identity_provider,
                user_data->status_code);
        }
    }

    return AWS_OP_SUCCESS;
}

static void s_query_credentials(struct sts_web_identity_user_data *user_data);

static void s_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *data) {
    struct sts_web_identity_user_data *user_data = data;

    struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;
    struct aws_http_connection *connection = impl->function_table->aws_http_stream_get_connection(stream);
    impl->function_table->aws_http_stream_release(stream);
    impl->function_table->aws_http_connection_manager_release_connection(impl->connection_manager, connection);

    /*
     * On anything other than a 200, if we can retry the request based on
     * error response, retry it, otherwise, call the finalize function.
     */
    if (user_data->status_code != AWS_HTTP_STATUS_CODE_200_OK || error_code != AWS_OP_SUCCESS) {
        if (++user_data->attempt_count < STS_WEB_IDENTITY_MAX_ATTEMPTS && user_data->response.len) {
            if (s_parse_retryable_error_from_response(user_data->allocator, &user_data->response)) {
                s_query_credentials(user_data);
                return;
            }
        }
    }

    s_finalize_get_credentials_query(user_data);
}

static struct aws_http_header s_content_type_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("content-type"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("application/x-www-form-urlencoded"),
};

static struct aws_http_header s_api_version_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-api-version"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("2011-06-15"),
};
static struct aws_http_header s_accept_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Accept"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("*/*"),
};

static struct aws_http_header s_user_agent_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("User-Agent"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("aws-sdk-crt/sts-web-identity-credentials-provider"),
};

static struct aws_http_header s_keep_alive_header = {
    .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Connection"),
    .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("keep-alive"),
};

static struct aws_byte_cursor s_content_length = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("content-length");
static struct aws_byte_cursor s_path = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/");

static int s_make_sts_web_identity_http_query(
    struct sts_web_identity_user_data *user_data,
    struct aws_byte_cursor *body_cursor) {
    AWS_FATAL_ASSERT(user_data->connection);

    struct aws_http_stream *stream = NULL;
    struct aws_input_stream *input_stream = NULL;
    struct aws_http_message *request = aws_http_message_new_request(user_data->allocator);
    if (request == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;

    char content_length[21];
    AWS_ZERO_ARRAY(content_length);
    snprintf(content_length, sizeof(content_length), "%" PRIu64, (uint64_t)body_cursor->len);

    struct aws_http_header content_len_header = {
        .name = s_content_length,
        .value = aws_byte_cursor_from_c_str(content_length),
    };

    if (aws_http_message_add_header(request, content_len_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_content_type_header)) {
        goto on_error;
    }

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_c_str("Host"),
        .value = aws_byte_cursor_from_string(impl->endpoint),
    };

    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_api_version_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_accept_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_user_agent_header)) {
        goto on_error;
    }

    if (aws_http_message_add_header(request, s_keep_alive_header)) {
        goto on_error;
    }

    input_stream = aws_input_stream_new_from_cursor(user_data->allocator, body_cursor);
    if (!input_stream) {
        goto on_error;
    }

    aws_http_message_set_body_stream(request, input_stream);

    if (aws_http_message_set_request_path(request, s_path)) {
        goto on_error;
    }

    if (aws_http_message_set_request_method(request, aws_http_method_post)) {
        goto on_error;
    }

    user_data->request = request;

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .on_response_headers = s_on_incoming_headers_fn,
        .on_response_header_block_done = NULL,
        .on_response_body = s_on_incoming_body_fn,
        .on_complete = s_on_stream_complete_fn,
        .user_data = user_data,
        .request = request,
    };

    stream = impl->function_table->aws_http_connection_make_request(user_data->connection, &request_options);

    if (!stream) {
        goto on_error;
    }

    if (impl->function_table->aws_http_stream_activate(stream)) {
        goto on_error;
    }

    return AWS_OP_SUCCESS;

on_error:
    impl->function_table->aws_http_stream_release(stream);
    aws_input_stream_destroy(input_stream);
    aws_http_message_destroy(request);
    user_data->request = NULL;
    return AWS_OP_ERR;
}

static void s_query_credentials(struct sts_web_identity_user_data *user_data) {
    AWS_FATAL_ASSERT(user_data->connection);

    struct aws_credentials_provider_sts_web_identity_impl *impl = user_data->sts_web_identity_provider->impl;

    /* "Clear" the result */
    s_user_data_reset_request_and_response(user_data);

    /*
     * Calculate body message:
     * "Action=AssumeRoleWithWebIdentity"
     * + "&Version=2011-06-15"
     * + "&RoleSessionName=" + url_encode(role_session_name)
     * + "&RoleArn=" + url_encode(role_arn)
     * + "&WebIdentityToken=" + url_encode(token);
     */
    struct aws_byte_buf token_buf;
    bool success = false;

    AWS_ZERO_STRUCT(token_buf);

    struct aws_byte_cursor work_cursor =
        aws_byte_cursor_from_c_str("Action=AssumeRoleWithWebIdentity&Version=2011-06-15&RoleArn=");
    if (aws_byte_buf_append_dynamic(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }

    work_cursor = aws_byte_cursor_from_string(impl->role_arn);
    if (aws_byte_buf_append_encoding_uri_param(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }

    work_cursor = aws_byte_cursor_from_c_str("&RoleSessionName=");
    if (aws_byte_buf_append_dynamic(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }

    work_cursor = aws_byte_cursor_from_string(impl->role_session_name);
    if (aws_byte_buf_append_encoding_uri_param(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }

    work_cursor = aws_byte_cursor_from_c_str("&WebIdentityToken=");
    if (aws_byte_buf_append_dynamic(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }

    if (aws_byte_buf_init_from_file(&token_buf, user_data->allocator, aws_string_c_str(impl->token_file_path))) {
        goto on_finish;
    }
    work_cursor = aws_byte_cursor_from_buf(&token_buf);
    if (aws_byte_buf_append_encoding_uri_param(&user_data->payload_buf, &work_cursor)) {
        goto on_finish;
    }
    struct aws_byte_cursor body_cursor = aws_byte_cursor_from_buf(&user_data->payload_buf);

    if (s_make_sts_web_identity_http_query(user_data, &body_cursor) == AWS_OP_ERR) {
        goto on_finish;
    }
    success = true;

on_finish:
    aws_byte_buf_clean_up(&token_buf);
    if (!success) {
        s_finalize_get_credentials_query(user_data);
    }
}

static void s_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *data) {
    struct sts_web_identity_user_data *user_data = data;

    if (connection == NULL) {
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: STS_WEB_IDENTITY provider failed to acquire a connection, error code %d(%s)",
            (void *)user_data->sts_web_identity_provider,
            error_code,
            aws_error_str(error_code));

        s_finalize_get_credentials_query(user_data);
        return;
    }

    user_data->connection = connection;

    s_query_credentials(user_data);
}

static int s_credentials_provider_sts_web_identity_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_sts_web_identity_impl *impl = provider->impl;

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "id=%p: STS_WEB_IDENTITY provider trying to load credentials",
        (void *)provider);

    struct sts_web_identity_user_data *wrapped_user_data = s_user_data_new(provider, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto error;
    }

    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_on_acquire_connection, wrapped_user_data);

    return AWS_OP_SUCCESS;

error:
    s_user_data_destroy(wrapped_user_data);
    return AWS_OP_ERR;
}

static void s_credentials_provider_sts_web_identity_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_sts_web_identity_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->role_arn);
    aws_string_destroy(impl->role_session_name);
    aws_string_destroy(impl->token_file_path);
    aws_string_destroy(impl->endpoint);

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

    /* freeing the provider takes place in the shutdown callback below */
}

static struct aws_credentials_provider_vtable s_aws_credentials_provider_sts_web_identity_vtable = {
    .get_credentials = s_credentials_provider_sts_web_identity_get_credentials_async,
    .destroy = s_credentials_provider_sts_web_identity_destroy,
};

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;

    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

AWS_STATIC_STRING_FROM_LITERAL(s_region_config, "region");
AWS_STATIC_STRING_FROM_LITERAL(s_role_arn_config, "role_arn");
AWS_STATIC_STRING_FROM_LITERAL(s_role_arn_env, "AWS_ROLE_ARN");
AWS_STATIC_STRING_FROM_LITERAL(s_role_session_name_config, "role_session_name");
AWS_STATIC_STRING_FROM_LITERAL(s_role_session_name_env, "AWS_ROLE_SESSION_NAME");
AWS_STATIC_STRING_FROM_LITERAL(s_token_file_path_config, "web_identity_token_file");
AWS_STATIC_STRING_FROM_LITERAL(s_token_file_path_env, "AWS_WEB_IDENTITY_TOKEN_FILE");

struct sts_web_identity_parameters {
    struct aws_allocator *allocator;
    /* region is actually used to construct endpoint */
    struct aws_string *endpoint;
    struct aws_byte_buf role_arn;
    struct aws_byte_buf role_session_name;
    struct aws_byte_buf token_file_path;
};

struct aws_profile_collection *s_load_profile(struct aws_allocator *allocator) {

    struct aws_profile_collection *config_profiles = NULL;
    struct aws_string *config_file_path = NULL;

    config_file_path = aws_get_config_file_path(allocator, NULL);
    if (!config_file_path) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve config file path during sts web identity provider initialization: %s",
            aws_error_str(aws_last_error()));
        goto on_error;
    }

    config_profiles = aws_profile_collection_new_from_file(allocator, config_file_path, AWS_PST_CONFIG);
    if (config_profiles != NULL) {
        AWS_LOGF_DEBUG(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Successfully built config profile collection from file at (%s)",
            aws_string_c_str(config_file_path));
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to build config profile collection from file at (%s) : %s",
            aws_string_c_str(config_file_path),
            aws_error_str(aws_last_error()));
        goto on_error;
    }

    aws_string_destroy(config_file_path);
    return config_profiles;

on_error:
    aws_string_destroy(config_file_path);
    aws_profile_collection_destroy(config_profiles);
    return NULL;
}

AWS_STATIC_STRING_FROM_LITERAL(s_sts_service_name, "sts");

static int s_generate_uuid_to_buf(struct aws_allocator *allocator, struct aws_byte_buf *dst) {

    if (!allocator || !dst) {
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_uuid uuid;
    if (aws_uuid_init(&uuid)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to initiate an uuid struct: %s", aws_error_str(aws_last_error()));
        return aws_last_error();
    }

    char uuid_str[AWS_UUID_STR_LEN] = {0};
    struct aws_byte_buf uuid_buf = aws_byte_buf_from_array(uuid_str, sizeof(uuid_str));
    uuid_buf.len = 0;
    if (aws_uuid_to_str(&uuid, &uuid_buf)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to stringify uuid: %s", aws_error_str(aws_last_error()));
        return aws_last_error();
    }
    if (aws_byte_buf_init_copy(dst, allocator, &uuid_buf)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to generate role session name during sts web identity provider initialization: %s",
            aws_error_str(aws_last_error()));
        return aws_last_error();
    }
    return AWS_OP_SUCCESS;
}

static struct aws_string *s_check_or_get_with_env(
    struct aws_allocator *allocator,
    const struct aws_string *env_key,
    struct aws_byte_cursor option) {

    AWS_ASSERT(allocator);
    struct aws_string *out = NULL;

    if (option.len) {
        out = aws_string_new_from_cursor(allocator, &option);
    } else {
        aws_get_environment_value(allocator, env_key, &out);
    }

    return out;
}

static void s_check_or_get_with_profile_config(
    struct aws_allocator *allocator,
    const struct aws_profile *profile,
    struct aws_string **target,
    const struct aws_string *config_key) {

    if (!allocator || !profile || !config_key) {
        return;
    }
    if ((!(*target) || !(*target)->len)) {
        if (*target) {
            aws_string_destroy(*target);
            *target = NULL;
        }
        const struct aws_profile_property *property = aws_profile_get_property(profile, config_key);
        if (property) {
            *target = aws_string_new_from_string(allocator, aws_profile_property_get_value(property));
        }
    }
}

static void s_parameters_destroy(struct sts_web_identity_parameters *parameters) {
    if (!parameters) {
        return;
    }
    aws_string_destroy(parameters->endpoint);
    aws_byte_buf_clean_up(&parameters->role_arn);
    aws_byte_buf_clean_up(&parameters->role_session_name);
    aws_byte_buf_clean_up(&parameters->token_file_path);
    aws_mem_release(parameters->allocator, parameters);
}

static struct sts_web_identity_parameters *s_parameters_new(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sts_web_identity_options *options) {

    struct sts_web_identity_parameters *parameters =
        aws_mem_calloc(allocator, 1, sizeof(struct sts_web_identity_parameters));
    if (parameters == NULL) {
        return NULL;
    }
    parameters->allocator = allocator;

    bool success = false;
    struct aws_string *region = NULL;
    if (options->region.len > 0) {
        region = aws_string_new_from_cursor(allocator, &options->region);
    } else {
        region = aws_credentials_provider_resolve_region_from_env(allocator);
    }

    struct aws_string *role_arn = s_check_or_get_with_env(allocator, s_role_arn_env, options->role_arn);
    struct aws_string *role_session_name =
        s_check_or_get_with_env(allocator, s_role_session_name_env, options->role_session_name);
    struct aws_string *token_file_path =
        s_check_or_get_with_env(allocator, s_token_file_path_env, options->token_file_path);
    ;

    /**
     * check config profile if either region, role_arn or token_file_path or role_session_name is not resolved from
     * environment variable. Role session name can also be generated by us using uuid if not found from both
     * sources.
     */
    struct aws_profile_collection *config_profile = NULL;
    struct aws_string *profile_name = NULL;
    const struct aws_profile *profile = NULL;
    bool get_all_parameters =
        (region && region->len && role_arn && role_arn->len && token_file_path && token_file_path->len);
    if (!get_all_parameters) {
        if (options->config_profile_collection_cached) {
            /* Use cached profile collection */
            config_profile = aws_profile_collection_acquire(options->config_profile_collection_cached);
        } else {
            /* Load profile collection from files */
            config_profile = s_load_profile(allocator);
            if (!config_profile) {
                goto on_finish;
            }
        }

        profile_name = aws_get_profile_name(allocator, &options->profile_name_override);
        profile = aws_profile_collection_get_profile(config_profile, profile_name);

        if (!profile) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "Failed to resolve either region, role arn or token file path during sts web identity provider "
                "initialization.");
            goto on_finish;

        } else {
            s_check_or_get_with_profile_config(allocator, profile, &region, s_region_config);
            s_check_or_get_with_profile_config(allocator, profile, &role_arn, s_role_arn_config);
            s_check_or_get_with_profile_config(allocator, profile, &role_session_name, s_role_session_name_config);
            s_check_or_get_with_profile_config(allocator, profile, &token_file_path, s_token_file_path_config);
        }
    }

    /* determine endpoint */
    if (aws_credentials_provider_construct_regional_endpoint(
            allocator, &parameters->endpoint, region, s_sts_service_name)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "Failed to construct sts endpoint with, probably region is missing.");
        goto on_finish;
    }

    /* determine role_arn */
    if (!role_arn || !role_arn->len ||
        aws_byte_buf_init_copy_from_cursor(&parameters->role_arn, allocator, aws_byte_cursor_from_string(role_arn))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve role arn during sts web identity provider initialization.");
        goto on_finish;
    }

    /* determine token_file_path */
    if (!token_file_path || !token_file_path->len ||
        aws_byte_buf_init_copy_from_cursor(
            &parameters->token_file_path, allocator, aws_byte_cursor_from_string(token_file_path))) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "Failed to resolve token file path during sts web identity provider initialization.");
        goto on_finish;
    }

    /* determine role_session_name */
    if (role_session_name && role_session_name->len) {
        if (aws_byte_buf_init_copy_from_cursor(
                &parameters->role_session_name, allocator, aws_byte_cursor_from_string(role_session_name))) {
            goto on_finish;
        }
    } else if (s_generate_uuid_to_buf(allocator, &parameters->role_session_name)) {
        goto on_finish;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "Successfully loaded all required parameters for sts web identity credentials provider.");
    success = true;

on_finish:
    aws_string_destroy(region);
    aws_string_destroy(role_arn);
    aws_string_destroy(role_session_name);
    aws_string_destroy(token_file_path);
    aws_string_destroy(profile_name);
    aws_profile_collection_release(config_profile);
    if (!success) {
        s_parameters_destroy(parameters);
        parameters = NULL;
    }
    return parameters;
}

struct aws_credentials_provider *aws_credentials_provider_new_sts_web_identity(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_sts_web_identity_options *options) {

    struct sts_web_identity_parameters *parameters = s_parameters_new(allocator, options);
    if (!parameters) {
        return NULL;
    }

    struct aws_tls_connection_options tls_connection_options;
    AWS_ZERO_STRUCT(tls_connection_options);

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_sts_web_identity_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_sts_web_identity_impl));

    if (!provider) {
        goto on_error;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_sts_web_identity_vtable, impl);

    if (!options->tls_ctx) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "a TLS context must be provided to the STS web identity credentials provider");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
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
        STS_WEB_IDENTITY_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = STS_WEB_IDENTITY_RESPONSE_SIZE_LIMIT;
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

    impl->role_arn = aws_string_new_from_array(allocator, parameters->role_arn.buffer, parameters->role_arn.len);
    if (impl->role_arn == NULL) {
        goto on_error;
    }

    impl->role_session_name =
        aws_string_new_from_array(allocator, parameters->role_session_name.buffer, parameters->role_session_name.len);
    if (impl->role_session_name == NULL) {
        goto on_error;
    }

    impl->token_file_path =
        aws_string_new_from_array(allocator, parameters->token_file_path.buffer, parameters->token_file_path.len);
    if (impl->token_file_path == NULL) {
        goto on_error;
    }

    impl->endpoint = aws_string_new_from_string(allocator, parameters->endpoint);
    if (impl->endpoint == NULL) {
        goto on_error;
    }

    provider->shutdown_options = options->shutdown_options;
    s_parameters_destroy(parameters);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    return provider;

on_error:

    aws_credentials_provider_destroy(provider);
    s_parameters_destroy(parameters);
    aws_tls_connection_options_clean_up(&tls_connection_options);
    return NULL;
}
