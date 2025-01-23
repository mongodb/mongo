/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/common/byte_buf.h"
#include <aws/auth/credentials.h>

#include <aws/auth/private/credentials_utils.h>
#include <aws/common/clock.h>
#include <aws/common/date_time.h>
#include <aws/common/environment.h>
#include <aws/common/host_utils.h>
#include <aws/common/string.h>
#include <aws/http/connection.h>
#include <aws/http/connection_manager.h>
#include <aws/http/request_response.h>
#include <aws/http/status_code.h>
#include <aws/io/channel_bootstrap.h>
#include <aws/io/host_resolver.h>
#include <aws/io/logging.h>
#include <aws/io/socket.h>
#include <aws/io/tls_channel_handler.h>
#include <aws/io/uri.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#    pragma warning(disable : 4232)
#endif /* _MSC_VER */

/* ecs task role credentials body response is currently ~ 1300 characters + name length */
#define ECS_RESPONSE_SIZE_INITIAL 2048
#define ECS_RESPONSE_SIZE_LIMIT 10000
#define ECS_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS 2

AWS_STATIC_STRING_FROM_LITERAL(s_ecs_creds_env_token_file, "AWS_CONTAINER_AUTHORIZATION_TOKEN_FILE");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_creds_env_token, "AWS_CONTAINER_AUTHORIZATION_TOKEN");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_creds_env_relative_uri, "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_creds_env_full_uri, "AWS_CONTAINER_CREDENTIALS_FULL_URI");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_host, "169.254.170.2");

static void s_on_connection_manager_shutdown(void *user_data);

struct aws_credentials_provider_ecs_impl {
    struct aws_http_connection_manager *connection_manager;
    const struct aws_auth_http_system_vtable *function_table;
    struct aws_string *host;
    struct aws_string *path_and_query;
    struct aws_string *auth_token_file_path;
    struct aws_string *auth_token;
    struct aws_client_bootstrap *bootstrap;
    bool is_https;
};

/*
 * Tracking structure for each outstanding async query to an ecs provider
 */
struct aws_credentials_provider_ecs_user_data {
    /* immutable post-creation */
    struct aws_allocator *allocator;
    struct aws_credentials_provider *ecs_provider;
    aws_on_get_credentials_callback_fn *original_callback;
    void *original_user_data;
    struct aws_byte_buf auth_token;

    /* mutable */
    struct aws_http_connection *connection;
    struct aws_http_message *request;
    struct aws_byte_buf current_result;
    int status_code;
    int error_code;
};

static void s_aws_credentials_provider_ecs_user_data_destroy(struct aws_credentials_provider_ecs_user_data *user_data) {
    if (user_data == NULL) {
        return;
    }

    struct aws_credentials_provider_ecs_impl *impl = user_data->ecs_provider->impl;

    if (user_data->connection) {
        impl->function_table->aws_http_connection_manager_release_connection(
            impl->connection_manager, user_data->connection);
    }

    aws_byte_buf_clean_up(&user_data->auth_token);
    aws_byte_buf_clean_up(&user_data->current_result);

    if (user_data->request) {
        aws_http_message_destroy(user_data->request);
    }
    aws_credentials_provider_release(user_data->ecs_provider);
    aws_mem_release(user_data->allocator, user_data);
}

static struct aws_credentials_provider_ecs_user_data *s_aws_credentials_provider_ecs_user_data_new(
    struct aws_credentials_provider *ecs_provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    struct aws_credentials_provider_ecs_user_data *wrapped_user_data =
        aws_mem_calloc(ecs_provider->allocator, 1, sizeof(struct aws_credentials_provider_ecs_user_data));

    wrapped_user_data->allocator = ecs_provider->allocator;
    wrapped_user_data->ecs_provider = ecs_provider;
    aws_credentials_provider_acquire(ecs_provider);
    wrapped_user_data->original_user_data = user_data;
    wrapped_user_data->original_callback = callback;

    if (aws_byte_buf_init(&wrapped_user_data->current_result, ecs_provider->allocator, ECS_RESPONSE_SIZE_INITIAL)) {
        goto on_error;
    }

    struct aws_credentials_provider_ecs_impl *impl = ecs_provider->impl;
    if (impl->auth_token_file_path != NULL && impl->auth_token_file_path->len > 0) {
        if (aws_byte_buf_init_from_file(
                &wrapped_user_data->auth_token,
                ecs_provider->allocator,
                aws_string_c_str(impl->auth_token_file_path))) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) ECS credentials provider failed to read token from the path: %s with error: %d",
                (void *)ecs_provider,
                aws_string_c_str(impl->auth_token_file_path),
                aws_last_error());
            aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_TOKEN_FILE_PATH);
            goto on_error;
        }
    } else if (impl->auth_token != NULL && impl->auth_token->len > 0) {
        if (aws_byte_buf_init_copy_from_cursor(
                &wrapped_user_data->auth_token,
                ecs_provider->allocator,
                aws_byte_cursor_from_string(impl->auth_token))) {
            goto on_error;
        }
    }

    return wrapped_user_data;
on_error:
    s_aws_credentials_provider_ecs_user_data_destroy(wrapped_user_data);
    return NULL;
}

static void s_aws_credentials_provider_ecs_user_data_reset_response(
    struct aws_credentials_provider_ecs_user_data *ecs_user_data) {
    ecs_user_data->current_result.len = 0;
    ecs_user_data->status_code = 0;

    if (ecs_user_data->request) {
        aws_http_message_destroy(ecs_user_data->request);
        ecs_user_data->request = NULL;
    }
}

/*
 * In general, the ECS document looks something like:
 {
  "Code" : "Success",
  "LastUpdated" : "2019-05-28T18:03:09Z",
  "Type" : "AWS-HMAC",
  "AccessKeyId" : "...",
  "SecretAccessKey" : "...",
  "Token" : "...",
  "Expiration" : "2019-05-29T00:21:43Z"
 }
 *
 * No matter the result, this always gets called assuming that esc_user_data is successfully allocated
 */
static void s_ecs_finalize_get_credentials_query(struct aws_credentials_provider_ecs_user_data *ecs_user_data) {
    /* Try to build credentials from whatever, if anything, was in the result */
    struct aws_credentials *credentials = NULL;
    struct aws_parse_credentials_from_json_doc_options parse_options = {
        .access_key_id_name = "AccessKeyId",
        .secret_access_key_name = "SecretAccessKey",
        .token_name = "Token",
        .expiration_name = "Expiration",
        .token_required = true,
        .expiration_required = true,
    };
    if (aws_byte_buf_append_null_terminator(&ecs_user_data->current_result) == AWS_OP_SUCCESS) {
        credentials = aws_parse_credentials_from_json_document(
            ecs_user_data->allocator, aws_byte_cursor_from_buf(&ecs_user_data->current_result), &parse_options);
    } else {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) ECS credentials provider failed to add null terminating char to resulting buffer.",
            (void *)ecs_user_data->ecs_provider);
    }

    if (credentials != NULL) {
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) ECS credentials provider successfully queried instance role credentials",
            (void *)ecs_user_data->ecs_provider);
    } else {
        /* no credentials, make sure we have a valid error to report */
        if (ecs_user_data->error_code == AWS_ERROR_SUCCESS) {
            ecs_user_data->error_code = aws_last_error();
            if (ecs_user_data->error_code == AWS_ERROR_SUCCESS) {
                ecs_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_ECS_SOURCE_FAILURE;
            }
        }
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) ECS credentials provider failed to query instance role credentials with error %d(%s)",
            (void *)ecs_user_data->ecs_provider,
            ecs_user_data->error_code,
            aws_error_str(ecs_user_data->error_code));
    }

    /* pass the credentials back */
    ecs_user_data->original_callback(credentials, ecs_user_data->error_code, ecs_user_data->original_user_data);

    /* clean up */
    s_aws_credentials_provider_ecs_user_data_destroy(ecs_user_data);
    aws_credentials_release(credentials);
}

static int s_ecs_on_incoming_body_fn(
    struct aws_http_stream *stream,
    const struct aws_byte_cursor *data,
    void *user_data) {

    (void)stream;

    struct aws_credentials_provider_ecs_user_data *ecs_user_data = user_data;
    struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;

    AWS_LOGF_TRACE(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "(id=%p) ECS credentials provider received %zu response bytes",
        (void *)ecs_user_data->ecs_provider,
        data->len);

    if (data->len + ecs_user_data->current_result.len > ECS_RESPONSE_SIZE_LIMIT) {
        impl->function_table->aws_http_connection_close(ecs_user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) ECS credentials provider query response exceeded maximum allowed length",
            (void *)ecs_user_data->ecs_provider);

        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    if (aws_byte_buf_append_dynamic(&ecs_user_data->current_result, data)) {
        impl->function_table->aws_http_connection_close(ecs_user_data->connection);
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "(id=%p) ECS credentials provider query error appending response",
            (void *)ecs_user_data->ecs_provider);

        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_ecs_on_incoming_headers_fn(
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

    struct aws_credentials_provider_ecs_user_data *ecs_user_data = user_data;
    if (header_block == AWS_HTTP_HEADER_BLOCK_MAIN) {
        if (ecs_user_data->status_code == 0) {
            struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;
            if (impl->function_table->aws_http_stream_get_incoming_response_status(
                    stream, &ecs_user_data->status_code)) {

                AWS_LOGF_ERROR(
                    AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                    "(id=%p) ECS credentials provider failed to get http status code",
                    (void *)ecs_user_data->ecs_provider);

                return AWS_OP_ERR;
            }
            AWS_LOGF_DEBUG(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p) ECS credentials provider query received http status code %d",
                (void *)ecs_user_data->ecs_provider,
                ecs_user_data->status_code);
        }
    }

    return AWS_OP_SUCCESS;
}

static void s_ecs_query_task_role_credentials(struct aws_credentials_provider_ecs_user_data *ecs_user_data);

static void s_ecs_on_stream_complete_fn(struct aws_http_stream *stream, int error_code, void *user_data) {
    struct aws_credentials_provider_ecs_user_data *ecs_user_data = user_data;

    aws_http_message_destroy(ecs_user_data->request);
    ecs_user_data->request = NULL;

    struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;
    impl->function_table->aws_http_stream_release(stream);

    /*
     * On anything other than a 200, nullify the response and pretend there was
     * an error
     */
    if (ecs_user_data->status_code != AWS_HTTP_STATUS_CODE_200_OK || error_code != AWS_OP_SUCCESS) {
        ecs_user_data->current_result.len = 0;

        if (error_code != AWS_OP_SUCCESS) {
            ecs_user_data->error_code = error_code;
        } else {
            ecs_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_HTTP_STATUS_FAILURE;
        }
    }

    s_ecs_finalize_get_credentials_query(ecs_user_data);
}

AWS_STATIC_STRING_FROM_LITERAL(s_ecs_accept_header, "Accept");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_accept_header_value, "application/json");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_user_agent_header, "User-Agent");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_user_agent_header_value, "aws-sdk-crt/ecs-credentials-provider");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_authorization_header, "Authorization");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_accept_encoding_header, "Accept-Encoding");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_accept_encoding_header_value, "identity");
AWS_STATIC_STRING_FROM_LITERAL(s_ecs_host_header, "Host");

static int s_make_ecs_http_query(
    struct aws_credentials_provider_ecs_user_data *ecs_user_data,
    struct aws_byte_cursor *uri) {
    AWS_FATAL_ASSERT(ecs_user_data->connection);

    struct aws_http_stream *stream = NULL;
    struct aws_http_message *request = aws_http_message_new_request(ecs_user_data->allocator);
    if (request == NULL) {
        return AWS_OP_ERR;
    }

    struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;

    struct aws_http_header host_header = {
        .name = aws_byte_cursor_from_string(s_ecs_host_header),
        .value = aws_byte_cursor_from_string(impl->host),
    };
    if (aws_http_message_add_header(request, host_header)) {
        goto on_error;
    }

    if (ecs_user_data->auth_token.len) {
        struct aws_http_header auth_header = {
            .name = aws_byte_cursor_from_string(s_ecs_authorization_header),
            .value = aws_byte_cursor_from_buf(&ecs_user_data->auth_token),
        };
        if (aws_http_message_add_header(request, auth_header)) {
            goto on_error;
        }
    }

    struct aws_http_header accept_header = {
        .name = aws_byte_cursor_from_string(s_ecs_accept_header),
        .value = aws_byte_cursor_from_string(s_ecs_accept_header_value),
    };
    if (aws_http_message_add_header(request, accept_header)) {
        goto on_error;
    }

    struct aws_http_header accept_encoding_header = {
        .name = aws_byte_cursor_from_string(s_ecs_accept_encoding_header),
        .value = aws_byte_cursor_from_string(s_ecs_accept_encoding_header_value),
    };
    if (aws_http_message_add_header(request, accept_encoding_header)) {
        goto on_error;
    }

    struct aws_http_header user_agent_header = {
        .name = aws_byte_cursor_from_string(s_ecs_user_agent_header),
        .value = aws_byte_cursor_from_string(s_ecs_user_agent_header_value),
    };
    if (aws_http_message_add_header(request, user_agent_header)) {
        goto on_error;
    }

    if (aws_http_message_set_request_path(request, *uri)) {
        goto on_error;
    }

    if (aws_http_message_set_request_method(request, aws_byte_cursor_from_c_str("GET"))) {
        goto on_error;
    }

    ecs_user_data->request = request;

    struct aws_http_make_request_options request_options = {
        .self_size = sizeof(request_options),
        .on_response_headers = s_ecs_on_incoming_headers_fn,
        .on_response_header_block_done = NULL,
        .on_response_body = s_ecs_on_incoming_body_fn,
        .on_complete = s_ecs_on_stream_complete_fn,
        .user_data = ecs_user_data,
        .request = request,
    };
    /* for test with mocking http stack where make request finishes
      immediately and releases client before stream activate call */
    struct aws_credentials_provider *provider = ecs_user_data->ecs_provider;
    aws_credentials_provider_acquire(provider);
    stream = impl->function_table->aws_http_connection_make_request(ecs_user_data->connection, &request_options);

    if (!stream) {
        goto on_error;
    }

    if (impl->function_table->aws_http_stream_activate(stream)) {
        goto on_error;
    }
    aws_credentials_provider_release(provider);

    return AWS_OP_SUCCESS;

on_error:
    impl->function_table->aws_http_stream_release(stream);
    aws_http_message_destroy(request);
    ecs_user_data->request = NULL;
    return AWS_OP_ERR;
}

static void s_ecs_query_task_role_credentials(struct aws_credentials_provider_ecs_user_data *ecs_user_data) {
    AWS_FATAL_ASSERT(ecs_user_data->connection);

    struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;

    /* "Clear" the result */
    s_aws_credentials_provider_ecs_user_data_reset_response(ecs_user_data);

    struct aws_byte_cursor uri_cursor = aws_byte_cursor_from_string(impl->path_and_query);
    if (s_make_ecs_http_query(ecs_user_data, &uri_cursor) == AWS_OP_ERR) {
        s_ecs_finalize_get_credentials_query(ecs_user_data);
    }
}

static void s_ecs_on_acquire_connection(struct aws_http_connection *connection, int error_code, void *user_data) {
    struct aws_credentials_provider_ecs_user_data *ecs_user_data = user_data;

    if (connection == NULL) {
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: ECS provider failed to acquire a connection, error code %d(%s)",
            (void *)ecs_user_data->ecs_provider,
            error_code,
            aws_error_str(error_code));

        ecs_user_data->error_code = error_code;
        s_ecs_finalize_get_credentials_query(ecs_user_data);
        return;
    }

    ecs_user_data->connection = connection;

    s_ecs_query_task_role_credentials(ecs_user_data);
}

/*
 * The resolved IP address must satisfy one of the following:
 * 1. within the loopback CIDR (IPv4 127.0.0.0/8, IPv6 ::1/128)
 * 2. corresponds to the ECS container host 169.254.170.2
 * 3. corresponds to the EKS container host IPs (IPv4 169.254.170.23, IPv6 fd00:ec2::23)
 */
static bool s_is_valid_remote_host_ip(const struct aws_host_address *host_address) {
    bool result = false;
    struct aws_byte_cursor address = aws_byte_cursor_from_string(host_address->address);
    if (host_address->record_type == AWS_ADDRESS_RECORD_TYPE_A) {
        const struct aws_byte_cursor ipv4_loopback_address_prefix = aws_byte_cursor_from_c_str("127.");
        const struct aws_byte_cursor ecs_container_host_address = aws_byte_cursor_from_c_str("169.254.170.2");
        const struct aws_byte_cursor eks_container_host_address = aws_byte_cursor_from_c_str("169.254.170.23");

        result |= aws_byte_cursor_starts_with(&address, &ipv4_loopback_address_prefix);
        result |= aws_byte_cursor_eq(&address, &ecs_container_host_address);
        result |= aws_byte_cursor_eq(&address, &eks_container_host_address);

    } else if (host_address->record_type == AWS_ADDRESS_RECORD_TYPE_AAAA) {
        /* Check for both the short form and long form of an IPv6 address to be safe. */
        const struct aws_byte_cursor ipv6_loopback_address = aws_byte_cursor_from_c_str("::1");
        const struct aws_byte_cursor ipv6_loopback_address_verbose = aws_byte_cursor_from_c_str("0:0:0:0:0:0:0:1");
        const struct aws_byte_cursor eks_container_host_ipv6_address = aws_byte_cursor_from_c_str("fd00:ec2::23");
        const struct aws_byte_cursor eks_container_host_ipv6_address_verbose =
            aws_byte_cursor_from_c_str("fd00:ec2:0:0:0:0:0:23");

        result |= aws_byte_cursor_eq(&address, &ipv6_loopback_address);
        result |= aws_byte_cursor_eq(&address, &ipv6_loopback_address_verbose);
        result |= aws_byte_cursor_eq(&address, &eks_container_host_ipv6_address);
        result |= aws_byte_cursor_eq(&address, &eks_container_host_ipv6_address_verbose);
    }

    return result;
}

static void s_on_host_resolved(
    struct aws_host_resolver *resolver,
    const struct aws_string *host_name,
    int error_code,
    const struct aws_array_list *host_addresses,
    void *user_data) {
    (void)resolver;
    (void)host_name;

    struct aws_credentials_provider_ecs_user_data *ecs_user_data = user_data;
    if (error_code) {
        AWS_LOGF_WARN(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "id=%p: ECS provider failed to resolve host, error code %d(%s)",
            (void *)ecs_user_data->ecs_provider,
            error_code,
            aws_error_str(error_code));
        ecs_user_data->error_code = error_code;
        s_ecs_finalize_get_credentials_query(ecs_user_data);
        return;
    }
    size_t host_addresses_len = aws_array_list_length(host_addresses);
    if (!host_addresses_len) {
        goto on_error;
    }
    for (size_t i = 0; i < host_addresses_len; ++i) {
        struct aws_host_address *host_address_ptr = NULL;
        aws_array_list_get_at_ptr(host_addresses, (void **)&host_address_ptr, i);
        if (!s_is_valid_remote_host_ip(host_address_ptr)) {
            goto on_error;
        }
    }
    struct aws_credentials_provider_ecs_impl *impl = ecs_user_data->ecs_provider->impl;
    impl->function_table->aws_http_connection_manager_acquire_connection(
        impl->connection_manager, s_ecs_on_acquire_connection, ecs_user_data);

    return;
on_error:
    AWS_LOGF_ERROR(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER,
        "id=%p: ECS provider failed to resolve address to an allowed ip address with error %d(%s)",
        (void *)ecs_user_data->ecs_provider,
        AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_HOST,
        aws_error_str(AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_HOST));
    ecs_user_data->error_code = AWS_AUTH_CREDENTIALS_PROVIDER_ECS_INVALID_HOST;
    s_ecs_finalize_get_credentials_query(ecs_user_data);
}

static int s_credentials_provider_ecs_get_credentials_async(
    struct aws_credentials_provider *provider,
    aws_on_get_credentials_callback_fn callback,
    void *user_data) {

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_CREDENTIALS_PROVIDER, "id=%p: ECS provider trying to load credentials", (void *)provider);

    struct aws_credentials_provider_ecs_impl *impl = provider->impl;

    struct aws_credentials_provider_ecs_user_data *wrapped_user_data =
        s_aws_credentials_provider_ecs_user_data_new(provider, callback, user_data);
    if (wrapped_user_data == NULL) {
        goto error;
    }
    /* No need to verify the host IP address if the connection is using HTTPS or the ECS container host (relative URI)
     */
    if (impl->is_https || aws_string_eq(impl->host, s_ecs_host)) {
        impl->function_table->aws_http_connection_manager_acquire_connection(
            impl->connection_manager, s_ecs_on_acquire_connection, wrapped_user_data);
    } else if (aws_host_resolver_resolve_host(
                   impl->bootstrap->host_resolver,
                   impl->host,
                   s_on_host_resolved,
                   &impl->bootstrap->host_resolver_config,
                   wrapped_user_data)) {
        goto error;
    }
    return AWS_OP_SUCCESS;

error:

    s_aws_credentials_provider_ecs_user_data_destroy(wrapped_user_data);

    return AWS_OP_ERR;
}

static void s_credentials_provider_ecs_destroy(struct aws_credentials_provider *provider) {
    struct aws_credentials_provider_ecs_impl *impl = provider->impl;
    if (impl == NULL) {
        return;
    }

    aws_string_destroy(impl->path_and_query);
    aws_string_destroy(impl->auth_token);
    aws_string_destroy(impl->auth_token_file_path);
    aws_string_destroy(impl->host);
    aws_client_bootstrap_release(impl->bootstrap);

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

static struct aws_credentials_provider_vtable s_aws_credentials_provider_ecs_vtable = {
    .get_credentials = s_credentials_provider_ecs_get_credentials_async,
    .destroy = s_credentials_provider_ecs_destroy,
};

static void s_on_connection_manager_shutdown(void *user_data) {
    struct aws_credentials_provider *provider = user_data;

    aws_credentials_provider_invoke_shutdown_callback(provider);
    aws_mem_release(provider->allocator, provider);
}

struct aws_credentials_provider *aws_credentials_provider_new_ecs(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_ecs_options *options) {

    if (!options->bootstrap) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "ECS provider: bootstrap must be specified");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_credentials_provider *provider = NULL;
    struct aws_credentials_provider_ecs_impl *impl = NULL;

    aws_mem_acquire_many(
        allocator,
        2,
        &provider,
        sizeof(struct aws_credentials_provider),
        &impl,
        sizeof(struct aws_credentials_provider_ecs_impl));

    if (!provider) {
        return NULL;
    }

    AWS_ZERO_STRUCT(*provider);
    AWS_ZERO_STRUCT(*impl);

    aws_credentials_provider_init_base(provider, allocator, &s_aws_credentials_provider_ecs_vtable, impl);
    impl->bootstrap = aws_client_bootstrap_acquire(options->bootstrap);
    struct aws_tls_connection_options tls_connection_options;
    AWS_ZERO_STRUCT(tls_connection_options);
    if (options->tls_ctx) {
        aws_tls_connection_options_init_from_ctx(&tls_connection_options, options->tls_ctx);
        struct aws_byte_cursor host = options->host;
        if (aws_tls_connection_options_set_server_name(&tls_connection_options, allocator, &host)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "(id=%p): failed to create a tls connection options with error %s",
                (void *)provider,
                aws_error_debug_str(aws_last_error()));
            goto on_error;
        }
        impl->is_https = true;
    }

    struct aws_socket_options socket_options;
    AWS_ZERO_STRUCT(socket_options);
    socket_options.type = AWS_SOCKET_STREAM;
    socket_options.domain = AWS_SOCKET_IPV4;
    socket_options.connect_timeout_ms = (uint32_t)aws_timestamp_convert(
        ECS_CONNECT_TIMEOUT_DEFAULT_IN_SECONDS, AWS_TIMESTAMP_SECS, AWS_TIMESTAMP_MILLIS, NULL);

    struct aws_http_connection_manager_options manager_options;
    AWS_ZERO_STRUCT(manager_options);
    manager_options.bootstrap = options->bootstrap;
    manager_options.initial_window_size = ECS_RESPONSE_SIZE_LIMIT;
    manager_options.socket_options = &socket_options;
    manager_options.host = options->host;
    if (options->port == 0) {
        manager_options.port = options->tls_ctx ? 443 : 80;
    } else {
        manager_options.port = options->port;
    }
    manager_options.max_connections = 2;
    manager_options.shutdown_complete_callback = s_on_connection_manager_shutdown;
    manager_options.shutdown_complete_user_data = provider;
    manager_options.tls_connection_options = options->tls_ctx ? &tls_connection_options : NULL;

    impl->function_table = options->function_table;
    if (impl->function_table == NULL) {
        impl->function_table = g_aws_credentials_provider_http_function_table;
    }

    impl->connection_manager = impl->function_table->aws_http_connection_manager_new(allocator, &manager_options);
    if (impl->connection_manager == NULL) {
        goto on_error;
    }
    if (options->auth_token.len != 0) {
        impl->auth_token = aws_string_new_from_cursor(allocator, &options->auth_token);
        if (impl->auth_token == NULL) {
            goto on_error;
        }
    }
    if (options->auth_token_file_path.len != 0) {
        impl->auth_token_file_path = aws_string_new_from_cursor(allocator, &options->auth_token_file_path);
        if (impl->auth_token_file_path == NULL) {
            goto on_error;
        }
    }

    impl->path_and_query = aws_string_new_from_cursor(allocator, &options->path_and_query);
    if (impl->path_and_query == NULL) {
        goto on_error;
    }

    impl->host = aws_string_new_from_cursor(allocator, &options->host);
    if (impl->host == NULL) {
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

struct aws_credentials_provider *aws_credentials_provider_new_ecs_from_environment(
    struct aws_allocator *allocator,
    const struct aws_credentials_provider_ecs_environment_options *options) {

    if (!options->tls_ctx) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_CREDENTIALS_PROVIDER, "ECS provider: tls_ctx must be specified");
        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_credentials_provider_ecs_options explicit_options = {
        .shutdown_options = options->shutdown_options,
        .bootstrap = options->bootstrap,
        .function_table = options->function_table,
    };

    struct aws_string *ecs_env_token_file_path = NULL;
    struct aws_string *ecs_env_token = NULL;
    struct aws_string *relative_uri_str = NULL;
    struct aws_string *full_uri_str = NULL;
    struct aws_uri full_uri;
    AWS_ZERO_STRUCT(full_uri);
    struct aws_credentials_provider *provider = NULL;

    /* read the environment variables */
    aws_get_environment_value(allocator, s_ecs_creds_env_token_file, &ecs_env_token_file_path);
    aws_get_environment_value(allocator, s_ecs_creds_env_token, &ecs_env_token);
    aws_get_environment_value(allocator, s_ecs_creds_env_relative_uri, &relative_uri_str);
    aws_get_environment_value(allocator, s_ecs_creds_env_full_uri, &full_uri_str);

    if (ecs_env_token_file_path != NULL && ecs_env_token_file_path->len > 0) {
        explicit_options.auth_token_file_path = aws_byte_cursor_from_string(ecs_env_token_file_path);
    }
    if (ecs_env_token != NULL && ecs_env_token->len > 0) {
        explicit_options.auth_token = aws_byte_cursor_from_string(ecs_env_token);
    }

    if (relative_uri_str != NULL && relative_uri_str->len != 0) {

        /* Using RELATIVE_URI */
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "ECS provider: using relative uri %s",
            aws_string_c_str(relative_uri_str));

        explicit_options.path_and_query = aws_byte_cursor_from_string(relative_uri_str);
        explicit_options.host = aws_byte_cursor_from_string(s_ecs_host);
        explicit_options.port = 80;

        provider = aws_credentials_provider_new_ecs(allocator, &explicit_options);

    } else if (full_uri_str != NULL && full_uri_str->len != 0) {

        /* Using FULL_URI */
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER, "ECS provider: using full uri %s", aws_string_c_str(full_uri_str));
        struct aws_byte_cursor full_uri_cursor = aws_byte_cursor_from_string(full_uri_str);
        if (aws_uri_init_parse(&full_uri, allocator, &full_uri_cursor)) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_CREDENTIALS_PROVIDER,
                "ECS provider: Failed because %s environment variable is invalid uri %s.",
                aws_string_c_str(s_ecs_creds_env_full_uri),
                aws_string_c_str(full_uri_str));
            goto cleanup;
        }

        explicit_options.host = *aws_uri_host_name(&full_uri);
        explicit_options.path_and_query = *aws_uri_path_and_query(&full_uri);
        if (explicit_options.path_and_query.len == 0) {
            explicit_options.path_and_query = aws_byte_cursor_from_c_str("/");
        }

        if (aws_byte_cursor_eq_c_str_ignore_case(aws_uri_scheme(&full_uri), "https")) {
            explicit_options.tls_ctx = options->tls_ctx;
        }

        explicit_options.port = aws_uri_port(&full_uri);
        if (explicit_options.port == 0) {
            explicit_options.port = explicit_options.tls_ctx ? 443 : 80;
        }

        provider = aws_credentials_provider_new_ecs(allocator, &explicit_options);

    } else {
        /* Neither environment variable is set */
        AWS_LOGF_INFO(
            AWS_LS_AUTH_CREDENTIALS_PROVIDER,
            "ECS provider: Unable to initialize from environment because AWS_CONTAINER_CREDENTIALS_FULL_URI and "
            "AWS_CONTAINER_CREDENTIALS_RELATIVE_URI are not set.");
        aws_raise_error(AWS_AUTH_CREDENTIALS_PROVIDER_INVALID_ENVIRONMENT);
        goto cleanup;
    }

cleanup:
    aws_string_destroy(relative_uri_str);
    aws_string_destroy(full_uri_str);
    aws_string_destroy(ecs_env_token_file_path);
    aws_string_destroy(ecs_env_token);

    aws_uri_clean_up(&full_uri);
    return provider;
}
