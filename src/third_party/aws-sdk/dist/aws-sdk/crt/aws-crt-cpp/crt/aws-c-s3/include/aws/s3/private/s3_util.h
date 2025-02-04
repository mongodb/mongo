#ifndef AWS_S3_UTIL_H
#define AWS_S3_UTIL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/* This file provides access to useful constants and simple utility functions. */

#include <aws/auth/signing_config.h>
#include <aws/common/byte_buf.h>
#include <aws/s3/s3_client.h>

#if ASSERT_LOCK_HELD
#    define ASSERT_SYNCED_DATA_LOCK_HELD(object)                                                                       \
        {                                                                                                              \
            int cached_error = aws_last_error();                                                                       \
            AWS_ASSERT(aws_mutex_try_lock(&(object)->synced_data.lock) == AWS_OP_ERR);                                 \
            aws_raise_error(cached_error);                                                                             \
        }
#else
#    define ASSERT_SYNCED_DATA_LOCK_HELD(object)
#endif
#define KB_TO_BYTES(kb) ((kb) * 1024)
#define MB_TO_BYTES(mb) ((mb) * 1024 * 1024)
#define GB_TO_BYTES(gb) ((gb) * 1024 * 1024 * 1024ULL)

#define MS_TO_NS(ms) ((uint64_t)(ms) * 1000000)
#define SEC_TO_NS(ms) ((uint64_t)(ms) * 1000000000)

struct aws_allocator;
struct aws_http_stream;
struct aws_http_headers;
struct aws_http_message;
struct aws_s3_client;
struct aws_s3_request;
struct aws_s3_meta_request;
struct aws_s3_checksum;

struct aws_cached_signing_config_aws {
    struct aws_allocator *allocator;
    struct aws_string *service;
    struct aws_string *region;
    struct aws_string *signed_body_value;

    struct aws_signing_config_aws config;
};

AWS_EXTERN_C_BEGIN

AWS_S3_API
extern const struct aws_byte_cursor g_content_md5_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_trailer_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_request_validation_mode;

AWS_S3_API
extern const struct aws_byte_cursor g_enabled;

/**
 * The checksum-algorithm header name used for CopyObject and CreateMultipartUpload
 */
AWS_S3_API
extern const struct aws_byte_cursor g_checksum_algorithm_header_name;

/**
 * The checksum-algorithm header name used for PutObject, UploadParts and PutObject*
 */
AWS_S3_API
extern const struct aws_byte_cursor g_sdk_checksum_algorithm_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_client_version;

AWS_S3_API
extern const struct aws_byte_cursor g_user_agent_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_user_agent_header_product_name;

AWS_S3_API
extern const struct aws_byte_cursor g_user_agent_header_platform;

AWS_S3_API
extern const struct aws_byte_cursor g_user_agent_header_unknown;

AWS_S3_API
extern const struct aws_byte_cursor g_acl_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_host_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_content_type_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_content_encoding_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_content_encoding_header_aws_chunked;

AWS_S3_API
extern const struct aws_byte_cursor g_content_length_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_decoded_content_length_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_etag_header_name;

AWS_S3_API
extern const size_t g_s3_min_upload_part_size;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_service_name;

AWS_S3_API
extern const struct aws_byte_cursor g_s3express_service_name;

AWS_S3_API
extern const struct aws_byte_cursor g_range_header_name;

extern const struct aws_byte_cursor g_if_match_header_name;

extern const struct aws_byte_cursor g_request_id_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_content_range_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_accept_ranges_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_mp_parts_count_header_name;

AWS_S3_API
extern const struct aws_byte_cursor g_post_method;

AWS_S3_API
extern const struct aws_byte_cursor g_head_method;

AWS_S3_API
extern const struct aws_byte_cursor g_delete_method;

AWS_S3_API
extern const uint32_t g_s3_max_num_upload_parts;

/**
 * Returns AWS_S3_REQUEST_TYPE_UNKNOWN if name doesn't map to an enum value.
 */
AWS_S3_API
enum aws_s3_request_type aws_s3_request_type_from_operation_name(struct aws_byte_cursor name);

/**
 * Return operation name for aws_s3_request_type as static-lifetime aws_string*
 * or NULL if the type doesn't map to an actual operation.
 * For example:
 * AWS_S3_REQUEST_TYPE_HEAD_OBJECT -> static aws_string "HeadObject"
 * AWS_S3_REQUEST_TYPE_UNKNOWN -> NULL
 * AWS_S3_REQUEST_TYPE_MAX -> NULL
 */
AWS_S3_API
struct aws_string *aws_s3_request_type_to_operation_name_static_string(enum aws_s3_request_type type);

/**
 * Cache and initial the signing config based on the client.
 *
 * @param client
 * @param signing_config
 * @return struct aws_cached_signing_config_aws*
 */
struct aws_cached_signing_config_aws *aws_cached_signing_config_new(
    struct aws_s3_client *client,
    const struct aws_signing_config_aws *signing_config);

void aws_cached_signing_config_destroy(struct aws_cached_signing_config_aws *cached_signing_config);

/* Sets all headers specified for src on dest */
AWS_S3_API
void copy_http_headers(const struct aws_http_headers *src, struct aws_http_headers *dest);

/**
 * Get content of XML element at path.
 *
 * path_name_array must be a C array of char*, with a NULL as its final entry.
 *
 * For example:
 * Given `xml_doc`: "<Error><Code>SlowDown</Code></Error>"
 * And `path_name_array`: {"Error", "Code", NULL}
 * `out_body` will get set to: "SlowDown"
 *
 * Returns AWS_OP_SUCCESS or AWS_OP_ERR.
 * Raises AWS_ERROR_STRING_MATCH_NOT_FOUND if path not found in XML,
 * or AWS_ERROR_INVALID_XML if the XML can't be parsed.
 *
 * DO NOT make this function public without a lot of thought.
 * The whole thing of passing a C-array of C-strings with a NULL sentinel
 * is unconventional for this codebase.
 */
AWS_S3_API
int aws_xml_get_body_at_path(
    struct aws_allocator *allocator,
    struct aws_byte_cursor xml_doc,
    const char *path_name_array[],
    struct aws_byte_cursor *out_body);

/* replace &quot; with escaped /"
 * Returns initialized aws_byte_buf */
AWS_S3_API
struct aws_byte_buf aws_replace_quote_entities(struct aws_allocator *allocator, struct aws_byte_cursor src);

/* strip quotes if string is enclosed in quotes. does not remove quotes if they only appear on either side of the string
 */
AWS_S3_API
struct aws_string *aws_strip_quotes(struct aws_allocator *allocator, struct aws_byte_cursor in_cur);

/* TODO could be moved to aws-c-common. */
AWS_S3_API
int aws_last_error_or_unknown(void);

AWS_S3_API
void aws_s3_add_user_agent_header(struct aws_allocator *allocator, struct aws_http_message *message);

/* Given the response headers list, finds the Content-Range header and parses the range-start, range-end and
 * object-size. All output arguments are optional.*/
AWS_S3_API
int aws_s3_parse_content_range_response_header(
    struct aws_allocator *allocator,
    struct aws_http_headers *response_headers,
    uint64_t *out_range_start,
    uint64_t *out_range_end,
    uint64_t *out_object_size);

/* Given response headers, parses the content-length from a content-length response header.*/
AWS_S3_API
int aws_s3_parse_content_length_response_header(
    struct aws_allocator *allocator,
    struct aws_http_headers *response_headers,
    uint64_t *out_content_length);

/*
 * Given the request headers list, finds the Range header and parses the range-start and range-end. All arguments are
 * required.
 * */
AWS_S3_API
int aws_s3_parse_request_range_header(
    struct aws_http_headers *request_headers,
    bool *out_has_start_range,
    bool *out_has_end_range,
    uint64_t *out_start_range,
    uint64_t *out_end_range);

/* Calculate the number of parts based on overall object-range and part_size. */
AWS_S3_API
uint32_t aws_s3_calculate_auto_ranged_get_num_parts(
    size_t part_size,
    uint64_t first_part_size,
    uint64_t object_range_start,
    uint64_t object_range_end);

/**
 * Calculates the optimal part size and num parts given the 'content_length' and 'client_part_size'.
 * This will increase the part size to stay within S3's number of parts.
 * If the required part size exceeds the 'client_max_part_size' or
 * if the system cannot support the required part size, it will raise an 'AWS_ERROR_INVALID_ARGUMENT' argument.
 */
AWS_S3_API
int aws_s3_calculate_optimal_mpu_part_size_and_num_parts(
    uint64_t content_length,
    size_t client_part_size,
    uint64_t client_max_part_size,
    size_t *out_part_size,
    uint32_t *out_num_parts);

/* Calculates the part range for a part given overall object range, size of each part, and the part's number. Note: part
 * numbers begin at one. Intended to be used in conjunction
 * with aws_s3_calculate_auto_ranged_get_num_parts. part_number should be less than or equal to the result of
 * aws_s3_calculate_auto_ranged_get_num_parts. */
AWS_S3_API
void aws_s3_calculate_auto_ranged_get_part_range(
    uint64_t object_range_start,
    uint64_t object_range_end,
    size_t part_size,
    uint64_t first_part_size,
    uint32_t part_number,
    uint64_t *out_part_range_start,
    uint64_t *out_part_range_end);

/* Match the retry-able S3 error code to CRT error code, return AWS_ERROR_UNKNOWN when not matched. */
AWS_S3_API
int aws_s3_crt_error_code_from_recoverable_server_error_code_string(struct aws_byte_cursor error_code_string);

AWS_S3_API
void aws_s3_request_finish_up_metrics_synced(struct aws_s3_request *request, struct aws_s3_meta_request *meta_request);

/* Check the response headers for checksum to verify, return a running checksum based on the algorithm found. If no
 * checksum found from header, return null. */
AWS_S3_API
int aws_s3_check_headers_for_checksum(
    struct aws_s3_meta_request *meta_request,
    const struct aws_http_headers *headers,
    struct aws_s3_checksum **out_checksum,
    struct aws_byte_buf *out_checksum_buffer,
    bool meta_request_level);

AWS_EXTERN_C_END

#endif /* AWS_S3_UTIL_H */
