/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_util.h"
#include <aws/cal/hash.h>
#include <aws/common/byte_buf.h>
#include <aws/common/encoding.h>
#include <aws/common/string.h>
#include <aws/http/request_response.h>
#include <aws/io/async_stream.h>
#include <aws/io/stream.h>
#include <inttypes.h>

const struct aws_byte_cursor g_s3_create_multipart_upload_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc64nvme"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc32c"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc32"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-sha1"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-sha256"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("if-none-match"),
};

const size_t g_s3_create_multipart_upload_excluded_headers_count =
    AWS_ARRAY_SIZE(g_s3_create_multipart_upload_excluded_headers);

const struct aws_byte_cursor g_s3_upload_part_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc64nvme"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc32c"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-crc32"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-sha1"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-sha256"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("if-none-match"),
};

const size_t g_s3_upload_part_excluded_headers_count = AWS_ARRAY_SIZE(g_s3_upload_part_excluded_headers);

const struct aws_byte_cursor g_s3_complete_multipart_upload_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-mp-object-size"),
};

const size_t g_s3_complete_multipart_upload_excluded_headers_count =
    AWS_ARRAY_SIZE(g_s3_complete_multipart_upload_excluded_headers);

/* The server-side encryption (SSE) is needed only when the object was created using a checksum algorithm for complete
 * multipart upload.  */
const struct aws_byte_cursor g_s3_complete_multipart_upload_with_checksum_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-sdk-checksum-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-mp-object-size"),
};

const struct aws_byte_cursor g_s3_list_parts_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
};

const size_t g_s3_list_parts_excluded_headers_count = AWS_ARRAY_SIZE(g_s3_list_parts_excluded_headers);

const struct aws_byte_cursor g_s3_list_parts_with_checksum_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
};

const size_t g_s3_list_parts_with_checksum_excluded_headers_count =
    AWS_ARRAY_SIZE(g_s3_list_parts_with_checksum_excluded_headers);

const struct aws_byte_cursor g_s3_abort_multipart_upload_excluded_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-acl"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Cache-Control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Disposition"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Encoding"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Language"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Length"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Content-Type"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("Expires"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-full-control"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-read-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-grant-write-acp"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-storage-class"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-website-redirect-location"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-aws-kms-key-id"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-bucket-key-enabled"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-tagging"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-mode"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-retain-until-date"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-object-lock-legal-hold"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("if-none-match"),
};

static const struct aws_byte_cursor s_x_amz_meta_prefix = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-meta-");

static const struct aws_byte_cursor s_checksum_type_header =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-checksum-type");
static const struct aws_byte_cursor s_checksum_type_full_object = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("full_object");

const size_t g_s3_abort_multipart_upload_excluded_headers_count =
    AWS_ARRAY_SIZE(g_s3_abort_multipart_upload_excluded_headers);

static void s_s3_message_util_add_range_header(
    uint64_t part_range_start,
    uint64_t part_range_end,
    struct aws_http_message *out_message);

/* Create a new get object request from an existing get object request. Currently just adds an optional ranged header.
 */
struct aws_http_message *aws_s3_ranged_get_object_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    uint64_t range_start,
    uint64_t range_end) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(base_message);

    struct aws_http_message *message =
        aws_s3_message_util_copy_http_message_no_body_all_headers(allocator, base_message);

    if (message == NULL) {
        return NULL;
    }

    s_s3_message_util_add_range_header(range_start, range_end, message);

    return message;
}

/* Creates a create-multipart-upload request from a given put object request. */
struct aws_http_message *aws_s3_create_multipart_upload_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    const struct checksum_config_storage *checksum_config) {
    AWS_PRECONDITION(allocator);

    /* For multipart upload, some headers should ONLY be in the initial create-multipart request.
     * Headers such as:
     * - SSE related headers
     * - user metadata (prefixed "x-amz-meta-") headers */
    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
        allocator,
        base_message,
        g_s3_create_multipart_upload_excluded_headers,
        AWS_ARRAY_SIZE(g_s3_create_multipart_upload_excluded_headers),
        false /*exclude_x_amz_meta*/);

    if (message == NULL) {
        return NULL;
    }

    if (aws_s3_message_util_set_multipart_request_path(allocator, NULL, 0, true, message)) {
        goto error_clean_up;
    }

    struct aws_http_headers *headers = aws_http_message_get_headers(message);

    if (headers == NULL) {
        goto error_clean_up;
    }

    if (aws_http_headers_erase(headers, g_content_md5_header_name)) {
        if (aws_last_error_or_unknown() != AWS_ERROR_HTTP_HEADER_NOT_FOUND) {
            goto error_clean_up;
        }
    }
    if (checksum_config && checksum_config->location != AWS_SCL_NONE) {
        if (checksum_config->checksum_algorithm) {
            if (aws_http_headers_set(
                    headers,
                    g_checksum_algorithm_header_name,
                    aws_get_checksum_algorithm_name(checksum_config->checksum_algorithm))) {
                goto error_clean_up;
            }
        }
        if (checksum_config->has_full_object_checksum) {
            /* Request S3 to store the full object checksum as it's set from user. */
            if (aws_http_headers_set(headers, s_checksum_type_header, s_checksum_type_full_object)) {
                goto error_clean_up;
            }
        }
    }

    struct aws_byte_cursor content_length_cursor = aws_byte_cursor_from_c_str("0");
    if (aws_http_headers_set(headers, g_content_length_header_name, content_length_cursor)) {
        goto error_clean_up;
    }

    aws_http_message_set_request_method(message, g_post_method);
    aws_http_message_set_body_stream(message, NULL);

    return message;

error_clean_up:
    aws_http_message_release(message);
    return NULL;
}

/* Create a new put object request from an existing put object request.  Currently just optionally adds part information
 * for a multipart upload. */
struct aws_http_message *aws_s3_upload_part_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *buffer,
    uint32_t part_number,
    const struct aws_string *upload_id,
    bool should_compute_content_md5,
    const struct checksum_config_storage *checksum_config,
    struct aws_byte_buf *encoded_checksum_output) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(base_message);
    AWS_PRECONDITION(part_number > 0);
    AWS_PRECONDITION(buffer);

    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
        allocator,
        base_message,
        g_s3_upload_part_excluded_headers,
        AWS_ARRAY_SIZE(g_s3_upload_part_excluded_headers),
        true /*exclude_x_amz_meta*/);

    if (message == NULL) {
        return NULL;
    }

    if (aws_s3_message_util_set_multipart_request_path(allocator, upload_id, part_number, false, message)) {
        goto error_clean_up;
    }

    if (aws_s3_message_util_assign_body(allocator, buffer, message, checksum_config, encoded_checksum_output) == NULL) {
        goto error_clean_up;
    }

    if (should_compute_content_md5) {
        if (!checksum_config || checksum_config->location == AWS_SCL_NONE) {
            /* MD5 will be skipped if flexible checksum used */
            if (aws_s3_message_util_add_content_md5_header(allocator, buffer, message)) {
                goto error_clean_up;
            }
        }
    }

    return message;

error_clean_up:
    aws_http_message_release(message);
    return NULL;
}

struct aws_http_message *aws_s3_upload_part_copy_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *buffer,
    uint32_t part_number,
    uint64_t range_start,
    uint64_t range_end,
    const struct aws_string *upload_id,
    bool should_compute_content_md5) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(base_message);
    AWS_PRECONDITION(part_number > 0);

    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
        allocator,
        base_message,
        g_s3_upload_part_excluded_headers,
        AWS_ARRAY_SIZE(g_s3_upload_part_excluded_headers),
        true /*exclude_x_amz_meta*/);

    if (message == NULL) {
        goto error_clean_up;
    }

    if (aws_s3_message_util_set_multipart_request_path(allocator, upload_id, part_number, false, message)) {
        goto error_clean_up;
    }

    if (buffer != NULL) {
        /* part copy does not have a ChecksumAlgorithm member, it will use the same algorithm as the create
         * multipart upload request specifies */
        if (aws_s3_message_util_assign_body(
                allocator, buffer, message, NULL /* checksum_config */, NULL /* out_checksum */) == NULL) {
            goto error_clean_up;
        }

        if (should_compute_content_md5) {
            if (aws_s3_message_util_add_content_md5_header(allocator, buffer, message)) {
                goto error_clean_up;
            }
        }
    }

    char source_range[1024];
    snprintf(source_range, sizeof(source_range), "bytes=%" PRIu64 "-%" PRIu64, range_start, range_end);

    struct aws_http_header source_range_header = {
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source-range"),
        .value = aws_byte_cursor_from_c_str(source_range),
    };

    struct aws_http_headers *headers = aws_http_message_get_headers(message);
    aws_http_headers_add_header(headers, &source_range_header);

    return message;

error_clean_up:

    if (message != NULL) {
        aws_http_message_release(message);
        message = NULL;
    }

    return NULL;
}

static const struct aws_byte_cursor s_slash_char = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("/");
/**
 * For the CopyObject operation, create the initial HEAD message to retrieve the size of the copy source.
 */
struct aws_http_message *aws_s3_get_source_object_size_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message) {
    struct aws_http_message *message = NULL;
    struct aws_byte_buf head_object_host_header;
    AWS_ZERO_STRUCT(head_object_host_header);

    AWS_PRECONDITION(allocator);

    /* Find the x-amz-copy-source header, to extract source bucket/key information. */
    struct aws_http_headers *headers = aws_http_message_get_headers(base_message);
    if (!headers) {
        AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "CopyRequest is missing headers");
        return NULL;
    }

    struct aws_byte_cursor source_header;
    const struct aws_byte_cursor copy_source_header = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-copy-source");
    if (aws_http_headers_get(headers, copy_source_header, &source_header) != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "CopyRequest is missing the x-amz-copy-source header");
        return NULL;
    }
    struct aws_byte_cursor host;
    if (aws_http_headers_get(headers, g_host_header_name, &host) != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "CopyRequest is missing the Host header");
        return NULL;
    }

    struct aws_byte_cursor request_path = source_header;

    /* Skip optional leading slash. */
    if (aws_byte_cursor_starts_with(&request_path, &s_slash_char)) {
        aws_byte_cursor_advance(&request_path, 1);
    }

    /* From this point forward, the format is {bucket}/{key} - split
    components.*/

    struct aws_byte_cursor source_bucket = {0};

    if (aws_byte_cursor_next_split(&request_path, '/', &source_bucket)) {
        aws_byte_cursor_advance(&request_path, source_bucket.len);
    }

    if (source_bucket.len == 0 || request_path.len == 0) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_GENERAL,
            "CopyRequest x-amz-copy-source header does not follow expected bucket/key format: " PRInSTR,
            AWS_BYTE_CURSOR_PRI(source_header));
        goto error_cleanup;
    }

    if (aws_byte_buf_init_copy_from_cursor(&head_object_host_header, allocator, source_bucket)) {
        goto error_cleanup;
    }

    /* Reuse the domain name from the original Host header for the HEAD request.
     * TODO: following code works by replacing bucket name in the host with the
     * source bucket name. this only works for virtual host endpoints and has a
     * slew of other issues, like not supporting source in a different region.
     * This covers common case, but we need to rethink how we can support all
     * cases in general.
     */
    struct aws_byte_cursor domain_name;
    const struct aws_byte_cursor dot = aws_byte_cursor_from_c_str(".");
    if (aws_byte_cursor_find_exact(&host, &dot, &domain_name)) {
        AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "CopyRequest Host header not in FQDN format");
        goto error_cleanup;
    }

    if (aws_byte_buf_append_dynamic(&head_object_host_header, &domain_name)) {
        goto error_cleanup;
    }

    message = aws_http_message_new_request(allocator);
    if (message == NULL) {
        goto error_cleanup;
    }

    if (aws_http_message_set_request_method(message, g_head_method)) {
        goto error_cleanup;
    }

    struct aws_http_header host_header = {
        .name = g_host_header_name,
        .value = aws_byte_cursor_from_buf(&head_object_host_header),
    };
    if (aws_http_message_add_header(message, host_header)) {
        goto error_cleanup;
    }

    if (aws_http_message_set_request_path(message, request_path)) {
        goto error_cleanup;
    }

    aws_byte_buf_clean_up(&head_object_host_header);
    return message;

error_cleanup:
    aws_byte_buf_clean_up(&head_object_host_header);
    aws_http_message_release(message);
    return NULL;
}

static const struct aws_byte_cursor s_complete_payload_begin = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
    "<CompleteMultipartUpload xmlns=\"http://s3.amazonaws.com/doc/2006-03-01/\">\n");

static const struct aws_byte_cursor s_complete_payload_end =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("</CompleteMultipartUpload>");

static const struct aws_byte_cursor s_part_section_string_0 = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("    <Part>\n"
                                                                                                    "        <ETag>");

static const struct aws_byte_cursor s_part_section_string_1 =
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("</ETag>\n"
                                          "         <PartNumber>");

static const struct aws_byte_cursor s_close_part_number_tag = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("</PartNumber>\n");
static const struct aws_byte_cursor s_close_part_tag = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("    </Part>\n");
static const struct aws_byte_cursor s_open_start_bracket = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("         <");
static const struct aws_byte_cursor s_open_end_bracket = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("</");
static const struct aws_byte_cursor s_close_bracket = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(">");
static const struct aws_byte_cursor s_close_bracket_new_line = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(">\n");
/* Create a complete-multipart message, which includes an XML payload of all completed parts. */
struct aws_http_message *aws_s3_complete_multipart_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *body_buffer,
    const struct aws_string *upload_id,
    const struct aws_array_list *parts,
    const struct checksum_config_storage *checksum_config) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(base_message);
    AWS_PRECONDITION(body_buffer);
    AWS_PRECONDITION(upload_id);
    AWS_PRECONDITION(parts);

    struct aws_byte_cursor mpu_algorithm_checksum_name;
    AWS_ZERO_STRUCT(mpu_algorithm_checksum_name);
    struct aws_http_message *message = NULL;
    bool set_checksums = checksum_config && checksum_config->location != AWS_SCL_NONE;
    const struct aws_http_headers *initial_message_headers = aws_http_message_get_headers(base_message);
    AWS_ASSERT(initial_message_headers);
    if (set_checksums) {
        mpu_algorithm_checksum_name =
            aws_get_completed_part_name_from_checksum_algorithm(checksum_config->checksum_algorithm);
        message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
            allocator,
            base_message,
            g_s3_complete_multipart_upload_with_checksum_excluded_headers,
            AWS_ARRAY_SIZE(g_s3_complete_multipart_upload_with_checksum_excluded_headers),
            true /*exclude_x_amz_meta*/);

    } else {
        message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
            allocator,
            base_message,
            g_s3_complete_multipart_upload_excluded_headers,
            AWS_ARRAY_SIZE(g_s3_complete_multipart_upload_excluded_headers),
            true /*exclude_x_amz_meta*/);
    }

    struct aws_http_headers *headers = NULL;

    if (message == NULL) {
        goto error_clean_up;
    }

    if (aws_s3_message_util_set_multipart_request_path(allocator, upload_id, 0, false, message)) {
        goto error_clean_up;
    }

    aws_http_message_set_request_method(message, g_post_method);

    headers = aws_http_message_get_headers(message);

    if (headers == NULL) {
        goto error_clean_up;
    }
    if (set_checksums && checksum_config->has_full_object_checksum) {
        /* Set the full object checksum header. */
        AWS_ASSERT(checksum_config->checksum_algorithm != AWS_SCA_NONE);
        if (aws_http_headers_set(
                headers,
                aws_get_http_header_name_from_checksum_algorithm(checksum_config->checksum_algorithm),
                aws_byte_cursor_from_buf(&checksum_config->full_object_checksum))) {
            goto error_clean_up;
        }
        /* Request S3 to store the full object checksum as it's set from user. */
        if (aws_http_headers_set(headers, s_checksum_type_header, s_checksum_type_full_object)) {
            goto error_clean_up;
        }
    }
    struct aws_byte_cursor content_length_cursor;
    if (aws_http_headers_get(initial_message_headers, g_content_length_header_name, &content_length_cursor) ==
        AWS_OP_SUCCESS) {
        /* Set content-length from base message as x-amz-mp-object-size. */
        if (aws_http_headers_set(headers, aws_byte_cursor_from_c_str("x-amz-mp-object-size"), content_length_cursor)) {
            goto error_clean_up;
        }
    }

    /* Create XML payload with all the etags of finished parts */
    {
        aws_byte_buf_reset(body_buffer, false);

        if (aws_byte_buf_append_dynamic(body_buffer, &s_complete_payload_begin)) {
            goto error_clean_up;
        }

        for (size_t part_index = 0; part_index < aws_array_list_length(parts); ++part_index) {
            struct aws_s3_mpu_part_info *part = NULL;

            aws_array_list_get_at(parts, &part, part_index);
            AWS_FATAL_ASSERT(part != NULL);

            if (aws_byte_buf_append_dynamic(body_buffer, &s_part_section_string_0)) {
                goto error_clean_up;
            }

            struct aws_byte_cursor etag_byte_cursor = aws_byte_cursor_from_string(part->etag);

            if (aws_byte_buf_append_dynamic(body_buffer, &etag_byte_cursor)) {
                goto error_clean_up;
            }

            if (aws_byte_buf_append_dynamic(body_buffer, &s_part_section_string_1)) {
                goto error_clean_up;
            }

            char part_number_buffer[32] = "";
            int part_number = (int)(part_index + 1);
            int part_number_num_char = snprintf(part_number_buffer, sizeof(part_number_buffer), "%d", part_number);
            struct aws_byte_cursor part_number_byte_cursor =
                aws_byte_cursor_from_array(part_number_buffer, part_number_num_char);

            if (aws_byte_buf_append_dynamic(body_buffer, &part_number_byte_cursor)) {
                goto error_clean_up;
            }

            if (aws_byte_buf_append_dynamic(body_buffer, &s_close_part_number_tag)) {
                goto error_clean_up;
            }

            if (mpu_algorithm_checksum_name.len) {
                struct aws_byte_cursor checksum = aws_byte_cursor_from_buf(&part->checksum_base64);

                if (aws_byte_buf_append_dynamic(body_buffer, &s_open_start_bracket)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &mpu_algorithm_checksum_name)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &s_close_bracket)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &checksum)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &s_open_end_bracket)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &mpu_algorithm_checksum_name)) {
                    goto error_clean_up;
                }
                if (aws_byte_buf_append_dynamic(body_buffer, &s_close_bracket_new_line)) {
                    goto error_clean_up;
                }
            }
            if (aws_byte_buf_append_dynamic(body_buffer, &s_close_part_tag)) {
                goto error_clean_up;
            }
        }

        if (aws_byte_buf_append_dynamic(body_buffer, &s_complete_payload_end)) {
            goto error_clean_up;
        }

        AWS_LOGF_TRACE(
            AWS_LS_S3_GENERAL, "Payload for Complete MPU is:\n" PRInSTR "\n", AWS_BYTE_BUF_PRI(*body_buffer));
        aws_s3_message_util_assign_body(
            allocator, body_buffer, message, NULL /* checksum_config */, NULL /* out_checksum */);
    }

    return message;

error_clean_up:

    AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "Could not create complete multipart message");

    if (message != NULL) {
        aws_http_message_release(message);
        message = NULL;
    }

    return NULL;
}

struct aws_http_message *aws_s3_abort_multipart_upload_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    const struct aws_string *upload_id) {

    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_filter_headers(
        allocator,
        base_message,
        g_s3_abort_multipart_upload_excluded_headers,
        AWS_ARRAY_SIZE(g_s3_abort_multipart_upload_excluded_headers),
        true /*exclude_x_amz_meta*/);

    if (aws_s3_message_util_set_multipart_request_path(allocator, upload_id, 0, false, message)) {
        goto error_clean_up;
    }
    aws_http_message_set_request_method(message, g_delete_method);

    return message;

error_clean_up:

    AWS_LOGF_ERROR(AWS_LS_S3_GENERAL, "Could not create abort multipart upload message");

    if (message != NULL) {
        aws_http_message_release(message);
        message = NULL;
    }

    return NULL;
}

/**
 * Calculate the in memory checksum based on the checksum config. Initialize and set the out_checksum to the encoded
 * checksum result
 */
static int s_calculate_in_memory_checksum_helper(
    struct aws_allocator *allocator,
    struct aws_byte_cursor data,
    const struct checksum_config_storage *checksum_config,
    struct aws_byte_buf *out_checksum) {
    AWS_ASSERT(checksum_config->checksum_algorithm != AWS_SCA_NONE);
    AWS_ASSERT(out_checksum != NULL);
    AWS_ZERO_STRUCT(*out_checksum);

    int ret_code = AWS_OP_ERR;
    size_t digest_size = aws_get_digest_size_from_checksum_algorithm(checksum_config->checksum_algorithm);
    size_t encoded_checksum_len = 0;
    if (aws_base64_compute_encoded_len(digest_size, &encoded_checksum_len)) {
        return AWS_OP_ERR;
    }

    aws_byte_buf_init(out_checksum, allocator, encoded_checksum_len);

    struct aws_byte_buf raw_checksum;
    aws_byte_buf_init(&raw_checksum, allocator, digest_size);

    if (aws_checksum_compute(allocator, checksum_config->checksum_algorithm, &data, &raw_checksum)) {
        goto done;
    }
    struct aws_byte_cursor raw_checksum_cursor = aws_byte_cursor_from_buf(&raw_checksum);
    if (aws_base64_encode(&raw_checksum_cursor, out_checksum)) {
        goto done;
    }

    ret_code = AWS_OP_SUCCESS;
done:
    if (ret_code) {
        aws_byte_buf_clean_up(out_checksum);
    }
    aws_byte_buf_clean_up(&raw_checksum);
    return ret_code;
}

/**
 * Calculate the in memory checksum based on the checksum config.
 * If out_checksum set, initialize and set it to the encoded checksum result.
 * Set the corresponding header in out_message to the encoded checksum result.
 */
static int s_calculate_and_add_checksum_to_header_helper(
    struct aws_allocator *allocator,
    struct aws_byte_cursor data,
    const struct checksum_config_storage *checksum_config,
    struct aws_http_message *out_message,
    struct aws_byte_buf *out_checksum) {
    AWS_ASSERT(checksum_config->checksum_algorithm != AWS_SCA_NONE);
    AWS_ASSERT(out_message != NULL);
    int ret_code = AWS_OP_ERR;

    struct aws_byte_buf local_encoded_checksum_buf;
    struct aws_byte_buf *local_encoded_checksum;
    if (out_checksum == NULL) {
        local_encoded_checksum = &local_encoded_checksum_buf;
    } else {
        local_encoded_checksum = out_checksum;
    }
    AWS_ZERO_STRUCT(*local_encoded_checksum);
    if (s_calculate_in_memory_checksum_helper(allocator, data, checksum_config, local_encoded_checksum)) {
        goto done;
    }

    /* Add the encoded checksum to header. */
    const struct aws_byte_cursor header_name =
        aws_get_http_header_name_from_checksum_algorithm(checksum_config->checksum_algorithm);
    struct aws_byte_cursor encoded_checksum_val = aws_byte_cursor_from_buf(local_encoded_checksum);
    struct aws_http_headers *headers = aws_http_message_get_headers(out_message);
    if (aws_http_headers_set(headers, header_name, encoded_checksum_val)) {
        goto done;
    }

    ret_code = AWS_OP_SUCCESS;
done:
    if (ret_code || out_checksum == NULL) {
        /* In case of error happen or out_checksum is not set, clean up the encoded checksum. Otherwise, the caller will
         * own the encoded checksum. */
        aws_byte_buf_clean_up(local_encoded_checksum);
    }
    return ret_code;
}

/* Assign a buffer to an HTTP message, creating a stream and setting the content-length header */
struct aws_input_stream *aws_s3_message_util_assign_body(
    struct aws_allocator *allocator,
    struct aws_byte_buf *byte_buf,
    struct aws_http_message *out_message,
    const struct checksum_config_storage *checksum_config,
    struct aws_byte_buf *out_checksum) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(out_message);
    AWS_PRECONDITION(byte_buf);

    struct aws_byte_cursor buffer_byte_cursor = aws_byte_cursor_from_buf(byte_buf);
    struct aws_http_headers *headers = aws_http_message_get_headers(out_message);

    if (headers == NULL) {
        return NULL;
    }

    struct aws_input_stream *input_stream = aws_input_stream_new_from_cursor(allocator, &buffer_byte_cursor);
    struct aws_byte_buf content_encoding_header_buf;
    AWS_ZERO_STRUCT(content_encoding_header_buf);

    if (input_stream == NULL) {
        goto error_clean_up;
    }

    if (checksum_config) {
        if (checksum_config->location == AWS_SCL_TRAILER) {
            /* aws-chunked encode the payload and add related headers */

            /* set Content-Encoding header. If the header already exists, append the exisiting value to aws-chunked
             * We already made sure that the existing value is not 'aws_chunked' in 'aws_s3_client_make_meta_request'
             * function.
             */
            struct aws_byte_cursor content_encoding_header_cursor;
            bool has_content_encoding_header =
                aws_http_headers_get(headers, g_content_encoding_header_name, &content_encoding_header_cursor) ==
                AWS_OP_SUCCESS;
            size_t content_encoding_header_buf_size =
                has_content_encoding_header
                    ? g_content_encoding_header_aws_chunked.len + content_encoding_header_cursor.len + 1
                    : g_content_encoding_header_aws_chunked.len;
            aws_byte_buf_init(&content_encoding_header_buf, allocator, content_encoding_header_buf_size);

            if (has_content_encoding_header) {
                aws_byte_buf_append_dynamic(&content_encoding_header_buf, &content_encoding_header_cursor);
                aws_byte_buf_append_byte_dynamic(&content_encoding_header_buf, ',');
            }
            aws_byte_buf_append_dynamic(&content_encoding_header_buf, &g_content_encoding_header_aws_chunked);

            if (aws_http_headers_set(
                    headers, g_content_encoding_header_name, aws_byte_cursor_from_buf(&content_encoding_header_buf))) {
                goto error_clean_up;
            }

            /* set x-amz-trailer header */
            if (aws_http_headers_set(
                    headers,
                    g_trailer_header_name,
                    aws_get_http_header_name_from_checksum_algorithm(checksum_config->checksum_algorithm))) {
                goto error_clean_up;
            }
            /* set x-amz-decoded-content-length header */
            char decoded_content_length_buffer[64] = "";
            snprintf(
                decoded_content_length_buffer,
                sizeof(decoded_content_length_buffer),
                "%" PRIu64,
                (uint64_t)buffer_byte_cursor.len);
            struct aws_byte_cursor decode_content_length_cursor =
                aws_byte_cursor_from_array(decoded_content_length_buffer, strlen(decoded_content_length_buffer));
            if (aws_http_headers_set(headers, g_decoded_content_length_header_name, decode_content_length_cursor)) {
                goto error_clean_up;
            }
            /* set input stream to chunk stream */
            struct aws_input_stream *chunk_stream =
                aws_chunk_stream_new(allocator, input_stream, checksum_config->checksum_algorithm, out_checksum);
            if (!chunk_stream) {
                goto error_clean_up;
            }
            aws_input_stream_release(input_stream);
            input_stream = chunk_stream;
        } else if (checksum_config->location == AWS_SCL_HEADER) {
            /* Calculate the checksum directly from memory and add it to the header. */
            if (s_calculate_and_add_checksum_to_header_helper(
                    allocator, buffer_byte_cursor, checksum_config, out_message, out_checksum)) {
                goto error_clean_up;
            }

        } else if (checksum_config->checksum_algorithm != AWS_SCA_NONE && out_checksum != NULL) {
            /* In case checksums still wanted, and we can calculate it directly from the buffer in memory to
             * out_checksum */
            if (s_calculate_in_memory_checksum_helper(allocator, buffer_byte_cursor, checksum_config, out_checksum)) {
                goto error_clean_up;
            }
        }
    }
    int64_t stream_length = 0;
    if (aws_input_stream_get_length(input_stream, &stream_length)) {
        goto error_clean_up;
    }
    char content_length_buffer[64] = "";
    snprintf(content_length_buffer, sizeof(content_length_buffer), "%" PRIu64, (uint64_t)stream_length);
    struct aws_byte_cursor content_length_cursor =
        aws_byte_cursor_from_array(content_length_buffer, strlen(content_length_buffer));
    if (aws_http_headers_set(headers, g_content_length_header_name, content_length_cursor)) {
        goto error_clean_up;
    }

    aws_http_message_set_body_stream(out_message, input_stream);
    /* Let the message take the full ownership */
    aws_input_stream_release(input_stream);
    aws_byte_buf_clean_up(&content_encoding_header_buf);
    return input_stream;

error_clean_up:
    AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "Failed to assign body for s3 request http message, from body buffer .");
    aws_input_stream_release(input_stream);
    aws_byte_buf_clean_up(&content_encoding_header_buf);
    return NULL;
}

/* Add a content-md5 header. */
int aws_s3_message_util_add_content_md5_header(
    struct aws_allocator *allocator,
    struct aws_byte_buf *input_buf,
    struct aws_http_message *out_message) {

    AWS_PRECONDITION(out_message);

    /* Compute MD5 */
    struct aws_byte_cursor md5_input = aws_byte_cursor_from_buf(input_buf);
    uint8_t md5_output[AWS_MD5_LEN];
    struct aws_byte_buf md5_output_buf = aws_byte_buf_from_empty_array(md5_output, sizeof(md5_output));
    if (aws_md5_compute(allocator, &md5_input, &md5_output_buf, 0)) {
        return AWS_OP_ERR;
    }

    /* Compute Base64 encoding of MD5 */
    struct aws_byte_cursor base64_input = aws_byte_cursor_from_buf(&md5_output_buf);
    size_t base64_output_size = 0;
    if (aws_base64_compute_encoded_len(md5_output_buf.len, &base64_output_size)) {
        return AWS_OP_ERR;
    }
    struct aws_byte_buf base64_output_buf;
    aws_byte_buf_init(&base64_output_buf, allocator, base64_output_size);
    if (aws_base64_encode(&base64_input, &base64_output_buf)) {
        goto error_clean_up;
    }

    struct aws_http_headers *headers = aws_http_message_get_headers(out_message);
    if (aws_http_headers_set(headers, g_content_md5_header_name, aws_byte_cursor_from_buf(&base64_output_buf))) {
        goto error_clean_up;
    }

    aws_byte_buf_clean_up(&base64_output_buf);
    return AWS_OP_SUCCESS;

error_clean_up:

    aws_byte_buf_clean_up(&base64_output_buf);
    return AWS_OP_ERR;
}

/* Copy an existing HTTP message's headers, method, and path. */
struct aws_http_message *aws_s3_message_util_copy_http_message_no_body_all_headers(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message) {

    return aws_s3_message_util_copy_http_message_no_body_filter_headers(allocator, base_message, NULL, 0, false);
}

struct aws_http_message *aws_s3_message_util_copy_http_message_no_body_filter_headers(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    const struct aws_byte_cursor *excluded_header_array,
    size_t excluded_header_array_size,
    bool exclude_x_amz_meta) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(base_message);

    struct aws_http_message *message = aws_http_message_new_request(allocator);
    AWS_ASSERT(message);

    struct aws_byte_cursor request_method;
    if (aws_http_message_get_request_method(base_message, &request_method)) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "Failed to get request method.");
        goto error_clean_up;
    }

    if (aws_http_message_set_request_method(message, request_method)) {
        goto error_clean_up;
    }

    struct aws_byte_cursor request_path;
    if (aws_http_message_get_request_path(base_message, &request_path)) {
        AWS_LOGF_ERROR(AWS_LS_S3_CLIENT, "Failed to get request path.");
        goto error_clean_up;
    }

    if (aws_http_message_set_request_path(message, request_path)) {
        goto error_clean_up;
    }

    aws_s3_message_util_copy_headers(
        base_message, message, excluded_header_array, excluded_header_array_size, exclude_x_amz_meta);

    return message;

error_clean_up:
    aws_http_message_release(message);
    return NULL;
}

void aws_s3_message_util_copy_headers(
    struct aws_http_message *source_message,
    struct aws_http_message *dest_message,
    const struct aws_byte_cursor *excluded_header_array,
    size_t excluded_header_array_size,
    bool exclude_x_amz_meta) {

    size_t num_headers = aws_http_message_get_header_count(source_message);

    for (size_t header_index = 0; header_index < num_headers; ++header_index) {
        struct aws_http_header header;

        int error = aws_http_message_get_header(source_message, &header, header_index);

        if (excluded_header_array && excluded_header_array_size > 0) {
            bool exclude_header = false;

            for (size_t exclude_index = 0; exclude_index < excluded_header_array_size; ++exclude_index) {
                if (aws_byte_cursor_eq_ignore_case(&header.name, &excluded_header_array[exclude_index])) {
                    exclude_header = true;
                    break;
                }
            }

            if (exclude_header) {
                continue;
            }
        }

        if (exclude_x_amz_meta) {
            if (aws_byte_cursor_starts_with_ignore_case(&header.name, &s_x_amz_meta_prefix)) {
                continue;
            }
        }

        error |= aws_http_message_add_header(dest_message, header);
        (void)error;
        AWS_ASSERT(!error);
    }
}

/* Add a range header.*/
static void s_s3_message_util_add_range_header(
    uint64_t part_range_start,
    uint64_t part_range_end,
    struct aws_http_message *out_message) {
    AWS_PRECONDITION(out_message);

    /* ((2^64)-1 = 20 characters;  2*20 + length-of("bytes=-") < 128) */
    char range_value_buffer[128] = "";
    snprintf(
        range_value_buffer, sizeof(range_value_buffer), "bytes=%" PRIu64 "-%" PRIu64, part_range_start, part_range_end);

    struct aws_http_header range_header;
    AWS_ZERO_STRUCT(range_header);
    range_header.name = g_range_header_name;
    range_header.value = aws_byte_cursor_from_c_str(range_value_buffer);

    struct aws_http_headers *headers = aws_http_message_get_headers(out_message);
    AWS_ASSERT(headers != NULL);

    int erase_result = aws_http_headers_erase(headers, range_header.name);
    AWS_ASSERT(erase_result == AWS_OP_SUCCESS || aws_last_error() == AWS_ERROR_HTTP_HEADER_NOT_FOUND);

    /* Only failed when the header has invalid name, which is impossible here. */
    erase_result = aws_http_message_add_header(out_message, range_header);
    AWS_ASSERT(erase_result == AWS_OP_SUCCESS);
    (void)erase_result;
}

/* Handle setting up the multipart request path for a message. */
int aws_s3_message_util_set_multipart_request_path(
    struct aws_allocator *allocator,
    const struct aws_string *upload_id,
    uint32_t part_number,
    bool append_uploads_suffix,
    struct aws_http_message *message) {

    const struct aws_byte_cursor question_mark = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("?");
    const struct aws_byte_cursor ampersand = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("&");

    const struct aws_byte_cursor uploads_suffix = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("uploads");
    const struct aws_byte_cursor part_number_arg = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("partNumber=");
    const struct aws_byte_cursor upload_id_arg = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("uploadId=");

    struct aws_byte_buf request_path_buf;
    struct aws_byte_cursor request_path;

    if (aws_http_message_get_request_path(message, &request_path)) {
        return AWS_OP_ERR;
    }

    aws_byte_buf_init(&request_path_buf, allocator, request_path.len);

    if (aws_byte_buf_append_dynamic(&request_path_buf, &request_path)) {
        goto error_clean_up;
    }

    bool has_existing_query_parameters = false;

    for (size_t i = 0; i < request_path.len; ++i) {
        if (request_path.ptr[i] == '?') {
            has_existing_query_parameters = true;
            break;
        }
    }

    if (part_number > 0) {
        if (aws_byte_buf_append_dynamic(
                &request_path_buf, has_existing_query_parameters ? &ampersand : &question_mark)) {
            goto error_clean_up;
        }

        if (aws_byte_buf_append_dynamic(&request_path_buf, &part_number_arg)) {
            goto error_clean_up;
        }

        char part_number_buffer[32] = "";
        snprintf(part_number_buffer, sizeof(part_number_buffer), "%d", part_number);
        struct aws_byte_cursor part_number_cursor =
            aws_byte_cursor_from_array(part_number_buffer, strlen(part_number_buffer));

        if (aws_byte_buf_append_dynamic(&request_path_buf, &part_number_cursor)) {
            goto error_clean_up;
        }

        has_existing_query_parameters = true;
    }

    if (upload_id != NULL) {

        struct aws_byte_cursor upload_id_cursor = aws_byte_cursor_from_string(upload_id);

        if (aws_byte_buf_append_dynamic(
                &request_path_buf, has_existing_query_parameters ? &ampersand : &question_mark)) {
            goto error_clean_up;
        }

        if (aws_byte_buf_append_dynamic(&request_path_buf, &upload_id_arg)) {
            goto error_clean_up;
        }

        if (aws_byte_buf_append_dynamic(&request_path_buf, &upload_id_cursor)) {
            goto error_clean_up;
        }

        has_existing_query_parameters = true;
    }

    if (append_uploads_suffix) {
        if (aws_byte_buf_append_dynamic(
                &request_path_buf, has_existing_query_parameters ? &ampersand : &question_mark)) {
            goto error_clean_up;
        }

        if (aws_byte_buf_append_dynamic(&request_path_buf, &uploads_suffix)) {
            goto error_clean_up;
        }

        has_existing_query_parameters = true;
    }

    struct aws_byte_cursor new_request_path = aws_byte_cursor_from_buf(&request_path_buf);

    if (aws_http_message_set_request_path(message, new_request_path)) {
        goto error_clean_up;
    }

    aws_byte_buf_clean_up(&request_path_buf);
    return AWS_OP_SUCCESS;

error_clean_up:

    aws_byte_buf_clean_up(&request_path_buf);

    return AWS_OP_ERR;
}
