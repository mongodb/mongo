/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_util.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_platform_info.h"
#include "aws/s3/private/s3_request.h"
#include <aws/auth/credentials.h>
#include <aws/common/clock.h>
#include <aws/common/encoding.h>
#include <aws/common/string.h>
#include <aws/common/xml_parser.h>
#include <aws/http/request_response.h>
#include <aws/s3/s3_client.h>
#include <inttypes.h>

#ifdef _MSC_VER
/* sscanf warning (not currently scanning for strings) */
#    pragma warning(disable : 4996)
#endif

const struct aws_byte_cursor g_s3_client_version = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(AWS_S3_CLIENT_VERSION);
const struct aws_byte_cursor g_s3_service_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("s3");
const struct aws_byte_cursor g_s3express_service_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("s3express");
const struct aws_byte_cursor g_host_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Host");
const struct aws_byte_cursor g_range_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Range");
const struct aws_byte_cursor g_if_match_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("If-Match");
const struct aws_byte_cursor g_request_id_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-request-id");
const struct aws_byte_cursor g_etag_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("ETag");
const struct aws_byte_cursor g_content_range_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Range");
const struct aws_byte_cursor g_content_type_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type");
const struct aws_byte_cursor g_content_encoding_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding");
const struct aws_byte_cursor g_content_encoding_header_aws_chunked =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("aws-chunked");
const struct aws_byte_cursor g_content_length_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length");
const struct aws_byte_cursor g_decoded_content_length_header_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-decoded-content-length");
const struct aws_byte_cursor g_content_md5_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5");
const struct aws_byte_cursor g_trailer_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-trailer");
const struct aws_byte_cursor g_request_validation_mode = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-mode");
const struct aws_byte_cursor g_enabled = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("enabled");

const struct aws_byte_cursor g_checksum_algorithm_header_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-algorithm");
const struct aws_byte_cursor g_sdk_checksum_algorithm_header_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-sdk-checksum-algorithm");
const struct aws_byte_cursor g_accept_ranges_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("accept-ranges");
const struct aws_byte_cursor g_acl_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl");
const struct aws_byte_cursor g_mp_parts_count_header_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-mp-parts-count");
const struct aws_byte_cursor g_post_method = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("POST");
const struct aws_byte_cursor g_head_method = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("HEAD");
const struct aws_byte_cursor g_delete_method = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("DELETE");

const struct aws_byte_cursor g_user_agent_header_name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("User-Agent");
const struct aws_byte_cursor g_user_agent_header_product_name =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("CRTS3NativeClient");
const struct aws_byte_cursor g_user_agent_header_platform = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("platform");
const struct aws_byte_cursor g_user_agent_header_unknown = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("unknown");

const uint32_t g_s3_max_num_upload_parts = 10000;
const size_t g_s3_min_upload_part_size = MB_TO_BYTES(5);

void copy_http_headers(const struct aws_http_headers *src, struct aws_http_headers *dest) {
    AWS_PRECONDITION(src);
    AWS_PRECONDITION(dest);

    size_t headers_count = aws_http_headers_count(src);

    for (size_t header_index = 0; header_index < headers_count; ++header_index) {
        struct aws_http_header header;

        aws_http_headers_get_index(src, header_index, &header);
        aws_http_headers_set(dest, header.name, header.value);
    }
}
/* user_data for XML traversal */
struct xml_get_body_at_path_traversal {
    struct aws_allocator *allocator;
    const char **path_name_array;
    size_t path_name_count;
    size_t path_name_i;
    struct aws_byte_cursor *out_body;
    bool found_node;
};

static int s_xml_get_body_at_path_on_node(struct aws_xml_node *node, void *user_data) {
    struct xml_get_body_at_path_traversal *traversal = user_data;

    /* if we already found what we're looking for, just finish parsing */
    if (traversal->found_node) {
        return AWS_OP_SUCCESS;
    }

    /* check if this node is on the path */
    struct aws_byte_cursor node_name = aws_xml_node_get_name(node);
    const char *expected_name = traversal->path_name_array[traversal->path_name_i];
    if (aws_byte_cursor_eq_c_str_ignore_case(&node_name, expected_name)) {

        bool is_final_node_on_path = traversal->path_name_i + 1 == traversal->path_name_count;
        if (is_final_node_on_path) {
            /* retrieve the body */
            if (aws_xml_node_as_body(node, traversal->out_body) != AWS_OP_SUCCESS) {
                return AWS_OP_ERR;
            }
            traversal->found_node = true;
            return AWS_OP_SUCCESS;
        } else {
            /* node is on path, but it's not the final node, so traverse its children */
            traversal->path_name_i++;
            if (aws_xml_node_traverse(node, s_xml_get_body_at_path_on_node, traversal) != AWS_OP_SUCCESS) {
                return AWS_OP_ERR;
            }
            traversal->path_name_i--;
            return AWS_OP_SUCCESS;
        }
    } else {
        /* this node is not on the path, continue parsing siblings */
        return AWS_OP_SUCCESS;
    }
}

int aws_xml_get_body_at_path(
    struct aws_allocator *allocator,
    struct aws_byte_cursor xml_doc,
    const char **path_name_array,
    struct aws_byte_cursor *out_body) {

    struct xml_get_body_at_path_traversal traversal = {
        .allocator = allocator,
        .path_name_array = path_name_array,
        .path_name_count = 0,
        .out_body = out_body,
    };

    /* find path_name_count */
    while (path_name_array[traversal.path_name_count] != NULL) {
        traversal.path_name_count++;
        AWS_ASSERT(traversal.path_name_count < 4); /* sanity check, increase cap if necessary */
    }
    AWS_ASSERT(traversal.path_name_count > 0);

    /* parse XML */
    struct aws_xml_parser_options parse_options = {
        .doc = xml_doc,
        .on_root_encountered = s_xml_get_body_at_path_on_node,
        .user_data = &traversal,
    };
    if (aws_xml_parse(allocator, &parse_options)) {
        goto error;
    }

    if (!traversal.found_node) {
        aws_raise_error(AWS_ERROR_STRING_MATCH_NOT_FOUND);
        goto error;
    }

    return AWS_OP_SUCCESS;
error:
    AWS_ZERO_STRUCT(*out_body);
    return AWS_OP_ERR;
}

struct aws_cached_signing_config_aws *aws_cached_signing_config_new(
    struct aws_s3_client *client,
    const struct aws_signing_config_aws *signing_config) {
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(signing_config);

    struct aws_allocator *allocator = client->allocator;

    struct aws_cached_signing_config_aws *cached_signing_config =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_cached_signing_config_aws));

    cached_signing_config->allocator = allocator;

    cached_signing_config->config.config_type =
        signing_config->config_type ? signing_config->config_type : AWS_SIGNING_CONFIG_AWS;

    AWS_ASSERT(aws_byte_cursor_is_valid(&signing_config->region));
    if (signing_config->region.len > 0) {
        cached_signing_config->region = aws_string_new_from_cursor(allocator, &signing_config->region);
    } else {
        /* Fall back to client region. */
        cached_signing_config->region = aws_string_new_from_string(allocator, client->region);
    }
    cached_signing_config->config.region = aws_byte_cursor_from_string(cached_signing_config->region);

    if (signing_config->service.len > 0) {
        cached_signing_config->service = aws_string_new_from_cursor(allocator, &signing_config->service);
        cached_signing_config->config.service = aws_byte_cursor_from_string(cached_signing_config->service);
    } else {
        cached_signing_config->config.service = g_s3_service_name;
    }

    cached_signing_config->config.date = signing_config->date;

    AWS_ASSERT(aws_byte_cursor_is_valid(&signing_config->signed_body_value));

    if (signing_config->signed_body_value.len > 0) {
        cached_signing_config->signed_body_value =
            aws_string_new_from_cursor(allocator, &signing_config->signed_body_value);
        cached_signing_config->config.signed_body_value =
            aws_byte_cursor_from_string(cached_signing_config->signed_body_value);
    } else {
        cached_signing_config->config.signed_body_value = g_aws_signed_body_value_unsigned_payload;
    }

    if (signing_config->credentials != NULL) {
        aws_credentials_acquire(signing_config->credentials);
        cached_signing_config->config.credentials = signing_config->credentials;
    }

    if (signing_config->credentials_provider != NULL) {
        aws_credentials_provider_acquire(signing_config->credentials_provider);
        cached_signing_config->config.credentials_provider = signing_config->credentials_provider;
    }

    /* Configs default to Zero. */
    cached_signing_config->config.algorithm = signing_config->algorithm;
    cached_signing_config->config.signature_type = signing_config->signature_type;
    /* TODO: you don't have a way to override this config as the other option is zero. But, you cannot really use the
     * other value, as it is always required. */
    cached_signing_config->config.signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
    cached_signing_config->config.should_sign_header = signing_config->should_sign_header;
    /* It's the user's responsibility to keep the user data around */
    cached_signing_config->config.should_sign_header_ud = signing_config->should_sign_header_ud;

    cached_signing_config->config.flags = signing_config->flags;
    cached_signing_config->config.expiration_in_seconds = signing_config->expiration_in_seconds;

    return cached_signing_config;
}

void aws_cached_signing_config_destroy(struct aws_cached_signing_config_aws *cached_signing_config) {
    if (cached_signing_config == NULL) {
        return;
    }

    aws_credentials_release(cached_signing_config->config.credentials);
    aws_credentials_provider_release(cached_signing_config->config.credentials_provider);

    aws_string_destroy(cached_signing_config->service);
    aws_string_destroy(cached_signing_config->region);
    aws_string_destroy(cached_signing_config->signed_body_value);

    aws_mem_release(cached_signing_config->allocator, cached_signing_config);
}

void aws_s3_init_default_signing_config(
    struct aws_signing_config_aws *signing_config,
    const struct aws_byte_cursor region,
    struct aws_credentials_provider *credentials_provider) {
    AWS_PRECONDITION(signing_config);
    AWS_PRECONDITION(credentials_provider);

    AWS_ZERO_STRUCT(*signing_config);

    signing_config->config_type = AWS_SIGNING_CONFIG_AWS;
    signing_config->algorithm = AWS_SIGNING_ALGORITHM_V4;
    signing_config->credentials_provider = credentials_provider;
    signing_config->region = region;
    signing_config->service = g_s3_service_name;
    signing_config->signed_body_header = AWS_SBHT_X_AMZ_CONTENT_SHA256;
    signing_config->signed_body_value = g_aws_signed_body_value_unsigned_payload;
}

static struct aws_byte_cursor s_quote_entity_literal = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("&quot;");
static struct aws_byte_cursor s_quote_literal = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("\"");

struct aws_byte_buf aws_replace_quote_entities(struct aws_allocator *allocator, struct aws_byte_cursor src) {
    struct aws_byte_buf out_buf;
    aws_byte_buf_init(&out_buf, allocator, src.len);

    for (size_t i = 0; i < src.len; ++i) {
        size_t chars_remaining = src.len - i;

        if (chars_remaining >= s_quote_entity_literal.len &&
            !strncmp((const char *)&src.ptr[i], (const char *)s_quote_entity_literal.ptr, s_quote_entity_literal.len)) {
            /* Append quote */
            aws_byte_buf_append(&out_buf, &s_quote_literal);
            i += s_quote_entity_literal.len - 1;
        } else {
            /* Append character */
            struct aws_byte_cursor character_cursor = aws_byte_cursor_from_array(&src.ptr[i], 1);
            aws_byte_buf_append(&out_buf, &character_cursor);
        }
    }

    return out_buf;
}

struct aws_string *aws_strip_quotes(struct aws_allocator *allocator, struct aws_byte_cursor in_cur) {

    if (in_cur.len >= 2 && in_cur.ptr[0] == '"' && in_cur.ptr[in_cur.len - 1] == '"') {
        aws_byte_cursor_advance(&in_cur, 1);
        --in_cur.len;
    }

    return aws_string_new_from_cursor(allocator, &in_cur);
}

int aws_last_error_or_unknown(void) {
    int error = aws_last_error();
    AWS_ASSERT(error != AWS_ERROR_SUCCESS); /* Someone forgot to call aws_raise_error() */
    if (error == AWS_ERROR_SUCCESS) {
        return AWS_ERROR_UNKNOWN;
    }

    return error;
}

void aws_s3_add_user_agent_header(struct aws_allocator *allocator, struct aws_http_message *message) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(message);

    const struct aws_byte_cursor space_delimiter = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(" ");
    const struct aws_byte_cursor forward_slash = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/");
    struct aws_byte_cursor platform_cursor = aws_s3_get_current_platform_ec2_intance_type(true /* cached_only */);
    if (!platform_cursor.len) {
        platform_cursor = g_user_agent_header_unknown;
    }
    const size_t user_agent_length = g_user_agent_header_product_name.len + forward_slash.len +
                                     g_s3_client_version.len + space_delimiter.len + g_user_agent_header_platform.len +
                                     forward_slash.len + platform_cursor.len;

    struct aws_http_headers *headers = aws_http_message_get_headers(message);
    AWS_ASSERT(headers != NULL);

    struct aws_byte_cursor current_user_agent_header;
    AWS_ZERO_STRUCT(current_user_agent_header);

    struct aws_byte_buf user_agent_buffer;
    AWS_ZERO_STRUCT(user_agent_buffer);

    if (aws_http_headers_get(headers, g_user_agent_header_name, &current_user_agent_header) == AWS_OP_SUCCESS) {
        /* If the header was found, then create a buffer with the total size we'll need, and append the current user
         * agent header with a trailing space. */
        aws_byte_buf_init(
            &user_agent_buffer, allocator, current_user_agent_header.len + space_delimiter.len + user_agent_length);

        aws_byte_buf_append_dynamic(&user_agent_buffer, &current_user_agent_header);

        aws_byte_buf_append_dynamic(&user_agent_buffer, &space_delimiter);

    } else {
        AWS_ASSERT(aws_last_error() == AWS_ERROR_HTTP_HEADER_NOT_FOUND);

        /* If the header was not found, then create a buffer with just the size of the user agent string that is about
         * to be appended to the buffer. */
        aws_byte_buf_init(&user_agent_buffer, allocator, user_agent_length);
    }

    /* Append the client's user-agent string. */
    {
        aws_byte_buf_append_dynamic(&user_agent_buffer, &g_user_agent_header_product_name);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &forward_slash);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &g_s3_client_version);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &space_delimiter);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &g_user_agent_header_platform);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &forward_slash);
        aws_byte_buf_append_dynamic(&user_agent_buffer, &platform_cursor);
    }

    /* Apply the updated header. */
    aws_http_headers_set(headers, g_user_agent_header_name, aws_byte_cursor_from_buf(&user_agent_buffer));

    /* Clean up the scratch buffer. */
    aws_byte_buf_clean_up(&user_agent_buffer);
}

int aws_s3_parse_content_range_response_header(
    struct aws_allocator *allocator,
    struct aws_http_headers *response_headers,
    uint64_t *out_range_start,
    uint64_t *out_range_end,
    uint64_t *out_object_size) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(response_headers);

    struct aws_byte_cursor content_range_header_value;

    if (aws_http_headers_get(response_headers, g_content_range_header_name, &content_range_header_value)) {
        aws_raise_error(AWS_ERROR_S3_MISSING_CONTENT_RANGE_HEADER);
        return AWS_OP_ERR;
    }

    int result = AWS_OP_ERR;

    uint64_t range_start = 0;
    uint64_t range_end = 0;
    uint64_t object_size = 0;

    struct aws_string *content_range_header_value_str =
        aws_string_new_from_cursor(allocator, &content_range_header_value);

    /* Expected Format of header is: "bytes StartByte-EndByte/TotalObjectSize" */
    int num_fields_found = sscanf(
        (const char *)content_range_header_value_str->bytes,
        "bytes %" PRIu64 "-%" PRIu64 "/%" PRIu64,
        &range_start,
        &range_end,
        &object_size);

    if (num_fields_found < 3) {
        aws_raise_error(AWS_ERROR_S3_INVALID_CONTENT_RANGE_HEADER);
        goto clean_up;
    }

    if (out_range_start != NULL) {
        *out_range_start = range_start;
    }

    if (out_range_end != NULL) {
        *out_range_end = range_end;
    }

    if (out_object_size != NULL) {
        *out_object_size = object_size;
    }

    result = AWS_OP_SUCCESS;

clean_up:
    aws_string_destroy(content_range_header_value_str);
    content_range_header_value_str = NULL;

    return result;
}

int aws_s3_parse_content_length_response_header(
    struct aws_allocator *allocator,
    struct aws_http_headers *response_headers,
    uint64_t *out_content_length) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(response_headers);
    AWS_PRECONDITION(out_content_length);

    struct aws_byte_cursor content_length_header_value;

    if (aws_http_headers_get(response_headers, g_content_length_header_name, &content_length_header_value)) {
        aws_raise_error(AWS_ERROR_S3_MISSING_CONTENT_LENGTH_HEADER);
        return AWS_OP_ERR;
    }

    struct aws_string *content_length_header_value_str =
        aws_string_new_from_cursor(allocator, &content_length_header_value);

    int result = AWS_OP_ERR;

    if (sscanf((const char *)content_length_header_value_str->bytes, "%" PRIu64, out_content_length) == 1) {
        result = AWS_OP_SUCCESS;
    } else {
        aws_raise_error(AWS_ERROR_S3_INVALID_CONTENT_LENGTH_HEADER);
    }

    aws_string_destroy(content_length_header_value_str);
    return result;
}

int aws_s3_parse_request_range_header(
    struct aws_http_headers *request_headers,
    bool *out_has_start_range,
    bool *out_has_end_range,
    uint64_t *out_start_range,
    uint64_t *out_end_range) {

    AWS_PRECONDITION(request_headers);
    AWS_PRECONDITION(out_has_start_range);
    AWS_PRECONDITION(out_has_end_range);
    AWS_PRECONDITION(out_start_range);
    AWS_PRECONDITION(out_end_range);

    bool has_start_range = false;
    bool has_end_range = false;
    uint64_t start_range = 0;
    uint64_t end_range = 0;

    struct aws_byte_cursor range_header_value;

    if (aws_http_headers_get(request_headers, g_range_header_name, &range_header_value)) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }

    struct aws_byte_cursor range_header_start = aws_byte_cursor_from_c_str("bytes=");

    /* verify bytes= */
    if (!aws_byte_cursor_starts_with(&range_header_value, &range_header_start)) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }

    aws_byte_cursor_advance(&range_header_value, range_header_start.len);
    struct aws_byte_cursor substr = {0};
    /* parse start range */
    if (!aws_byte_cursor_next_split(&range_header_value, '-', &substr)) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }
    if (substr.len > 0) {
        if (aws_byte_cursor_utf8_parse_u64(substr, &start_range)) {
            return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
        }
        has_start_range = true;
    }

    /* parse end range */
    if (!aws_byte_cursor_next_split(&range_header_value, '-', &substr)) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }
    if (substr.len > 0) {
        if (aws_byte_cursor_utf8_parse_u64(substr, &end_range)) {
            return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
        }
        has_end_range = true;
    }

    /* verify that there is nothing extra */
    if (aws_byte_cursor_next_split(&range_header_value, '-', &substr)) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }

    /* verify that start-range <= end-range */
    if (has_end_range && start_range > end_range) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }

    /* verify that start-range or end-range is present */
    if (!has_start_range && !has_end_range) {
        return aws_raise_error(AWS_ERROR_S3_INVALID_RANGE_HEADER);
    }

    *out_has_start_range = has_start_range;
    *out_has_end_range = has_end_range;
    *out_start_range = start_range;
    *out_end_range = end_range;
    return AWS_OP_SUCCESS;
}

uint32_t aws_s3_calculate_auto_ranged_get_num_parts(
    size_t part_size,
    uint64_t first_part_size,
    uint64_t object_range_start,
    uint64_t object_range_end) {
    uint32_t num_parts = 1;

    if (first_part_size == 0) {
        return num_parts;
    }
    uint64_t second_part_start = object_range_start + first_part_size;

    /* If the range has room for a second part, calculate the additional amount of parts. */
    if (second_part_start <= object_range_end) {
        uint64_t aligned_range_remainder = object_range_end + 1 - second_part_start; /* range-end is inclusive */
        num_parts += (uint32_t)(aligned_range_remainder / (uint64_t)part_size);

        if ((aligned_range_remainder % part_size) > 0) {
            ++num_parts;
        }
    }

    return num_parts;
}

void aws_s3_calculate_auto_ranged_get_part_range(
    uint64_t object_range_start,
    uint64_t object_range_end,
    size_t part_size,
    uint64_t first_part_size,
    uint32_t part_number,
    uint64_t *out_part_range_start,
    uint64_t *out_part_range_end) {
    AWS_PRECONDITION(out_part_range_start);
    AWS_PRECONDITION(out_part_range_end);

    AWS_ASSERT(part_number > 0);

    const uint32_t part_index = part_number - 1;

    /* Part index is assumed to be in a valid range. */
    AWS_ASSERT(
        part_index <
        aws_s3_calculate_auto_ranged_get_num_parts(part_size, first_part_size, object_range_start, object_range_end));

    uint64_t part_size_uint64 = (uint64_t)part_size;

    if (part_index == 0) {
        /* If this is the first part, then use the first part size. */
        *out_part_range_start = object_range_start;
        *out_part_range_end = *out_part_range_start + first_part_size - 1;
    } else {
        /* Else, find the next part by adding the object range + total number of whole parts before this one + initial
         * part size*/
        *out_part_range_start = object_range_start + ((uint64_t)(part_index - 1)) * part_size_uint64 + first_part_size;
        *out_part_range_end = *out_part_range_start + part_size_uint64 - 1; /* range-end is inclusive */
    }

    /* Cap the part's range end using the object's range end. */
    if (*out_part_range_end > object_range_end) {
        *out_part_range_end = object_range_end;
    }
}

int aws_s3_calculate_optimal_mpu_part_size_and_num_parts(
    uint64_t content_length,
    size_t client_part_size,
    uint64_t client_max_part_size,
    size_t *out_part_size,
    uint32_t *out_num_parts) {

    AWS_FATAL_ASSERT(out_part_size);
    AWS_FATAL_ASSERT(out_num_parts);

    if (content_length == 0) {
        *out_part_size = 0;
        *out_num_parts = 0;
        return AWS_OP_SUCCESS;
    }

    uint64_t part_size_uint64 = content_length / (uint64_t)g_s3_max_num_upload_parts;

    if ((content_length % g_s3_max_num_upload_parts) > 0) {
        ++part_size_uint64;
    }

    if (part_size_uint64 > SIZE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create meta request; required part size of %" PRIu64 " bytes is too large for platform.",
            part_size_uint64);

        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    size_t part_size = (size_t)part_size_uint64;

    if (part_size > client_max_part_size) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create meta request; required part size for request is %" PRIu64
            ", but current maximum part size is %" PRIu64,
            (uint64_t)part_size,
            (uint64_t)client_max_part_size);
        return aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
    }

    if (part_size < client_part_size) {
        part_size = client_part_size;
    }

    if (content_length < part_size) {
        /* When the content length is smaller than part size and larger than the threshold, we set one part
         * with the whole length */
        part_size = (size_t)content_length;
    }

    uint32_t num_parts = (uint32_t)(content_length / part_size);
    if ((content_length % part_size) > 0) {
        ++num_parts;
    }
    AWS_ASSERT(num_parts <= g_s3_max_num_upload_parts);

    *out_part_size = part_size;
    *out_num_parts = num_parts;
    return AWS_OP_SUCCESS;
}

int aws_s3_crt_error_code_from_recoverable_server_error_code_string(struct aws_byte_cursor error_code_string) {
    if (aws_byte_cursor_eq_c_str_ignore_case(&error_code_string, "SlowDown")) {
        return AWS_ERROR_S3_SLOW_DOWN;
    }
    if (aws_byte_cursor_eq_c_str_ignore_case(&error_code_string, "InternalError") ||
        aws_byte_cursor_eq_c_str_ignore_case(&error_code_string, "InternalErrors")) {
        return AWS_ERROR_S3_INTERNAL_ERROR;
    }
    if (aws_byte_cursor_eq_c_str_ignore_case(&error_code_string, "RequestTimeTooSkewed")) {
        return AWS_ERROR_S3_REQUEST_TIME_TOO_SKEWED;
    }

    if (aws_byte_cursor_eq_c_str_ignore_case(&error_code_string, "RequestTimeout")) {
        return AWS_ERROR_S3_REQUEST_TIMEOUT;
    }

    return AWS_ERROR_UNKNOWN;
}

void aws_s3_request_finish_up_metrics_synced(struct aws_s3_request *request, struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(request);
    ASSERT_SYNCED_DATA_LOCK_HELD(meta_request);

    if (request->send_data.metrics != NULL) {
        /* Request is done, complete the metrics for the request now. */
        struct aws_s3_request_metrics *metrics = request->send_data.metrics;
        aws_high_res_clock_get_ticks((uint64_t *)&metrics->time_metrics.end_timestamp_ns);
        metrics->time_metrics.total_duration_ns =
            metrics->time_metrics.end_timestamp_ns - metrics->time_metrics.start_timestamp_ns;

        if (meta_request->telemetry_callback != NULL) {
            struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_TELEMETRY};
            event.u.telemetry.metrics = aws_s3_request_metrics_acquire(metrics);
            aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
        }
        request->send_data.metrics = aws_s3_request_metrics_release(metrics);
    }
}

int aws_s3_check_headers_for_checksum(
    struct aws_s3_meta_request *meta_request,
    const struct aws_http_headers *headers,
    struct aws_s3_checksum **out_checksum,
    struct aws_byte_buf *out_checksum_buffer,
    bool meta_request_level) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_checksum);
    AWS_PRECONDITION(out_checksum_buffer);

    if (!headers || aws_http_headers_count(headers) == 0) {
        *out_checksum = NULL;
        return AWS_OP_SUCCESS;
    }
    if (meta_request_level && aws_http_headers_has(headers, g_mp_parts_count_header_name)) {
        /* g_mp_parts_count_header_name indicates it's a object was uploaded as a
         * multipart upload. So, the checksum should not be applied to the meta request level.
         * But we we want to check it for the request level. */
        *out_checksum = NULL;
        return AWS_OP_SUCCESS;
    }

    for (size_t i = 0; i < AWS_ARRAY_SIZE(s_checksum_algo_priority_list); i++) {
        enum aws_s3_checksum_algorithm algorithm = s_checksum_algo_priority_list[i];
        if (!aws_s3_meta_request_checksum_config_has_algorithm(meta_request, algorithm)) {
            /* If user doesn't select this algorithm, skip */
            continue;
        }
        const struct aws_byte_cursor algorithm_header_name =
            aws_get_http_header_name_from_checksum_algorithm(algorithm);
        struct aws_byte_cursor checksum_value;
        if (aws_http_headers_get(headers, algorithm_header_name, &checksum_value) == AWS_OP_SUCCESS) {
            /* Found the checksum header, keep the header value and initialize the running checksum */
            size_t encoded_len = 0;
            aws_base64_compute_encoded_len(aws_get_digest_size_from_checksum_algorithm(algorithm), &encoded_len);
            if (checksum_value.len == encoded_len - 1) {
                /* encoded_len includes the nullptr length. -1 is the expected length. */
                aws_byte_buf_init_copy_from_cursor(out_checksum_buffer, meta_request->allocator, checksum_value);
                *out_checksum = aws_checksum_new(meta_request->allocator, algorithm);
                if (!*out_checksum) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "Could not create checksum for algorithm: %d, due to error code %d (%s)",
                        algorithm,
                        aws_last_error_or_unknown(),
                        aws_error_str(aws_last_error_or_unknown()));
                    return AWS_OP_ERR;
                }
                return AWS_OP_SUCCESS;
            }
            break;
        }
    }
    *out_checksum = NULL;
    return AWS_OP_SUCCESS;
}
