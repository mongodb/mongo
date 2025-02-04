/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/private/aws_signing.h>

#include <aws/auth/credentials.h>
#include <aws/auth/private/key_derivation.h>
#include <aws/auth/signable.h>
#include <aws/auth/signing.h>
#include <aws/cal/ecc.h>
#include <aws/cal/hash.h>
#include <aws/cal/hmac.h>
#include <aws/common/date_time.h>
#include <aws/common/encoding.h>
#include <aws/common/string.h>
#include <aws/io/stream.h>
#include <aws/io/uri.h>

#include <ctype.h>
#include <inttypes.h>

#if defined(_MSC_VER)
#    pragma warning(disable : 4204)
#endif /* _MSC_VER */

/*
 * A bunch of initial size values for various buffers used throughout the signing process
 *
 * We want them to be sufficient-but-not-wasting-significant-amounts-of-memory for "most"
 * requests.  The body read buffer is an exception since it will just be holding windows rather than
 * the entire thing.
 */
#define BODY_READ_BUFFER_SIZE 4096
#define CANONICAL_REQUEST_STARTING_SIZE 1024
#define STRING_TO_SIGN_STARTING_SIZE 256
#define SIGNED_HEADERS_STARTING_SIZE 256
#define CANONICAL_HEADER_BLOCK_STARTING_SIZE 1024
#define AUTHORIZATION_VALUE_STARTING_SIZE 512
#define PAYLOAD_HASH_STARTING_SIZE (AWS_SHA256_LEN * 2)
#define CREDENTIAL_SCOPE_STARTING_SIZE 128
#define ACCESS_CREDENTIAL_SCOPE_STARTING_SIZE 149
#define SCRATCH_BUF_STARTING_SIZE 256
#define MAX_AUTHORIZATION_HEADER_COUNT 4
#define MAX_AUTHORIZATION_QUERY_PARAM_COUNT 6
#define DEFAULT_PATH_COMPONENT_COUNT 10
#define CANONICAL_REQUEST_SPLIT_OVER_ESTIMATE 20
#define HEX_ENCODED_SIGNATURE_OVER_ESTIMATE 256
#define MAX_ECDSA_P256_SIGNATURE_AS_BINARY_LENGTH 72
#define MAX_ECDSA_P256_SIGNATURE_AS_HEX_LENGTH (MAX_ECDSA_P256_SIGNATURE_AS_BINARY_LENGTH * 2)
#define AWS_SIGV4A_SIGNATURE_PADDING_BYTE ('*')

AWS_STRING_FROM_LITERAL(g_aws_signing_content_header_name, "x-amz-content-sha256");
AWS_STRING_FROM_LITERAL(g_aws_signing_authorization_header_name, "Authorization");
AWS_STRING_FROM_LITERAL(g_aws_signing_authorization_query_param_name, "X-Amz-Signature");
AWS_STRING_FROM_LITERAL(g_aws_signing_algorithm_query_param_name, "X-Amz-Algorithm");
AWS_STRING_FROM_LITERAL(g_aws_signing_credential_query_param_name, "X-Amz-Credential");
AWS_STRING_FROM_LITERAL(g_aws_signing_date_name, "X-Amz-Date");
AWS_STRING_FROM_LITERAL(g_aws_signing_signed_headers_query_param_name, "X-Amz-SignedHeaders");
AWS_STRING_FROM_LITERAL(g_aws_signing_security_token_name, "X-Amz-Security-Token");
AWS_STRING_FROM_LITERAL(g_aws_signing_s3session_token_name, "X-Amz-S3session-Token");
AWS_STRING_FROM_LITERAL(g_aws_signing_expires_query_param_name, "X-Amz-Expires");
AWS_STRING_FROM_LITERAL(g_aws_signing_region_set_name, "X-Amz-Region-Set");

AWS_STATIC_STRING_FROM_LITERAL(s_signature_type_sigv4_http_request, "AWS4-HMAC-SHA256");
AWS_STATIC_STRING_FROM_LITERAL(s_signature_type_sigv4_s3_chunked_payload, "AWS4-HMAC-SHA256-PAYLOAD");
AWS_STATIC_STRING_FROM_LITERAL(s_signature_type_sigv4a_s3_chunked_payload, "AWS4-ECDSA-P256-SHA256-PAYLOAD");

AWS_STATIC_STRING_FROM_LITERAL(s_signature_type_sigv4_s3_chunked_trailer_payload, "AWS4-HMAC-SHA256-TRAILER");
AWS_STATIC_STRING_FROM_LITERAL(s_signature_type_sigv4a_s3_chunked_trailer_payload, "AWS4-ECDSA-P256-SHA256-TRAILER");

/* aws-related query param and header tables */
static struct aws_hash_table s_forbidden_headers;
static struct aws_hash_table s_forbidden_params;
static struct aws_hash_table s_skipped_headers;

static struct aws_byte_cursor s_amzn_trace_id_header_name;
static struct aws_byte_cursor s_user_agent_header_name;
static struct aws_byte_cursor s_connection_header_name;
static struct aws_byte_cursor s_sec_websocket_key_header_name;
static struct aws_byte_cursor s_sec_websocket_protocol_header_name;
static struct aws_byte_cursor s_sec_websocket_version_header_name;
static struct aws_byte_cursor s_upgrade_header_name;

static struct aws_byte_cursor s_amz_content_sha256_header_name;
static struct aws_byte_cursor s_amz_date_header_name;
static struct aws_byte_cursor s_authorization_header_name;
static struct aws_byte_cursor s_region_set_header_name;
static struct aws_byte_cursor s_amz_security_token_header_name;
static struct aws_byte_cursor s_amz_s3session_token_header_name;

static struct aws_byte_cursor s_amz_signature_param_name;
static struct aws_byte_cursor s_amz_date_param_name;
static struct aws_byte_cursor s_amz_credential_param_name;
static struct aws_byte_cursor s_amz_algorithm_param_name;
static struct aws_byte_cursor s_amz_signed_headers_param_name;
static struct aws_byte_cursor s_amz_security_token_param_name;
static struct aws_byte_cursor s_amz_expires_param_name;
static struct aws_byte_cursor s_amz_region_set_param_name;

/*
 * Build a set of library-static tables for quick lookup.
 *
 * Construction errors are considered fatal.
 */
int aws_signing_init_signing_tables(struct aws_allocator *allocator) {

    if (aws_hash_table_init(
            &s_skipped_headers,
            allocator,
            10,
            aws_hash_byte_cursor_ptr_ignore_case,
            (aws_hash_callback_eq_fn *)aws_byte_cursor_eq_ignore_case,
            NULL,
            NULL)) {
        return AWS_OP_ERR;
    }

    s_amzn_trace_id_header_name = aws_byte_cursor_from_c_str("x-amzn-trace-id");
    if (aws_hash_table_put(&s_skipped_headers, &s_amzn_trace_id_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_user_agent_header_name = aws_byte_cursor_from_c_str("User-Agent");
    if (aws_hash_table_put(&s_skipped_headers, &s_user_agent_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_connection_header_name = aws_byte_cursor_from_c_str("connection");
    if (aws_hash_table_put(&s_skipped_headers, &s_connection_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_connection_header_name = aws_byte_cursor_from_c_str("expect");
    if (aws_hash_table_put(&s_skipped_headers, &s_connection_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_sec_websocket_key_header_name = aws_byte_cursor_from_c_str("sec-websocket-key");
    if (aws_hash_table_put(&s_skipped_headers, &s_sec_websocket_key_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_sec_websocket_protocol_header_name = aws_byte_cursor_from_c_str("sec-websocket-protocol");
    if (aws_hash_table_put(&s_skipped_headers, &s_sec_websocket_protocol_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_sec_websocket_version_header_name = aws_byte_cursor_from_c_str("sec-websocket-version");
    if (aws_hash_table_put(&s_skipped_headers, &s_sec_websocket_version_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_upgrade_header_name = aws_byte_cursor_from_c_str("upgrade");
    if (aws_hash_table_put(&s_skipped_headers, &s_upgrade_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    if (aws_hash_table_init(
            &s_forbidden_headers,
            allocator,
            10,
            aws_hash_byte_cursor_ptr_ignore_case,
            (aws_hash_callback_eq_fn *)aws_byte_cursor_eq_ignore_case,
            NULL,
            NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_content_sha256_header_name = aws_byte_cursor_from_string(g_aws_signing_content_header_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_amz_content_sha256_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_date_header_name = aws_byte_cursor_from_string(g_aws_signing_date_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_amz_date_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_authorization_header_name = aws_byte_cursor_from_string(g_aws_signing_authorization_header_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_authorization_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_region_set_header_name = aws_byte_cursor_from_string(g_aws_signing_region_set_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_region_set_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_security_token_header_name = aws_byte_cursor_from_string(g_aws_signing_security_token_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_amz_security_token_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_s3session_token_header_name = aws_byte_cursor_from_string(g_aws_signing_s3session_token_name);
    if (aws_hash_table_put(&s_forbidden_headers, &s_amz_s3session_token_header_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    if (aws_hash_table_init(
            &s_forbidden_params,
            allocator,
            10,
            aws_hash_byte_cursor_ptr_ignore_case,
            (aws_hash_callback_eq_fn *)aws_byte_cursor_eq_ignore_case,
            NULL,
            NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_signature_param_name = aws_byte_cursor_from_string(g_aws_signing_authorization_query_param_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_signature_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_date_param_name = aws_byte_cursor_from_string(g_aws_signing_date_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_date_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_credential_param_name = aws_byte_cursor_from_string(g_aws_signing_credential_query_param_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_credential_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_algorithm_param_name = aws_byte_cursor_from_string(g_aws_signing_algorithm_query_param_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_algorithm_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_signed_headers_param_name = aws_byte_cursor_from_string(g_aws_signing_signed_headers_query_param_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_signed_headers_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_security_token_param_name = aws_byte_cursor_from_string(g_aws_signing_security_token_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_security_token_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_expires_param_name = aws_byte_cursor_from_string(g_aws_signing_expires_query_param_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_expires_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    s_amz_region_set_param_name = aws_byte_cursor_from_string(g_aws_signing_region_set_name);
    if (aws_hash_table_put(&s_forbidden_params, &s_amz_region_set_param_name, NULL, NULL)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_signing_clean_up_signing_tables(void) {
    aws_hash_table_clean_up(&s_skipped_headers);
    aws_hash_table_clean_up(&s_forbidden_headers);
    aws_hash_table_clean_up(&s_forbidden_params);
}

static bool s_is_header_based_signature_value(enum aws_signature_type signature_type) {
    switch (signature_type) {
        case AWS_ST_HTTP_REQUEST_HEADERS:
        case AWS_ST_CANONICAL_REQUEST_HEADERS:
            return true;

        default:
            return false;
    }
}

static bool s_is_query_param_based_signature_value(enum aws_signature_type signature_type) {
    switch (signature_type) {
        case AWS_ST_HTTP_REQUEST_QUERY_PARAMS:
        case AWS_ST_CANONICAL_REQUEST_QUERY_PARAMS:
            return true;

        default:
            return false;
    }
}

static int s_get_signature_type_cursor(struct aws_signing_state_aws *state, struct aws_byte_cursor *cursor) {
    switch (state->config.signature_type) {
        case AWS_ST_HTTP_REQUEST_HEADERS:
        case AWS_ST_HTTP_REQUEST_QUERY_PARAMS:
        case AWS_ST_CANONICAL_REQUEST_HEADERS:
        case AWS_ST_CANONICAL_REQUEST_QUERY_PARAMS:
            if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
                *cursor = aws_byte_cursor_from_string(g_signature_type_sigv4a_http_request);
            } else {
                *cursor = aws_byte_cursor_from_string(s_signature_type_sigv4_http_request);
            }
            break;
        case AWS_ST_HTTP_REQUEST_CHUNK:
        case AWS_ST_HTTP_REQUEST_EVENT:
            if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
                *cursor = aws_byte_cursor_from_string(s_signature_type_sigv4a_s3_chunked_payload);
            } else {
                *cursor = aws_byte_cursor_from_string(s_signature_type_sigv4_s3_chunked_payload);
            }
            break;
        case AWS_ST_HTTP_REQUEST_TRAILING_HEADERS:
            if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
                *cursor = aws_byte_cursor_from_string(s_signature_type_sigv4a_s3_chunked_trailer_payload);
            } else {
                *cursor = aws_byte_cursor_from_string(s_signature_type_sigv4_s3_chunked_trailer_payload);
            }
            break;

        default:
            return aws_raise_error(AWS_AUTH_SIGNING_UNSUPPORTED_SIGNATURE_TYPE);
    }

    return AWS_OP_SUCCESS;
}

static int s_append_sts_signature_type(struct aws_signing_state_aws *state, struct aws_byte_buf *dest) {
    struct aws_byte_cursor algorithm_cursor;
    if (s_get_signature_type_cursor(state, &algorithm_cursor)) {
        return AWS_OP_ERR;
    }

    return aws_byte_buf_append_dynamic(dest, &algorithm_cursor);
}

/*
 * signing state management
 */
struct aws_signing_state_aws *aws_signing_state_new(
    struct aws_allocator *allocator,
    const struct aws_signing_config_aws *config,
    const struct aws_signable *signable,
    aws_signing_complete_fn *on_complete,
    void *userdata) {

    if (aws_validate_aws_signing_config_aws(config)) {
        return NULL;
    }

    struct aws_signing_state_aws *state = aws_mem_calloc(allocator, 1, sizeof(struct aws_signing_state_aws));
    if (!state) {
        return NULL;
    }

    state->allocator = allocator;

    /* Make our own copy of the signing config */
    state->config = *config;

    if (state->config.credentials_provider != NULL) {
        aws_credentials_provider_acquire(state->config.credentials_provider);
    }

    if (state->config.credentials != NULL) {
        aws_credentials_acquire(state->config.credentials);
    }

    if (aws_byte_buf_init_cache_and_update_cursors(
            &state->config_string_buffer,
            allocator,
            &state->config.region,
            &state->config.service,
            &state->config.signed_body_value,
            NULL /*end*/)) {
        goto on_error;
    }

    state->signable = signable;
    state->on_complete = on_complete;
    state->userdata = userdata;

    if (aws_signing_result_init(&state->result, allocator)) {
        goto on_error;
    }

    if (aws_byte_buf_init(&state->canonical_request, allocator, CANONICAL_REQUEST_STARTING_SIZE) ||
        aws_byte_buf_init(&state->string_to_sign, allocator, STRING_TO_SIGN_STARTING_SIZE) ||
        aws_byte_buf_init(&state->signed_headers, allocator, SIGNED_HEADERS_STARTING_SIZE) ||
        aws_byte_buf_init(&state->canonical_header_block, allocator, CANONICAL_HEADER_BLOCK_STARTING_SIZE) ||
        aws_byte_buf_init(&state->payload_hash, allocator, PAYLOAD_HASH_STARTING_SIZE) ||
        aws_byte_buf_init(&state->credential_scope, allocator, CREDENTIAL_SCOPE_STARTING_SIZE) ||
        aws_byte_buf_init(&state->access_credential_scope, allocator, ACCESS_CREDENTIAL_SCOPE_STARTING_SIZE) ||
        aws_byte_buf_init(&state->date, allocator, AWS_DATE_TIME_STR_MAX_LEN) ||
        aws_byte_buf_init(&state->signature, allocator, PAYLOAD_HASH_STARTING_SIZE) ||
        aws_byte_buf_init(&state->string_to_sign_payload, allocator, PAYLOAD_HASH_STARTING_SIZE) ||
        aws_byte_buf_init(&state->scratch_buf, allocator, SCRATCH_BUF_STARTING_SIZE)) {

        goto on_error;
    }

    snprintf(
        state->expiration_array, AWS_ARRAY_SIZE(state->expiration_array), "%" PRIu64 "", config->expiration_in_seconds);

    return state;

on_error:
    aws_signing_state_destroy(state);
    return NULL;
}

void aws_signing_state_destroy(struct aws_signing_state_aws *state) {
    aws_signing_result_clean_up(&state->result);

    aws_credentials_provider_release(state->config.credentials_provider);
    aws_credentials_release(state->config.credentials);

    aws_byte_buf_clean_up(&state->config_string_buffer);
    aws_byte_buf_clean_up(&state->canonical_request);
    aws_byte_buf_clean_up(&state->string_to_sign);
    aws_byte_buf_clean_up(&state->signed_headers);
    aws_byte_buf_clean_up(&state->canonical_header_block);
    aws_byte_buf_clean_up(&state->payload_hash);
    aws_byte_buf_clean_up(&state->credential_scope);
    aws_byte_buf_clean_up(&state->access_credential_scope);
    aws_byte_buf_clean_up(&state->date);
    aws_byte_buf_clean_up(&state->signature);
    aws_byte_buf_clean_up(&state->string_to_sign_payload);
    aws_byte_buf_clean_up(&state->scratch_buf);

    aws_mem_release(state->allocator, state);
}

/*
 * canonical request utility functions:
 *
 * various appends, conversion/encoding, etc...
 *
 */

static int s_append_canonical_method(struct aws_signing_state_aws *state) {
    const struct aws_signable *signable = state->signable;
    struct aws_byte_buf *buffer = &state->canonical_request;

    struct aws_byte_cursor method_cursor;
    aws_signable_get_property(signable, g_aws_http_method_property_name, &method_cursor);

    if (aws_byte_buf_append_dynamic(buffer, &method_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(buffer, '\n')) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_append_with_lookup(
    struct aws_byte_buf *dst,
    const struct aws_byte_cursor *src,
    const uint8_t *lookup_table) {

    if (aws_byte_buf_reserve_relative(dst, src->len)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_with_lookup(dst, src, lookup_table)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * A function that builds a normalized path (removes redundant '/' characters, '.' components, and properly pops off
 * components in response '..' components)
 *
 * We use a simple algorithm to do this:
 *
 * First split the path into components
 * Then, using a secondary stack of components, build the final path by pushing and popping (on '..') components
 * on the stack.  The final path is then the concatenation of the secondary stack.
 */
static int s_append_normalized_path(
    const struct aws_byte_cursor *raw_path,
    struct aws_allocator *allocator,
    struct aws_byte_buf *dest) {

    struct aws_array_list raw_split;
    AWS_ZERO_STRUCT(raw_split);

    struct aws_array_list normalized_split;
    AWS_ZERO_STRUCT(normalized_split);

    int result = AWS_OP_ERR;

    if (aws_array_list_init_dynamic(
            &raw_split, allocator, DEFAULT_PATH_COMPONENT_COUNT, sizeof(struct aws_byte_cursor))) {
        goto cleanup;
    }

    if (aws_byte_cursor_split_on_char(raw_path, '/', &raw_split)) {
        goto cleanup;
    }

    const size_t raw_split_count = aws_array_list_length(&raw_split);
    if (aws_array_list_init_dynamic(&normalized_split, allocator, raw_split_count, sizeof(struct aws_byte_cursor))) {
        goto cleanup;
    }

    /*
     * Iterate the raw split to build a list of path components that make up the
     * normalized path
     */
    for (size_t i = 0; i < raw_split_count; ++i) {
        struct aws_byte_cursor path_component;
        AWS_ZERO_STRUCT(path_component);
        if (aws_array_list_get_at(&raw_split, &path_component, i)) {
            goto cleanup;
        }

        if (path_component.len == 0 || (path_component.len == 1 && *path_component.ptr == '.')) {
            /* '.' and '' contribute nothing to a normalized path */
            continue;
        }

        if (path_component.len == 2 && *path_component.ptr == '.' && *(path_component.ptr + 1) == '.') {
            /* '..' causes us to remove the last valid path component */
            aws_array_list_pop_back(&normalized_split);
        } else {
            aws_array_list_push_back(&normalized_split, &path_component);
        }
    }

    /*
     * Special case preserve whether or not the path ended with a '/'
     */
    bool ends_with_slash = raw_path->len > 0 && raw_path->ptr[raw_path->len - 1] == '/';

    /*
     * Paths always start with a single '/'
     */
    if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
        goto cleanup;
    }

    /*
     * build the final normalized path from the normalized split by joining
     * the components together with '/'
     */
    const size_t normalized_split_count = aws_array_list_length(&normalized_split);
    for (size_t i = 0; i < normalized_split_count; ++i) {
        struct aws_byte_cursor normalized_path_component;
        AWS_ZERO_STRUCT(normalized_path_component);
        if (aws_array_list_get_at(&normalized_split, &normalized_path_component, i)) {
            goto cleanup;
        }

        if (aws_byte_buf_append_dynamic(dest, &normalized_path_component)) {
            goto cleanup;
        }

        if (i + 1 < normalized_split_count || ends_with_slash) {
            if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
                goto cleanup;
            }
        }
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_array_list_clean_up(&raw_split);
    aws_array_list_clean_up(&normalized_split);

    return result;
}

static int s_append_canonical_path(const struct aws_uri *uri, struct aws_signing_state_aws *state) {
    const struct aws_signing_config_aws *config = &state->config;
    struct aws_byte_buf *canonical_request_buffer = &state->canonical_request;
    struct aws_allocator *allocator = state->allocator;
    int result = AWS_OP_ERR;

    /*
     * Put this at function global scope so that it gets cleaned up even though it's only used inside
     * a single branch.  Allows error handling and cleanup to follow the pattern established
     * throughout this file.
     */
    struct aws_byte_buf normalized_path;
    AWS_ZERO_STRUCT(normalized_path);

    /*
     * We assume the request's uri path has already been encoded once (in order to go out on the wire).
     * Some services do not decode the path before performing the sig v4 calculation, resulting in the
     * service actually performing sigv4 on a double-encoding of the path.  In order to match those
     * services, we must double encode in our calculation as well.
     */
    if (config->flags.use_double_uri_encode) {
        struct aws_byte_cursor path_cursor;

        /*
         * We need to transform the the normalized path, so we can't just append it into the canonical
         * request.  Instead we append it into a temporary buffer and perform the transformation from
         * it.
         *
         * All this does is skip the temporary normalized path in the case where we don't need to
         * double encode.
         */
        if (config->flags.should_normalize_uri_path) {
            if (aws_byte_buf_init(&normalized_path, state->allocator, uri->path.len)) {
                goto cleanup;
            }

            if (s_append_normalized_path(&uri->path, allocator, &normalized_path)) {
                goto cleanup;
            }

            path_cursor = aws_byte_cursor_from_buf(&normalized_path);
        } else {
            path_cursor = uri->path;
        }

        if (aws_byte_buf_append_encoding_uri_path(canonical_request_buffer, &path_cursor)) {
            goto cleanup;
        }
    } else {
        /*
         * If we don't need to perform any kind of transformation on the normalized path, just append it directly
         * into the canonical request buffer
         */
        if (config->flags.should_normalize_uri_path) {
            if (s_append_normalized_path(&uri->path, allocator, canonical_request_buffer)) {
                goto cleanup;
            }
        } else {
            if (aws_byte_buf_append_dynamic(canonical_request_buffer, &uri->path)) {
                goto cleanup;
            }
        }
    }

    if (aws_byte_buf_append_byte_dynamic(canonical_request_buffer, '\n')) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_byte_buf_clean_up(&normalized_path);

    return result;
}

/*
 * URI-encoded query params are compared first by key, then by value
 */
int s_canonical_query_param_comparator(const void *lhs, const void *rhs) {
    const struct aws_uri_param *left_param = lhs;
    const struct aws_uri_param *right_param = rhs;

    int key_compare = aws_byte_cursor_compare_lexical(&left_param->key, &right_param->key);
    if (key_compare != 0) {
        return key_compare;
    }

    return aws_byte_cursor_compare_lexical(&left_param->value, &right_param->value);
}

/*
 * We need to sort the headers in a stable fashion, but the default sorting methods available in the c library are not
 * guaranteed to be stable.  We can make the sort stable by instead sorting a wrapper object that includes the original
 * index of the wrapped object and using that index to break lexical ties.
 *
 * We sort a copy of the header (rather than pointers) so that we can easily inject secondary headers into
 * the canonical request.
 */
struct stable_header {
    struct aws_signable_property_list_pair header;
    size_t original_index;
};

int s_canonical_header_comparator(const void *lhs, const void *rhs) {
    const struct stable_header *left_header = lhs;
    const struct stable_header *right_header = rhs;

    int result = aws_byte_cursor_compare_lookup(
        &left_header->header.name, &right_header->header.name, aws_lookup_table_to_lower_get());
    if (result != 0) {
        return result;
    }

    /* they're the same header, use the original index to keep the sort stable */
    if (left_header->original_index < right_header->original_index) {
        return -1;
    }

    /* equality should never happen */
    AWS_ASSERT(left_header->original_index > right_header->original_index);

    return 1;
}

/**
 * Given URI-encoded query param, write it to canonical buffer.
 */
static int s_append_canonical_query_param(struct aws_uri_param *encoded_param, struct aws_byte_buf *buffer) {
    if (aws_byte_buf_append_dynamic(buffer, &encoded_param->key)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(buffer, '=')) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_dynamic(buffer, &encoded_param->value)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/**
 * Given unencoded authorization query param:
 * Add it, URI-encoded to final signing result (to be added to signable later).
 */
static int s_add_query_param_to_signing_result(
    struct aws_signing_state_aws *state,
    const struct aws_uri_param *unencoded_param) {
    /* URI-Encode, and add to final signing result */
    state->scratch_buf.len = 0;
    if (aws_byte_buf_append_encoding_uri_param(&state->scratch_buf, &unencoded_param->key)) {
        return AWS_OP_ERR;
    }
    size_t key_len = state->scratch_buf.len;
    if (aws_byte_buf_append_encoding_uri_param(&state->scratch_buf, &unencoded_param->value)) {
        return AWS_OP_ERR;
    }
    struct aws_byte_cursor encoded_val = aws_byte_cursor_from_buf(&state->scratch_buf);
    struct aws_byte_cursor encoded_key = aws_byte_cursor_advance(&encoded_val, key_len);

    if (aws_signing_result_append_property_list(
            &state->result, g_aws_http_query_params_property_list_name, &encoded_key, &encoded_val)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/**
 * Given unencoded authorization query param:
 * 1) Add it to list of all unencoded query params (to be canonicalized later).
 * 2) Add it, URI-encoded to final signing result (to be added to signable later).
 */
static int s_add_authorization_query_param(
    struct aws_signing_state_aws *state,
    struct aws_array_list *unencoded_auth_params,
    const struct aws_uri_param *unencoded_auth_param) {

    /* Add to unencoded list */
    if (aws_array_list_push_back(unencoded_auth_params, unencoded_auth_param)) {
        return AWS_OP_ERR;
    }

    return s_add_query_param_to_signing_result(state, unencoded_auth_param);
}

/*
 * Checks the header against both an internal skip list as well as an optional user-supplied filter
 * function.  Only sign the header if both functions allow it.
 */
static bool s_should_sign_header(struct aws_signing_state_aws *state, struct aws_byte_cursor *name) {
    if (state->config.should_sign_header) {
        if (!state->config.should_sign_header(name, state->config.should_sign_header_ud)) {
            return false;
        }
    }

    struct aws_hash_element *element = NULL;
    if (aws_hash_table_find(&s_skipped_headers, name, &element) == AWS_OP_ERR || element != NULL) {
        return false;
    }

    return true;
}

/*
 * If the auth type was query param then this function adds all the required query params and values with the
 * exception of X-Amz-Signature (because we're still computing its value)  Parameters are added to both the
 * canonical request and the final signing result.
 */
static int s_add_authorization_query_params(
    struct aws_signing_state_aws *state,
    struct aws_array_list *unencoded_query_params) {

    if (state->config.signature_type != AWS_ST_HTTP_REQUEST_QUERY_PARAMS) {
        return AWS_OP_SUCCESS;
    }

    int result = AWS_OP_ERR;

    /* X-Amz-Algorithm */
    struct aws_uri_param algorithm_param = {
        .key = aws_byte_cursor_from_string(g_aws_signing_algorithm_query_param_name),
    };

    if (s_get_signature_type_cursor(state, &algorithm_param.value)) {
        goto done;
    }

    if (s_add_authorization_query_param(state, unencoded_query_params, &algorithm_param)) {
        goto done;
    }

    /* X-Amz-Credential */
    struct aws_uri_param credential_param = {
        .key = aws_byte_cursor_from_string(g_aws_signing_credential_query_param_name),
        .value = aws_byte_cursor_from_buf(&state->access_credential_scope),
    };

    if (s_add_authorization_query_param(state, unencoded_query_params, &credential_param)) {
        goto done;
    }

    /* X-Amz-Date */
    struct aws_uri_param date_param = {
        .key = aws_byte_cursor_from_string(g_aws_signing_date_name),
        .value = aws_byte_cursor_from_buf(&state->date),
    };

    if (s_add_authorization_query_param(state, unencoded_query_params, &date_param)) {
        goto done;
    }

    /* X-Amz-SignedHeaders */
    struct aws_uri_param signed_headers_param = {
        .key = aws_byte_cursor_from_string(g_aws_signing_signed_headers_query_param_name),
        .value = aws_byte_cursor_from_buf(&state->signed_headers),
    };

    if (s_add_authorization_query_param(state, unencoded_query_params, &signed_headers_param)) {
        goto done;
    }

    /* X-Amz-Expires */
    uint64_t expiration_in_seconds = state->config.expiration_in_seconds;
    if (expiration_in_seconds > 0) {
        struct aws_uri_param expires_param = {
            .key = aws_byte_cursor_from_string(g_aws_signing_expires_query_param_name),
            .value = aws_byte_cursor_from_c_str(state->expiration_array),
        };

        if (s_add_authorization_query_param(state, unencoded_query_params, &expires_param)) {
            goto done;
        }
    }

    /* X-Amz-*-token */
    /* We have different token between S3Express and other signing, which needs different token header name */
    struct aws_byte_cursor token_header_name;
    if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_S3EXPRESS) {
        /* X-Amz-S3session-Token */
        token_header_name = s_amz_s3session_token_header_name;
    } else {
        /* X-Amz-Security-Token */
        token_header_name = s_amz_security_token_header_name;
    }
    struct aws_byte_cursor session_token_cursor = aws_credentials_get_session_token(state->config.credentials);
    if (session_token_cursor.len > 0) {
        struct aws_uri_param security_token_param = {
            .key = token_header_name,
            .value = session_token_cursor,
        };

        /* If omit_session_token is true, then security token is added to the
         * final signing result, but is treated as "unsigned" and does not
         * contribute to the authorization signature */
        if (state->config.flags.omit_session_token) {
            if (s_add_query_param_to_signing_result(state, &security_token_param)) {
                goto done;
            }
        } else {
            if (s_add_authorization_query_param(state, unencoded_query_params, &security_token_param)) {
                goto done;
            }
        }
    }

    /* X-Amz-Region-Set */
    if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
        struct aws_uri_param region_set_param = {
            .key = aws_byte_cursor_from_string(g_aws_signing_region_set_name),
            .value = state->config.region,
        };

        if (s_add_authorization_query_param(state, unencoded_query_params, &region_set_param)) {
            goto done;
        }
    }

    /* NOTE: Update MAX_AUTHORIZATION_QUERY_PARAM_COUNT if more params added */

    result = AWS_OP_SUCCESS;

done:
    return result;
}

static int s_validate_query_params(struct aws_array_list *unencoded_query_params) {
    const size_t param_count = aws_array_list_length(unencoded_query_params);
    for (size_t i = 0; i < param_count; ++i) {
        struct aws_uri_param param;
        AWS_ZERO_STRUCT(param);
        aws_array_list_get_at(unencoded_query_params, &param, i);

        struct aws_hash_element *forbidden_element = NULL;
        aws_hash_table_find(&s_forbidden_params, &param.key, &forbidden_element);

        if (forbidden_element != NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_SIGNING,
                "AWS authorization query param \"" PRInSTR "\" found in request while signing",
                AWS_BYTE_CURSOR_PRI(param.key));
            return aws_raise_error(AWS_AUTH_SIGNING_ILLEGAL_REQUEST_QUERY_PARAM);
        }
    }

    return AWS_OP_SUCCESS;
}

/**
 * Apply or remove URI-encoding to each aws_uri_param in a list.
 * (new strings are added to temp_strings)
 * Append function must grow buffer if necessary.
 */
static int s_transform_query_params(
    struct aws_signing_state_aws *state,
    struct aws_array_list *param_list,
    struct aws_array_list *temp_strings,
    int (*byte_buf_append_dynamic_param_fn)(struct aws_byte_buf *, const struct aws_byte_cursor *)) {

    const size_t param_count = aws_array_list_length(param_list);
    struct aws_uri_param *param = NULL;
    for (size_t i = 0; i < param_count; ++i) {
        aws_array_list_get_at_ptr(param_list, (void **)&param, i);

        /* encode/decode key and save string */
        state->scratch_buf.len = 0;
        if (byte_buf_append_dynamic_param_fn(&state->scratch_buf, &param->key)) {
            return AWS_OP_ERR;
        }
        struct aws_string *key_str = aws_string_new_from_buf(state->allocator, &state->scratch_buf);
        if (!key_str) {
            return AWS_OP_ERR;
        }
        if (aws_array_list_push_back(temp_strings, &key_str)) {
            aws_string_destroy(key_str);
            return AWS_OP_ERR;
        }

        /* encode/decode value and save string */
        state->scratch_buf.len = 0;
        if (byte_buf_append_dynamic_param_fn(&state->scratch_buf, &param->value)) {
            return AWS_OP_ERR;
        }
        struct aws_string *value_str = aws_string_new_from_buf(state->allocator, &state->scratch_buf);
        if (!value_str) {
            return AWS_OP_ERR;
        }
        if (aws_array_list_push_back(temp_strings, &value_str)) {
            aws_string_destroy(value_str);
            return AWS_OP_ERR;
        }

        /* save encoded/decoded param */
        param->key = aws_byte_cursor_from_string(key_str);
        param->value = aws_byte_cursor_from_string(value_str);
    }

    return AWS_OP_SUCCESS;
}

/*
 * Adds the full canonical query string to the canonical request.
 * Note that aws-c-auth takes query params from the URI, so they should already be URI-encoded.
 * To ensure that the signature uses "canonical" URI-encoding, we decode and then re-encode the params.
 */
static int s_append_canonical_query_string(struct aws_uri *uri, struct aws_signing_state_aws *state) {
    struct aws_allocator *allocator = state->allocator;
    struct aws_byte_buf *canonical_request_buffer = &state->canonical_request;

    int result = AWS_OP_ERR;
    struct aws_array_list query_params;
    AWS_ZERO_STRUCT(query_params);
    struct aws_array_list temp_strings;
    AWS_ZERO_STRUCT(temp_strings);

    /* Determine max number of query parameters.
     * If none, skip to end of function */
    size_t max_param_count = 0;
    struct aws_uri_param param_i;
    AWS_ZERO_STRUCT(param_i);
    while (aws_uri_query_string_next_param(uri, &param_i)) {
        ++max_param_count;
    }
    if (state->config.signature_type == AWS_ST_HTTP_REQUEST_QUERY_PARAMS) {
        max_param_count += MAX_AUTHORIZATION_QUERY_PARAM_COUNT;
    }
    if (max_param_count == 0) {
        goto finish;
    }

    /* Allocate storage for mutable list of query params */
    if (aws_array_list_init_dynamic(&query_params, allocator, max_param_count, sizeof(struct aws_uri_param))) {
        goto cleanup;
    }

    /* Allocate storage for both the decoded, and re-encoded, key and value strings */
    if (aws_array_list_init_dynamic(
            &temp_strings, state->allocator, max_param_count * 4, sizeof(struct aws_string *))) {
        goto cleanup;
    }

    /* Get existing query params */
    if (aws_uri_query_string_params(uri, &query_params)) {
        goto cleanup;
    }

    /* Remove URI-encoding */
    if (s_transform_query_params(state, &query_params, &temp_strings, aws_byte_buf_append_decoding_uri)) {
        goto cleanup;
    }

    /* Validate existing query params */
    if (s_validate_query_params(&query_params)) {
        goto cleanup;
    }

    /* Add authorization query params */
    if (s_add_authorization_query_params(state, &query_params)) {
        goto cleanup;
    }

    /* Apply canonical URI-encoding to the query params */
    if (s_transform_query_params(state, &query_params, &temp_strings, aws_byte_buf_append_encoding_uri_param)) {
        goto cleanup;
    }

    const size_t param_count = aws_array_list_length(&query_params);

    /* Sort the encoded params and append to canonical request */
    qsort(query_params.data, param_count, sizeof(struct aws_uri_param), s_canonical_query_param_comparator);
    for (size_t i = 0; i < param_count; ++i) {
        struct aws_uri_param param;
        AWS_ZERO_STRUCT(param);
        if (aws_array_list_get_at(&query_params, &param, i)) {
            goto cleanup;
        }

        if (s_append_canonical_query_param(&param, canonical_request_buffer)) {
            goto cleanup;
        }

        if (i + 1 < param_count) {
            if (aws_byte_buf_append_byte_dynamic(canonical_request_buffer, '&')) {
                goto cleanup;
            }
        }
    }

finish:
    if (aws_byte_buf_append_byte_dynamic(canonical_request_buffer, '\n')) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_array_list_clean_up(&query_params);

    if (aws_array_list_is_valid(&temp_strings)) {
        const size_t string_count = aws_array_list_length(&temp_strings);
        for (size_t i = 0; i < string_count; ++i) {
            struct aws_string *string = NULL;
            aws_array_list_get_at(&temp_strings, &string, i);
            aws_string_destroy(string);
        }
        aws_array_list_clean_up(&temp_strings);
    }

    return result;
}

/*
 * It is unclear from the spec (and not resolved by the tests) whether other forms of whitespace (\t \v) should be
 * included in the trimming done to headers
 */
static bool s_is_space(uint8_t value) {
    return aws_isspace(value);
}

/*
 * Appends a single header key-value pair to the canonical request.  Multi-line and repeat headers make this more
 * complicated than you'd expect.
 *
 * We call this function on a sorted collection, so header repeats are guaranteed to be consecutive.
 *
 * In particular, there are two cases:
 *   (1) This is a header whose name hasn't been seen before, in which case we start a new line and append both name and
 * value. (2) This is a header we've previously seen, just append the value.
 *
 * The fact that we can't '\n' until we've moved to a new header name also complicates the logic.
 *
 * This function appends to a state buffer rather than the canonical request.  This allows us to calculate the signed
 * headers (so that it can go into the query param if needed) before the query params are put into the canonical
 * request.
 */
static int s_append_canonical_header(
    struct aws_signing_state_aws *state,
    struct aws_signable_property_list_pair *header,
    const struct aws_byte_cursor *last_seen_header_name) {

    struct aws_byte_buf *canonical_header_buffer = &state->canonical_header_block;
    struct aws_byte_buf *signed_headers_buffer = &state->signed_headers;
    const uint8_t *to_lower_table = aws_lookup_table_to_lower_get();

    /*
     * Write to the signed_headers shared state for later use, copy
     * to canonical header buffer as well
     */
    if (last_seen_header_name == NULL ||
        aws_byte_cursor_compare_lookup(last_seen_header_name, &header->name, aws_lookup_table_to_lower_get()) != 0) {
        /*
         * The headers arrive in sorted order, so we know we've never seen this header before
         */
        if (last_seen_header_name) {
            /*
             * there's a previous header, add appropriate separator in both canonical header buffer
             * and signed headers buffer
             */
            if (aws_byte_buf_append_byte_dynamic(canonical_header_buffer, '\n')) {
                return AWS_OP_ERR;
            }

            if (aws_byte_buf_append_byte_dynamic(signed_headers_buffer, ';')) {
                return AWS_OP_ERR;
            }
        }

        /* add it to the signed headers buffer */
        if (s_append_with_lookup(signed_headers_buffer, &header->name, to_lower_table)) {
            return AWS_OP_ERR;
        }

        /* add it to the canonical header buffer */
        if (s_append_with_lookup(canonical_header_buffer, &header->name, to_lower_table)) {
            return AWS_OP_ERR;
        }

        if (aws_byte_buf_append_byte_dynamic(canonical_header_buffer, ':')) {
            return AWS_OP_ERR;
        }
    } else {
        /* we've seen this header before, add a comma before appending the value */
        if (aws_byte_buf_append_byte_dynamic(canonical_header_buffer, ',')) {
            return AWS_OP_ERR;
        }
    }

    /*
     * This is the unsafe, non-append write of the header value where consecutive whitespace
     * is squashed into a single space.  Since this can only shrink the value length and we've
     * already reserved enough to hold the value, we can do raw buffer writes safely without
     * worrying about capacity.
     */
    struct aws_byte_cursor trimmed_value = aws_byte_cursor_trim_pred(&header->value, s_is_space);

    /* raw, unsafe write loop */
    bool in_space = false;
    uint8_t *start_ptr = trimmed_value.ptr;
    uint8_t *end_ptr = trimmed_value.ptr + trimmed_value.len;
    uint8_t *dest_ptr = canonical_header_buffer->buffer + canonical_header_buffer->len;
    while (start_ptr < end_ptr) {
        uint8_t value = *start_ptr;
        bool is_space = s_is_space(value);
        if (is_space) {
            value = ' ';
        }

        if (!is_space || !in_space) {
            *dest_ptr++ = value;
            ++canonical_header_buffer->len;
        }

        in_space = is_space;

        ++start_ptr;
    }

    return AWS_OP_SUCCESS;
}

/* Add header to stable_header_list to be canonicalized, and also to final signing result */
static int s_add_authorization_header(
    struct aws_signing_state_aws *state,
    struct aws_array_list *stable_header_list,
    size_t *out_required_capacity,
    struct aws_byte_cursor name,
    struct aws_byte_cursor value) {

    /* Add to stable_header_list to be canonicalized */
    struct stable_header stable_header = {
        .original_index = aws_array_list_length(stable_header_list),
        .header =
            {
                .name = name,
                .value = value,
            },
    };
    if (aws_array_list_push_back(stable_header_list, &stable_header)) {
        return AWS_OP_ERR;
    }

    /* Add to signing result */
    if (aws_signing_result_append_property_list(&state->result, g_aws_http_headers_property_list_name, &name, &value)) {
        return AWS_OP_ERR;
    }

    *out_required_capacity += name.len + value.len;
    return AWS_OP_SUCCESS;
}

/*
 * Builds the list of header name-value pairs to be added to the canonical request.  The list members are
 * actually the header wrapper structs that allow for stable sorting.
 *
 * Takes the original request headers, adds X-Amz-Date, and optionally, x-amz-content-sha256
 *
 * If we add filtering/exclusion support, this is where it would go
 */
static int s_build_canonical_stable_header_list(
    struct aws_signing_state_aws *state,
    struct aws_array_list *stable_header_list,
    size_t *out_required_capacity) {

    AWS_ASSERT(aws_array_list_length(stable_header_list) == 0);

    *out_required_capacity = 0;
    const struct aws_signable *signable = state->signable;

    /*
     * request headers
     */
    struct aws_array_list *signable_header_list = NULL;
    if (aws_signable_get_property_list(signable, g_aws_http_headers_property_list_name, &signable_header_list)) {
        return AWS_OP_ERR;
    }

    const size_t signable_header_count = aws_array_list_length(signable_header_list);
    for (size_t i = 0; i < signable_header_count; ++i) {
        struct stable_header header_wrapper;
        AWS_ZERO_STRUCT(header_wrapper);
        header_wrapper.original_index = i;

        if (aws_array_list_get_at(signable_header_list, &header_wrapper.header, i)) {
            return AWS_OP_ERR;
        }

        struct aws_byte_cursor *header_name_cursor = &header_wrapper.header.name;
        if (!s_should_sign_header(state, header_name_cursor)) {
            continue;
        }

        *out_required_capacity += header_wrapper.header.name.len + header_wrapper.header.value.len;

        if (aws_array_list_push_back(stable_header_list, &header_wrapper)) {
            return AWS_OP_ERR;
        }
    }

    /* If doing HEADERS signature type, add required X-Amz-*** headers.
     * NOTE: For QUERY_PARAMS signature type, X-Amz-*** params are added to query string instead. */
    if (state->config.signature_type == AWS_ST_HTTP_REQUEST_HEADERS) {

        /*
         * X-Amz-*-Token
         */
        /* We have different token between S3Express and other signing, which needs different token header name */
        struct aws_byte_cursor token_header_name;
        if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_S3EXPRESS) {
            /* X-Amz-S3session-Token */
            token_header_name = s_amz_s3session_token_header_name;
        } else {
            /* X-Amz-Security-Token */
            token_header_name = s_amz_security_token_header_name;
        }
        struct aws_byte_cursor session_token_cursor = aws_credentials_get_session_token(state->config.credentials);
        if (session_token_cursor.len > 0) {
            /* Note that if omit_session_token is true, it is added to final
             * signing result but NOT included in canonicalized headers. */
            if (state->config.flags.omit_session_token) {
                if (aws_signing_result_append_property_list(
                        &state->result,
                        g_aws_http_headers_property_list_name,
                        &token_header_name,
                        &session_token_cursor)) {
                    return AWS_OP_ERR;
                }
            } else {
                if (s_add_authorization_header(
                        state, stable_header_list, out_required_capacity, token_header_name, session_token_cursor)) {
                    return AWS_OP_ERR;
                }
            }
        }

        /*
         * X-Amz-Date
         */
        if (s_add_authorization_header(
                state,
                stable_header_list,
                out_required_capacity,
                s_amz_date_header_name,
                aws_byte_cursor_from_buf(&state->date))) {
            return AWS_OP_ERR;
        }

        *out_required_capacity += g_aws_signing_date_name->len + state->date.len;

        /*
         * x-amz-region-set
         */
        if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
            if (s_add_authorization_header(
                    state,
                    stable_header_list,
                    out_required_capacity,
                    aws_byte_cursor_from_string(g_aws_signing_region_set_name),
                    state->config.region)) {
                return AWS_OP_ERR;
            }
        }

        /*
         * x-amz-content-sha256 (optional)
         */
        if (state->config.signed_body_header == AWS_SBHT_X_AMZ_CONTENT_SHA256) {
            if (s_add_authorization_header(
                    state,
                    stable_header_list,
                    out_required_capacity,
                    s_amz_content_sha256_header_name,
                    aws_byte_cursor_from_buf(&state->payload_hash))) {
                return AWS_OP_ERR;
            }
        }

        /* NOTE: Update MAX_AUTHORIZATION_HEADER_COUNT if more headers added */
    } else if (
        state->config.signature_type == AWS_ST_HTTP_REQUEST_QUERY_PARAMS &&
        aws_byte_cursor_eq_c_str(&state->config.service, "vpc-lattice-svcs")) {
        /* NOTES: TEMPORAY WORKAROUND FOR VPC Lattice. SHALL BE REMOVED IN NEAR FUTURE */
        /* Add unsigned payload as `x-amz-content-sha256` header to the canonical request when signing through query
         * params.  */
        if (s_add_authorization_header(
                state,
                stable_header_list,
                out_required_capacity,
                s_amz_content_sha256_header_name,
                g_aws_signed_body_value_unsigned_payload)) {
            return AWS_OP_ERR;
        }
    }

    *out_required_capacity += aws_array_list_length(stable_header_list) * 2; /*  ':' + '\n' per header */

    return AWS_OP_SUCCESS;
}

static int s_validate_signable_header_list(struct aws_array_list *header_list) {
    const size_t header_count = aws_array_list_length(header_list);
    for (size_t i = 0; i < header_count; ++i) {
        struct aws_signable_property_list_pair header;
        AWS_ZERO_STRUCT(header);

        aws_array_list_get_at(header_list, &header, i);

        struct aws_hash_element *forbidden_element = NULL;
        aws_hash_table_find(&s_forbidden_headers, &header.name, &forbidden_element);

        if (forbidden_element != NULL) {
            AWS_LOGF_ERROR(
                AWS_LS_AUTH_SIGNING,
                "AWS authorization header \"" PRInSTR "\" found in request while signing",
                AWS_BYTE_CURSOR_PRI(header.name));
            return aws_raise_error(AWS_AUTH_SIGNING_ILLEGAL_REQUEST_HEADER);
        }
    }

    return AWS_OP_SUCCESS;
}

static int s_canonicalize_headers(struct aws_signing_state_aws *state) {
    const struct aws_signable *signable = state->signable;
    struct aws_allocator *allocator = state->allocator;
    struct aws_byte_buf *header_buffer = &state->canonical_header_block;

    AWS_ASSERT(header_buffer->len == 0);

    int result = AWS_OP_ERR;

    struct aws_array_list *signable_header_list = NULL;
    if (aws_signable_get_property_list(signable, g_aws_http_headers_property_list_name, &signable_header_list)) {
        return AWS_OP_ERR;
    }

    if (s_validate_signable_header_list(signable_header_list)) {
        return AWS_OP_ERR;
    }

    const size_t signable_header_count = aws_array_list_length(signable_header_list);

    /* Overestimate capacity to avoid re-allocation */
    size_t headers_reserve_count = signable_header_count + MAX_AUTHORIZATION_HEADER_COUNT;

    struct aws_array_list headers;
    if (aws_array_list_init_dynamic(&headers, allocator, headers_reserve_count, sizeof(struct stable_header))) {
        return AWS_OP_ERR;
    }

    size_t header_buffer_reserve_size = 0;
    if (s_build_canonical_stable_header_list(state, &headers, &header_buffer_reserve_size)) {
        goto on_cleanup;
    }

    /*
     * Make sure there's enough room in the request buffer to hold a conservative overestimate of the room
     * needed for canonical headers.  There are places we'll be using an append function that does not resize.
     */
    if (aws_byte_buf_reserve(header_buffer, header_buffer_reserve_size)) {
        return AWS_OP_ERR;
    }

    const size_t header_count = aws_array_list_length(&headers);

    /* Sort the arraylist via lowercase header name and original position */
    qsort(headers.data, header_count, sizeof(struct stable_header), s_canonical_header_comparator);

    /* Iterate the sorted list, writing the canonical representation into the request */
    struct aws_byte_cursor *last_seen_header_name = NULL;
    for (size_t i = 0; i < header_count; ++i) {
        struct stable_header *wrapper = NULL;
        if (aws_array_list_get_at_ptr(&headers, (void **)&wrapper, i)) {
            goto on_cleanup;
        }

        if (s_append_canonical_header(state, &wrapper->header, last_seen_header_name)) {
            goto on_cleanup;
        }

        last_seen_header_name = &wrapper->header.name;
    }

    /* check for count greater than zero in case someone attempts to canonicalize an empty list of trailing headers */
    /* There's always at least one header entry (X-Amz-Date), end the last one */
    if (header_count > 0) {
        if (aws_byte_buf_append_byte_dynamic(header_buffer, '\n')) {
            return AWS_OP_ERR;
        }
    }

    result = AWS_OP_SUCCESS;

on_cleanup:

    aws_array_list_clean_up(&headers);

    return result;
}

static int s_append_signed_headers(struct aws_signing_state_aws *state) {

    struct aws_byte_buf *header_buffer = &state->canonical_header_block;
    struct aws_byte_buf *signed_headers_buffer = &state->signed_headers;

    if (aws_byte_buf_append_byte_dynamic(header_buffer, '\n')) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signed_headers_cursor = aws_byte_cursor_from_buf(signed_headers_buffer);
    if (aws_byte_buf_append_dynamic(header_buffer, &signed_headers_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(header_buffer, '\n')) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Top-level-ish function to write the canonical header set into a buffer as well as the signed header names
 * into a separate buffer.  We do this very early in the canonical request construction process so that the
 * query params processing has the signed header names available to it.
 */
static int s_build_canonical_headers(struct aws_signing_state_aws *state) {
    if (s_canonicalize_headers(state)) {
        return AWS_OP_ERR;
    }
    if (s_append_signed_headers(state)) {
        return AWS_OP_ERR;
    }
    return AWS_OP_SUCCESS;
}

/*
 * Computes the canonical request payload value.
 */
static int s_build_canonical_payload(struct aws_signing_state_aws *state) {
    const struct aws_signable *signable = state->signable;
    struct aws_allocator *allocator = state->allocator;
    struct aws_byte_buf *payload_hash_buffer = &state->payload_hash;

    AWS_ASSERT(payload_hash_buffer->len == 0);

    struct aws_byte_buf body_buffer;
    AWS_ZERO_STRUCT(body_buffer);
    struct aws_byte_buf digest_buffer;
    AWS_ZERO_STRUCT(digest_buffer);

    struct aws_hash *hash = NULL;

    int result = AWS_OP_ERR;
    if (state->config.signature_type == AWS_ST_HTTP_REQUEST_QUERY_PARAMS &&
        aws_byte_cursor_eq_c_str(&state->config.service, "vpc-lattice-svcs")) {
        /* NOTES: TEMPORAY WORKAROUND FOR VPC Lattice. SHALL BE REMOVED IN NEAR FUTURE */
        /* ALWAYS USE UNSIGNED-PAYLOAD FOR VPC Lattice.  */
        if (aws_byte_buf_append_dynamic(payload_hash_buffer, &g_aws_signed_body_value_unsigned_payload) ==
            AWS_OP_SUCCESS) {
            result = AWS_OP_SUCCESS;
        }
        goto on_cleanup;
    }

    if (state->config.signed_body_value.len == 0) {
        /* No value provided by user, so we must calculate it */
        hash = aws_sha256_new(allocator);
        if (hash == NULL) {
            return AWS_OP_ERR;
        }

        if (aws_byte_buf_init(&body_buffer, allocator, BODY_READ_BUFFER_SIZE) ||
            aws_byte_buf_init(&digest_buffer, allocator, AWS_SHA256_LEN)) {
            goto on_cleanup;
        }

        struct aws_input_stream *payload_stream = NULL;
        if (aws_signable_get_payload_stream(signable, &payload_stream)) {
            goto on_cleanup;
        }

        if (payload_stream != NULL) {
            if (aws_input_stream_seek(payload_stream, 0, AWS_SSB_BEGIN)) {
                goto on_cleanup;
            }

            struct aws_stream_status payload_status;
            AWS_ZERO_STRUCT(payload_status);

            while (!payload_status.is_end_of_stream) {
                /* reset the temporary body buffer; we can calculate the hash in window chunks */
                body_buffer.len = 0;
                if (aws_input_stream_read(payload_stream, &body_buffer)) {
                    goto on_cleanup;
                }

                if (body_buffer.len > 0) {
                    struct aws_byte_cursor body_cursor = aws_byte_cursor_from_buf(&body_buffer);
                    aws_hash_update(hash, &body_cursor);
                }

                if (aws_input_stream_get_status(payload_stream, &payload_status)) {
                    goto on_cleanup;
                }
            }

            /* reset the input stream for sending */
            if (aws_input_stream_seek(payload_stream, 0, AWS_SSB_BEGIN)) {
                goto on_cleanup;
            }
        }

        if (aws_hash_finalize(hash, &digest_buffer, 0)) {
            goto on_cleanup;
        }

        struct aws_byte_cursor digest_cursor = aws_byte_cursor_from_buf(&digest_buffer);
        if (aws_hex_encode_append_dynamic(&digest_cursor, payload_hash_buffer)) {
            goto on_cleanup;
        }
    } else {
        /* Use value provided in config */
        if (aws_byte_buf_append_dynamic(payload_hash_buffer, &state->config.signed_body_value)) {
            goto on_cleanup;
        }
    }

    result = AWS_OP_SUCCESS;

on_cleanup:

    aws_byte_buf_clean_up(&digest_buffer);
    aws_byte_buf_clean_up(&body_buffer);

    if (hash) {
        aws_hash_destroy(hash);
    }

    return result;
}

/*
 * Copies the previously-computed payload hash into the canonical request buffer
 */
static int s_append_canonical_payload_hash(struct aws_signing_state_aws *state) {
    struct aws_byte_buf *canonical_request_buffer = &state->canonical_request;
    struct aws_byte_buf *payload_hash_buffer = &state->payload_hash;

    /*
     * Copy the hex-encoded payload hash into the canonical request
     */
    struct aws_byte_cursor payload_hash_cursor = aws_byte_cursor_from_buf(payload_hash_buffer);
    if (aws_byte_buf_append_dynamic(canonical_request_buffer, &payload_hash_cursor)) {
        return AWS_OP_ERR;
    }

    /* Sigv4 spec claims a newline should be included after the payload, but the implementation doesn't do this */

    return AWS_OP_SUCCESS;
}

AWS_STATIC_STRING_FROM_LITERAL(s_credential_scope_sigv4_terminator, "aws4_request");

static int s_append_credential_scope_terminator(enum aws_signing_algorithm algorithm, struct aws_byte_buf *dest) {
    struct aws_byte_cursor terminator_cursor;

    switch (algorithm) {
        case AWS_SIGNING_ALGORITHM_V4:
        case AWS_SIGNING_ALGORITHM_V4_S3EXPRESS:
        case AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC:
            terminator_cursor = aws_byte_cursor_from_string(s_credential_scope_sigv4_terminator);
            break;

        default:
            return aws_raise_error(AWS_AUTH_SIGNING_UNSUPPORTED_ALGORITHM);
    }

    return aws_byte_buf_append_dynamic(dest, &terminator_cursor);
}

/*
 * Builds the credential scope string by appending a bunch of things together:
 *   Date, region, service, algorithm terminator
 */
static int s_build_credential_scope(struct aws_signing_state_aws *state) {
    AWS_ASSERT(state->credential_scope.len == 0);

    const struct aws_signing_config_aws *config = &state->config;
    struct aws_byte_buf *dest = &state->credential_scope;

    /*
     * date output uses the non-dynamic append, so make sure there's enough room first
     */
    if (aws_byte_buf_reserve_relative(dest, AWS_DATE_TIME_STR_MAX_LEN)) {
        return AWS_OP_ERR;
    }

    if (aws_date_time_to_utc_time_short_str(&config->date, AWS_DATE_FORMAT_ISO_8601_BASIC, dest)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
        return AWS_OP_ERR;
    }

    if (config->algorithm != AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
        if (aws_byte_buf_append_dynamic(dest, &config->region)) {
            return AWS_OP_ERR;
        }

        if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
            return AWS_OP_ERR;
        }
    }

    if (aws_byte_buf_append_dynamic(dest, &config->service)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
        return AWS_OP_ERR;
    }

    if (s_append_credential_scope_terminator(state->config.algorithm, dest)) {
        return AWS_OP_ERR;
    }

    /* While we're at it, build the accesskey/credential scope string which is used during query param signing*/
    struct aws_byte_cursor access_key_cursor = aws_credentials_get_access_key_id(state->config.credentials);
    if (aws_byte_buf_append_dynamic(&state->access_credential_scope, &access_key_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(&state->access_credential_scope, '/')) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor credential_scope_cursor = aws_byte_cursor_from_buf(&state->credential_scope);
    if (aws_byte_buf_append_dynamic(&state->access_credential_scope, &credential_scope_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Hashes the canonical request and stores its hex representation
 */
static int s_build_canonical_request_hash(struct aws_signing_state_aws *state) {
    struct aws_allocator *allocator = state->allocator;
    struct aws_byte_buf *dest = &state->string_to_sign_payload;

    int result = AWS_OP_ERR;

    struct aws_byte_buf digest_buffer;
    AWS_ZERO_STRUCT(digest_buffer);

    if (aws_byte_buf_init(&digest_buffer, allocator, AWS_SHA256_LEN)) {
        goto cleanup;
    }

    struct aws_byte_cursor canonical_request_cursor = aws_byte_cursor_from_buf(&state->canonical_request);
    if (aws_sha256_compute(allocator, &canonical_request_cursor, &digest_buffer, 0)) {
        goto cleanup;
    }

    struct aws_byte_cursor digest_cursor = aws_byte_cursor_from_buf(&digest_buffer);
    if (aws_hex_encode_append_dynamic(&digest_cursor, dest)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:
    aws_byte_buf_clean_up(&digest_buffer);

    return result;
}

/**
 * Note that there is no canonical request for event signing.
 * The string to sign for events is detailed here:
 * https://docs.aws.amazon.com/transcribe/latest/dg/streaming-http2.html
 *
 *      String stringToSign =
 *      "AWS4-HMAC-SHA256" +
 *      "\n" +
 *      DateTime +
 *      "\n" +
 *      Keypath +
 *      "\n" +
 *      Hex(priorSignature) +
 *      "\n" +
 *      HexHash(nonSignatureHeaders) +
 *      "\n" +
 *      HexHash(payload);
 *
 * This function will build the string_to_sign_payload,
 * aka "everything after the Keypath line in the string to sign".
 */
static int s_build_string_to_sign_payload_for_event(struct aws_signing_state_aws *state) {
    int result = AWS_OP_ERR;

    struct aws_byte_buf *dest = &state->string_to_sign_payload;

    /*
     * Hex(priorSignature) + "\n"
     *
     * Fortunately, the prior signature is already hex.
     */
    struct aws_byte_cursor prev_signature_cursor;
    AWS_ZERO_STRUCT(prev_signature_cursor);
    if (aws_signable_get_property(state->signable, g_aws_previous_signature_property_name, &prev_signature_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING, "(id=%p) Event signable missing previous signature property", (void *)state->signable);
        return aws_raise_error(AWS_AUTH_SIGNING_MISSING_PREVIOUS_SIGNATURE);
    }

    /* strip any padding (AWS_SIGV4A_SIGNATURE_PADDING_BYTE) from the previous signature */
    prev_signature_cursor = aws_trim_padded_sigv4a_signature(prev_signature_cursor);

    if (aws_byte_buf_append_dynamic(dest, &prev_signature_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    /*
     * HexHash(nonSignatureHeaders) + "\n"
     *
     * nonSignatureHeaders is just the ":date" header.
     * We need to encode these headers in event-stream format, as described here:
     * https://docs.aws.amazon.com/transcribe/latest/dg/streaming-setting-up.html
     *
     * | Header Name Length | Header Name | Header Value Type | Header Value Length | Header Value |
     * |       1 byte       |   N bytes   |       1 byte      |        2 bytes      |    N bytes   |
     */
    struct aws_byte_buf date_buffer;
    AWS_ZERO_STRUCT(date_buffer);
    struct aws_byte_buf digest_buffer;
    AWS_ZERO_STRUCT(digest_buffer);

    if (aws_byte_buf_init(&date_buffer, state->allocator, 15)) {
        goto cleanup;
    }

    struct aws_byte_cursor header_name = aws_byte_cursor_from_c_str(":date");
    AWS_FATAL_ASSERT(aws_byte_buf_write_u8(&date_buffer, (uint8_t)header_name.len));
    if (aws_byte_buf_append_dynamic(&date_buffer, &header_name)) {
        goto cleanup;
    }

    /* Type of timestamp header */
    AWS_FATAL_ASSERT(aws_byte_buf_write_u8(&date_buffer, 8 /*AWS_EVENT_STREAM_HEADER_TIMESTAMP*/));
    AWS_FATAL_ASSERT(aws_byte_buf_write_be64(&date_buffer, (int64_t)aws_date_time_as_millis(&state->config.date)));

    /* calculate sha 256 of encoded buffer */
    if (aws_byte_buf_init(&digest_buffer, state->allocator, AWS_SHA256_LEN)) {
        goto cleanup;
    }

    struct aws_byte_cursor date_cursor = aws_byte_cursor_from_buf(&date_buffer);
    if (aws_sha256_compute(state->allocator, &date_cursor, &digest_buffer, 0)) {
        goto cleanup;
    }

    struct aws_byte_cursor digest_cursor = aws_byte_cursor_from_buf(&digest_buffer);
    if (aws_hex_encode_append_dynamic(&digest_cursor, dest)) {
        goto cleanup;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        goto cleanup;
    }

    /*
     * HexHash(payload);
     *
     * The payload was already hashed in an earlier stage
     */
    struct aws_byte_cursor current_chunk_hash_cursor = aws_byte_cursor_from_buf(&state->payload_hash);
    if (aws_byte_buf_append_dynamic(dest, &current_chunk_hash_cursor)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:
    aws_byte_buf_clean_up(&date_buffer);
    aws_byte_buf_clean_up(&digest_buffer);

    return result;
}

static int s_build_canonical_request_body_chunk(struct aws_signing_state_aws *state) {

    struct aws_byte_buf *dest = &state->string_to_sign_payload;

    /* previous signature + \n */
    struct aws_byte_cursor prev_signature_cursor;
    AWS_ZERO_STRUCT(prev_signature_cursor);
    if (aws_signable_get_property(state->signable, g_aws_previous_signature_property_name, &prev_signature_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING, "(id=%p) Chunk signable missing previous signature property", (void *)state->signable);
        return aws_raise_error(AWS_AUTH_SIGNING_MISSING_PREVIOUS_SIGNATURE);
    }

    /* strip any padding (AWS_SIGV4A_SIGNATURE_PADDING_BYTE) from the previous signature */
    prev_signature_cursor = aws_trim_padded_sigv4a_signature(prev_signature_cursor);

    if (aws_byte_buf_append_dynamic(dest, &prev_signature_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    /* empty hash + \n */
    if (aws_byte_buf_append_dynamic(dest, &g_aws_signed_body_value_empty_sha256)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    /* current hash */
    struct aws_byte_cursor current_chunk_hash_cursor = aws_byte_cursor_from_buf(&state->payload_hash);
    if (aws_byte_buf_append_dynamic(dest, &current_chunk_hash_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

static int s_build_canonical_request_trailing_headers(struct aws_signing_state_aws *state) {

    struct aws_byte_buf *dest = &state->string_to_sign_payload;

    /* previous signature + \n */
    struct aws_byte_cursor prev_signature_cursor;
    AWS_ZERO_STRUCT(prev_signature_cursor);
    if (aws_signable_get_property(state->signable, g_aws_previous_signature_property_name, &prev_signature_cursor)) {
        AWS_LOGF_ERROR(
            AWS_LS_AUTH_SIGNING,
            "(id=%p) trailing_headers signable missing previous signature property",
            (void *)state->signable);
        return aws_raise_error(AWS_AUTH_SIGNING_MISSING_PREVIOUS_SIGNATURE);
    }

    /* strip any padding (AWS_SIGV4A_SIGNATURE_PADDING_BYTE) from the previous signature */
    prev_signature_cursor = aws_trim_padded_sigv4a_signature(prev_signature_cursor);

    if (aws_byte_buf_append_dynamic(dest, &prev_signature_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    /* current hash */

    if (s_canonicalize_headers(state)) {
        return AWS_OP_ERR;
    }
    struct aws_byte_cursor header_block_cursor = aws_byte_cursor_from_buf(&state->canonical_header_block);
    if (aws_byte_buf_append_dynamic(&state->canonical_request, &header_block_cursor)) {
        return AWS_OP_ERR;
    }
    if (s_build_canonical_request_hash(state)) {
        return AWS_OP_ERR;
    }
    return AWS_OP_SUCCESS;
}

/*
 * Builds a sigv4-signed canonical request and its hashed value
 */
static int s_build_canonical_request_sigv4(struct aws_signing_state_aws *state) {
    AWS_ASSERT(state->canonical_request.len == 0);
    AWS_ASSERT(state->payload_hash.len > 0);

    int result = AWS_OP_ERR;

    struct aws_uri uri;
    AWS_ZERO_STRUCT(uri);

    struct aws_byte_cursor uri_cursor;
    if (aws_signable_get_property(state->signable, g_aws_http_uri_property_name, &uri_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_uri_init_parse(&uri, state->allocator, &uri_cursor)) {
        goto cleanup;
    }

    if (s_build_canonical_headers(state)) {
        goto cleanup;
    }

    if (s_append_canonical_method(state)) {
        goto cleanup;
    }

    if (s_append_canonical_path(&uri, state)) {
        goto cleanup;
    }

    if (s_append_canonical_query_string(&uri, state)) {
        goto cleanup;
    }

    struct aws_byte_cursor header_block_cursor = aws_byte_cursor_from_buf(&state->canonical_header_block);
    if (aws_byte_buf_append_dynamic(&state->canonical_request, &header_block_cursor)) {
        goto cleanup;
    }

    if (s_append_canonical_payload_hash(state)) {
        goto cleanup;
    }

    if (s_build_canonical_request_hash(state)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_uri_clean_up(&uri);

    return result;
}

/*
 * The canonical header list is the next-to-the-last line on the canonical request, so split by lines and take
 * the penultimate value.
 */
static struct aws_byte_cursor s_get_signed_headers_from_canonical_request(
    struct aws_allocator *allocator,
    struct aws_byte_cursor canonical_request) {

    struct aws_byte_cursor header_cursor;
    AWS_ZERO_STRUCT(header_cursor);

    struct aws_array_list splits;
    AWS_ZERO_STRUCT(splits);

    if (aws_array_list_init_dynamic(
            &splits, allocator, CANONICAL_REQUEST_SPLIT_OVER_ESTIMATE, sizeof(struct aws_byte_cursor))) {
        return header_cursor;
    }

    if (aws_byte_cursor_split_on_char(&canonical_request, '\n', &splits)) {
        goto done;
    }

    size_t split_count = aws_array_list_length(&splits);

    if (split_count > 1) {
        aws_array_list_get_at(&splits, &header_cursor, split_count - 2);
    }

done:

    aws_array_list_clean_up(&splits);

    return header_cursor;
}

/*
 * Fill in the signing state values needed by later stages that computing the canonical request would have done.
 */
static int s_apply_existing_canonical_request(struct aws_signing_state_aws *state) {

    struct aws_byte_cursor canonical_request_cursor;
    AWS_ZERO_STRUCT(canonical_request_cursor);
    if (aws_signable_get_property(state->signable, g_aws_canonical_request_property_name, &canonical_request_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_dynamic(&state->canonical_request, &canonical_request_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signed_headers_cursor =
        s_get_signed_headers_from_canonical_request(state->allocator, canonical_request_cursor);
    if (aws_byte_buf_append_dynamic(&state->signed_headers, &signed_headers_cursor)) {
        return AWS_OP_ERR;
    }

    if (s_build_canonical_request_hash(state)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Top-level canonical request construction function.
 * For signature types not associated directly with an http request (chunks, events), this calculates the
 * string-to-sign payload that replaces the hashed canonical request in those signing procedures.
 */
int aws_signing_build_canonical_request(struct aws_signing_state_aws *state) {

    if (aws_date_time_to_utc_time_str(&state->config.date, AWS_DATE_FORMAT_ISO_8601_BASIC, &state->date)) {
        return AWS_OP_ERR;
    }

    if (s_build_canonical_payload(state)) {
        return AWS_OP_ERR;
    }

    if (s_build_credential_scope(state)) {
        return AWS_OP_ERR;
    }

    switch (state->config.signature_type) {
        case AWS_ST_HTTP_REQUEST_HEADERS:
        case AWS_ST_HTTP_REQUEST_QUERY_PARAMS:
            return s_build_canonical_request_sigv4(state);

        case AWS_ST_HTTP_REQUEST_CHUNK:
            return s_build_canonical_request_body_chunk(state);
        case AWS_ST_HTTP_REQUEST_EVENT:
            return s_build_string_to_sign_payload_for_event(state);

        case AWS_ST_HTTP_REQUEST_TRAILING_HEADERS:
            return s_build_canonical_request_trailing_headers(state);

        case AWS_ST_CANONICAL_REQUEST_HEADERS:
        case AWS_ST_CANONICAL_REQUEST_QUERY_PARAMS:
            return s_apply_existing_canonical_request(state);

        default:
            return aws_raise_error(AWS_AUTH_SIGNING_UNSUPPORTED_SIGNATURE_TYPE);
    }
}

/*
 * Top-level function for computing the string-to-sign in an AWS signing process.
 */
int aws_signing_build_string_to_sign(struct aws_signing_state_aws *state) {
    /* We must have a canonical request and the credential scope.  We must not have the string to sign */
    AWS_ASSERT(state->string_to_sign_payload.len > 0);
    AWS_ASSERT(state->credential_scope.len > 0);
    AWS_ASSERT(state->string_to_sign.len == 0);

    struct aws_byte_buf *dest = &state->string_to_sign;

    if (s_append_sts_signature_type(state, dest)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    /*  date_time output uses raw array writes, so ensure there's enough room beforehand */
    if (aws_byte_buf_reserve_relative(dest, AWS_DATE_TIME_STR_MAX_LEN)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor date_cursor = aws_byte_cursor_from_buf(&state->date);
    if (aws_byte_buf_append_dynamic(dest, &date_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor credential_scope_cursor = aws_byte_cursor_from_buf(&state->credential_scope);
    if (aws_byte_buf_append_dynamic(dest, &credential_scope_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '\n')) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor sts_payload_cursor = aws_byte_cursor_from_buf(&state->string_to_sign_payload);
    if (aws_byte_buf_append_dynamic(dest, &sts_payload_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Signature calculation utility functions
 */

AWS_STATIC_STRING_FROM_LITERAL(s_secret_key_prefix, "AWS4");

/*
 * Computes the key to sign with as a function of the secret access key in the credentials and
 *  the components of the credential scope: date, region, service, algorithm terminator
 */
static int s_compute_sigv4_signing_key(struct aws_signing_state_aws *state, struct aws_byte_buf *dest) {
    /* dest should be empty */
    AWS_ASSERT(dest->len == 0);

    const struct aws_signing_config_aws *config = &state->config;
    struct aws_allocator *allocator = state->allocator;

    int result = AWS_OP_ERR;

    struct aws_byte_buf secret_key;
    AWS_ZERO_STRUCT(secret_key);

    struct aws_byte_buf output;
    AWS_ZERO_STRUCT(output);

    struct aws_byte_buf date_buf;
    AWS_ZERO_STRUCT(date_buf);

    struct aws_byte_cursor secret_access_key_cursor = aws_credentials_get_secret_access_key(state->config.credentials);
    if (aws_byte_buf_init(&secret_key, allocator, s_secret_key_prefix->len + secret_access_key_cursor.len) ||
        aws_byte_buf_init(&output, allocator, AWS_SHA256_LEN) ||
        aws_byte_buf_init(&date_buf, allocator, AWS_DATE_TIME_STR_MAX_LEN)) {
        goto cleanup;
    }

    /*
     * Prep Key
     */
    struct aws_byte_cursor prefix_cursor = aws_byte_cursor_from_string(s_secret_key_prefix);
    if (aws_byte_buf_append_dynamic(&secret_key, &prefix_cursor) ||
        aws_byte_buf_append_dynamic(&secret_key, &secret_access_key_cursor)) {
        goto cleanup;
    }

    /*
     * Prep date
     */
    if (aws_date_time_to_utc_time_short_str(&config->date, AWS_DATE_FORMAT_ISO_8601_BASIC, &date_buf)) {
        goto cleanup;
    }

    struct aws_byte_cursor date_cursor = aws_byte_cursor_from_buf(&date_buf);
    struct aws_byte_cursor secret_key_cursor = aws_byte_cursor_from_buf(&secret_key);
    if (aws_sha256_hmac_compute(allocator, &secret_key_cursor, &date_cursor, &output, 0)) {
        goto cleanup;
    }

    struct aws_byte_cursor chained_key_cursor = aws_byte_cursor_from_buf(&output);
    output.len = 0; /* necessary evil part 1*/
    if (aws_sha256_hmac_compute(allocator, &chained_key_cursor, &config->region, &output, 0)) {
        goto cleanup;
    }

    chained_key_cursor = aws_byte_cursor_from_buf(&output);
    output.len = 0; /* necessary evil part 2 */
    if (aws_sha256_hmac_compute(allocator, &chained_key_cursor, &config->service, &output, 0)) {
        goto cleanup;
    }

    chained_key_cursor = aws_byte_cursor_from_buf(&output);
    struct aws_byte_cursor scope_terminator_cursor = aws_byte_cursor_from_string(s_credential_scope_sigv4_terminator);
    if (aws_sha256_hmac_compute(allocator, &chained_key_cursor, &scope_terminator_cursor, dest, 0)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:
    aws_byte_buf_clean_up_secure(&secret_key);
    aws_byte_buf_clean_up(&output);
    aws_byte_buf_clean_up(&date_buf);

    return result;
}

/*
 * Calculates the hex-encoding of the final signature value from the sigv4 signing process
 */
static int s_calculate_sigv4_signature_value(struct aws_signing_state_aws *state) {
    struct aws_allocator *allocator = state->allocator;

    int result = AWS_OP_ERR;

    struct aws_byte_buf key;
    AWS_ZERO_STRUCT(key);

    struct aws_byte_buf digest;
    AWS_ZERO_STRUCT(digest);

    if (aws_byte_buf_init(&key, allocator, AWS_SHA256_LEN) || aws_byte_buf_init(&digest, allocator, AWS_SHA256_LEN)) {
        goto cleanup;
    }

    if (s_compute_sigv4_signing_key(state, &key)) {
        goto cleanup;
    }

    struct aws_byte_cursor key_cursor = aws_byte_cursor_from_buf(&key);
    struct aws_byte_cursor string_to_sign_cursor = aws_byte_cursor_from_buf(&state->string_to_sign);
    if (aws_sha256_hmac_compute(allocator, &key_cursor, &string_to_sign_cursor, &digest, 0)) {
        goto cleanup;
    }

    struct aws_byte_cursor digest_cursor = aws_byte_cursor_from_buf(&digest);
    if (aws_hex_encode_append_dynamic(&digest_cursor, &state->signature)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_byte_buf_clean_up(&key);
    aws_byte_buf_clean_up(&digest);

    return result;
}

/*
 * Calculates the hex-encoding of the final signature value from the sigv4a signing process
 */
static int s_calculate_sigv4a_signature_value(struct aws_signing_state_aws *state) {
    struct aws_allocator *allocator = state->allocator;

    int result = AWS_OP_ERR;

    struct aws_byte_buf ecdsa_digest;
    AWS_ZERO_STRUCT(ecdsa_digest);

    struct aws_byte_buf sha256_digest;
    AWS_ZERO_STRUCT(sha256_digest);

    struct aws_ecc_key_pair *ecc_key = aws_credentials_get_ecc_key_pair(state->config.credentials);
    if (ecc_key == NULL) {
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CREDENTIALS);
    }

    if (aws_byte_buf_init(&ecdsa_digest, allocator, aws_ecc_key_pair_signature_length(ecc_key)) ||
        aws_byte_buf_init(&sha256_digest, allocator, AWS_SHA256_LEN)) {
        goto cleanup;
    }

    struct aws_byte_cursor string_to_sign_cursor = aws_byte_cursor_from_buf(&state->string_to_sign);
    if (aws_sha256_compute(allocator, &string_to_sign_cursor, &sha256_digest, 0)) {
        goto cleanup;
    }

    struct aws_byte_cursor sha256_digest_cursor = aws_byte_cursor_from_buf(&sha256_digest);
    if (aws_ecc_key_pair_sign_message(ecc_key, &sha256_digest_cursor, &ecdsa_digest)) {
        goto cleanup;
    }

    struct aws_byte_cursor ecdsa_digest_cursor = aws_byte_cursor_from_buf(&ecdsa_digest);
    if (aws_hex_encode_append_dynamic(&ecdsa_digest_cursor, &state->signature)) {
        goto cleanup;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_byte_buf_clean_up(&ecdsa_digest);
    aws_byte_buf_clean_up(&sha256_digest);

    return result;
}

/*
 * Appends a final signature value to a buffer based on the requested signing algorithm
 */
int s_calculate_signature_value(struct aws_signing_state_aws *state) {
    switch (state->config.algorithm) {
        case AWS_SIGNING_ALGORITHM_V4:
        case AWS_SIGNING_ALGORITHM_V4_S3EXPRESS:
            return s_calculate_sigv4_signature_value(state);

        case AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC:
            return s_calculate_sigv4a_signature_value(state);

        default:
            return aws_raise_error(AWS_AUTH_SIGNING_UNSUPPORTED_ALGORITHM);
    }
}

static int s_add_signature_property_to_result_set(struct aws_signing_state_aws *state) {

    int result = AWS_OP_ERR;

    struct aws_byte_buf final_signature_buffer;
    AWS_ZERO_STRUCT(final_signature_buffer);

    if (aws_byte_buf_init(&final_signature_buffer, state->allocator, HEX_ENCODED_SIGNATURE_OVER_ESTIMATE)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signature_value = aws_byte_cursor_from_buf(&state->signature);
    if (aws_byte_buf_append_dynamic(&final_signature_buffer, &signature_value)) {
        goto cleanup;
    }

    if (state->config.algorithm == AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC &&
        (state->config.signature_type == AWS_ST_HTTP_REQUEST_CHUNK ||
         state->config.signature_type == AWS_ST_HTTP_REQUEST_TRAILING_HEADERS)) {
        if (aws_byte_buf_reserve(&final_signature_buffer, MAX_ECDSA_P256_SIGNATURE_AS_HEX_LENGTH)) {
            goto cleanup;
        }

        if (signature_value.len < MAX_ECDSA_P256_SIGNATURE_AS_HEX_LENGTH) {
            size_t padding_byte_count = MAX_ECDSA_P256_SIGNATURE_AS_HEX_LENGTH - signature_value.len;
            if (!aws_byte_buf_write_u8_n(
                    &final_signature_buffer, AWS_SIGV4A_SIGNATURE_PADDING_BYTE, padding_byte_count)) {
                goto cleanup;
            }
        }
    }

    signature_value = aws_byte_cursor_from_buf(&final_signature_buffer);
    if (aws_signing_result_set_property(&state->result, g_aws_signature_property_name, &signature_value)) {
        return AWS_OP_ERR;
    }

    result = AWS_OP_SUCCESS;

cleanup:

    aws_byte_buf_clean_up(&final_signature_buffer);

    return result;
}

/*
 * Adds the appropriate authorization header or query param to the signing result
 */
static int s_add_authorization_to_result(
    struct aws_signing_state_aws *state,
    struct aws_byte_buf *authorization_value) {
    struct aws_byte_cursor name;
    struct aws_byte_cursor value = aws_byte_cursor_from_buf(authorization_value);

    if (s_is_header_based_signature_value(state->config.signature_type)) {
        name = aws_byte_cursor_from_string(g_aws_signing_authorization_header_name);
        if (aws_signing_result_append_property_list(
                &state->result, g_aws_http_headers_property_list_name, &name, &value)) {
            return AWS_OP_ERR;
        }
    }

    if (s_is_query_param_based_signature_value(state->config.signature_type)) {
        name = aws_byte_cursor_from_string(g_aws_signing_authorization_query_param_name);
        if (aws_signing_result_append_property_list(
                &state->result, g_aws_http_query_params_property_list_name, &name, &value)) {
            return AWS_OP_ERR;
        }
    }

    /*
     * Unconditionally add the signature value as a top-level property.
     */
    if (s_add_signature_property_to_result_set(state)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

AWS_STATIC_STRING_FROM_LITERAL(s_credential_prefix, " Credential=");
AWS_STATIC_STRING_FROM_LITERAL(s_signed_headers_prefix, ", SignedHeaders=");
AWS_STATIC_STRING_FROM_LITERAL(s_signature_prefix, ", Signature=");

/*
 * The Authorization has a lot more than just the final signature value in it.  This function appends all those
 * other values together ala:
 *
 * "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20150830/us-east-1/service/aws4_request, SignedHeaders=host;x-amz-date,
 * Signature="
 *
 * The final header value is this with the signature value appended to the end.
 */
static int s_append_authorization_header_preamble(struct aws_signing_state_aws *state, struct aws_byte_buf *dest) {
    if (s_append_sts_signature_type(state, dest)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor credential_cursor = aws_byte_cursor_from_string(s_credential_prefix);
    if (aws_byte_buf_append_dynamic(dest, &credential_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor access_key_cursor = aws_credentials_get_access_key_id(state->config.credentials);
    if (aws_byte_buf_append_dynamic(dest, &access_key_cursor)) {
        return AWS_OP_ERR;
    }

    if (aws_byte_buf_append_byte_dynamic(dest, '/')) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor credential_scope_cursor = aws_byte_cursor_from_buf(&state->credential_scope);
    if (aws_byte_buf_append_dynamic(dest, &credential_scope_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signed_headers_prefix_cursor = aws_byte_cursor_from_string(s_signed_headers_prefix);
    if (aws_byte_buf_append_dynamic(dest, &signed_headers_prefix_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signed_headers_cursor = aws_byte_cursor_from_buf(&state->signed_headers);
    if (aws_byte_buf_append_dynamic(dest, &signed_headers_cursor)) {
        return AWS_OP_ERR;
    }

    struct aws_byte_cursor signature_prefix_cursor = aws_byte_cursor_from_string(s_signature_prefix);
    if (aws_byte_buf_append_dynamic(dest, &signature_prefix_cursor)) {
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

/*
 * Top-level function for constructing the final authorization header/query-param and adding it to the
 * signing result.
 */
int aws_signing_build_authorization_value(struct aws_signing_state_aws *state) {
    AWS_ASSERT(state->string_to_sign.len > 0);
    AWS_ASSERT(state->credential_scope.len > 0);

    int result = AWS_OP_ERR;

    struct aws_byte_buf authorization_value;

    if (aws_byte_buf_init(&authorization_value, state->allocator, AUTHORIZATION_VALUE_STARTING_SIZE)) {
        goto cleanup;
    }

    if (s_is_header_based_signature_value(state->config.signature_type) &&
        s_append_authorization_header_preamble(state, &authorization_value)) {
        goto cleanup;
    }

    if (s_calculate_signature_value(state)) {
        goto cleanup;
    }

    struct aws_byte_cursor signature_cursor = aws_byte_cursor_from_buf(&state->signature);
    if (aws_byte_buf_append_dynamic(&authorization_value, &signature_cursor)) {
        goto cleanup;
    }

    if (s_add_authorization_to_result(state, &authorization_value)) {
        goto cleanup;
    }

    AWS_LOGF_INFO(
        AWS_LS_AUTH_SIGNING,
        "(id=%p) Http request successfully built final authorization value via algorithm %s, with contents "
        "\n" PRInSTR "\n",
        (void *)state->signable,
        aws_signing_algorithm_to_string(state->config.algorithm),
        AWS_BYTE_BUF_PRI(authorization_value));

    result = AWS_OP_SUCCESS;

cleanup:
    aws_byte_buf_clean_up(&authorization_value);

    return result;
}

int aws_validate_v4a_authorization_value(
    struct aws_allocator *allocator,
    struct aws_ecc_key_pair *ecc_key,
    struct aws_byte_cursor string_to_sign_cursor,
    struct aws_byte_cursor signature_value_cursor) {

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_SIGNING,
        "(id=%p) Verifying v4a auth value: \n" PRInSTR "\n\nusing string-to-sign: \n" PRInSTR "\n\n",
        (void *)ecc_key,
        AWS_BYTE_CURSOR_PRI(signature_value_cursor),
        AWS_BYTE_CURSOR_PRI(string_to_sign_cursor));

    signature_value_cursor = aws_trim_padded_sigv4a_signature(signature_value_cursor);

    size_t binary_length = 0;
    if (aws_hex_compute_decoded_len(signature_value_cursor.len, &binary_length)) {
        return AWS_OP_ERR;
    }

    int result = AWS_OP_ERR;

    struct aws_byte_buf binary_signature;
    AWS_ZERO_STRUCT(binary_signature);

    struct aws_byte_buf sha256_digest;
    AWS_ZERO_STRUCT(sha256_digest);

    if (aws_byte_buf_init(&binary_signature, allocator, binary_length) ||
        aws_byte_buf_init(&sha256_digest, allocator, AWS_SHA256_LEN)) {
        goto done;
    }

    if (aws_hex_decode(&signature_value_cursor, &binary_signature)) {
        goto done;
    }

    if (aws_sha256_compute(allocator, &string_to_sign_cursor, &sha256_digest, 0)) {
        goto done;
    }

    struct aws_byte_cursor binary_signature_cursor =
        aws_byte_cursor_from_array(binary_signature.buffer, binary_signature.len);
    struct aws_byte_cursor digest_cursor = aws_byte_cursor_from_buf(&sha256_digest);
    if (aws_ecc_key_pair_verify_signature(ecc_key, &digest_cursor, &binary_signature_cursor)) {
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    aws_byte_buf_clean_up(&binary_signature);
    aws_byte_buf_clean_up(&sha256_digest);

    return result;
}

int aws_verify_sigv4a_signing(
    struct aws_allocator *allocator,
    const struct aws_signable *signable,
    const struct aws_signing_config_base *base_config,
    struct aws_byte_cursor expected_canonical_request_cursor,
    struct aws_byte_cursor signature_cursor,
    struct aws_byte_cursor ecc_key_pub_x,
    struct aws_byte_cursor ecc_key_pub_y) {

    int result = AWS_OP_ERR;

    if (base_config->config_type != AWS_SIGNING_CONFIG_AWS) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Signing config is not an AWS signing config");
        return aws_raise_error(AWS_AUTH_SIGNING_MISMATCHED_CONFIGURATION);
    }

    if (aws_validate_aws_signing_config_aws((void *)base_config)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Signing config failed validation");
        return aws_raise_error(AWS_AUTH_SIGNING_INVALID_CONFIGURATION);
    }

    const struct aws_signing_config_aws *config = (void *)base_config;
    if (config->algorithm != AWS_SIGNING_ALGORITHM_V4_ASYMMETRIC) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Signing algorithm is not V4_ASYMMETRIC");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (config->credentials == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "AWS credentials were not provided/null");
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    struct aws_signing_state_aws *signing_state = aws_signing_state_new(allocator, config, signable, NULL, NULL);
    if (!signing_state) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Unable to create new signing state");
        return AWS_OP_ERR;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_AUTH_SIGNING,
        "(id=%p) Verifying v4a signature: \n" PRInSTR "\n\nagainst expected canonical request: \n" PRInSTR
        "\n\nusing ecc key:\n X:" PRInSTR "\n Y:" PRInSTR "\n\n",
        (void *)signable,
        AWS_BYTE_CURSOR_PRI(signature_cursor),
        AWS_BYTE_CURSOR_PRI(expected_canonical_request_cursor),
        AWS_BYTE_CURSOR_PRI(ecc_key_pub_x),
        AWS_BYTE_CURSOR_PRI(ecc_key_pub_y));

    struct aws_ecc_key_pair *verification_key =
        aws_ecc_key_new_from_hex_coordinates(allocator, AWS_CAL_ECDSA_P256, ecc_key_pub_x, ecc_key_pub_y);
    if (verification_key == NULL) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Unable to create an ECC key from provided coordinates");
        goto done;
    }

    if (aws_credentials_get_ecc_key_pair(signing_state->config.credentials) == NULL) {
        struct aws_credentials *ecc_credentials =
            aws_credentials_new_ecc_from_aws_credentials(allocator, signing_state->config.credentials);
        aws_credentials_release(signing_state->config.credentials);
        signing_state->config.credentials = ecc_credentials;
        if (signing_state->config.credentials == NULL) {
            AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Unable to create ECC from provided credentials");
            goto done;
        }
    }

    if (aws_signing_build_canonical_request(signing_state)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Unable to canonicalize request for signing");
        goto done;
    }

    struct aws_byte_cursor canonical_request_cursor = aws_byte_cursor_from_buf(&signing_state->canonical_request);
    if (aws_byte_cursor_compare_lexical(&expected_canonical_request_cursor, &canonical_request_cursor) != 0) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Canonicalized request and expected canonical request do not match");
        aws_raise_error(AWS_AUTH_CANONICAL_REQUEST_MISMATCH);
        goto done;
    }

    if (aws_signing_build_string_to_sign(signing_state)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Unable to build string to sign from canonical request");
        goto done;
    }

    if (aws_validate_v4a_authorization_value(
            allocator, verification_key, aws_byte_cursor_from_buf(&signing_state->string_to_sign), signature_cursor)) {
        AWS_LOGF_ERROR(AWS_LS_AUTH_SIGNING, "Signature does not validate");
        aws_raise_error(AWS_AUTH_SIGV4A_SIGNATURE_VALIDATION_FAILURE);
        goto done;
    }

    result = AWS_OP_SUCCESS;

done:

    if (verification_key) {
        aws_ecc_key_pair_release(verification_key);
    }
    aws_signing_state_destroy(signing_state);

    return result;
}

static bool s_is_padding_byte(uint8_t byte) {
    return byte == AWS_SIGV4A_SIGNATURE_PADDING_BYTE;
}

struct aws_byte_cursor aws_trim_padded_sigv4a_signature(struct aws_byte_cursor signature) {
    return aws_byte_cursor_trim_pred(&signature, s_is_padding_byte);
}
