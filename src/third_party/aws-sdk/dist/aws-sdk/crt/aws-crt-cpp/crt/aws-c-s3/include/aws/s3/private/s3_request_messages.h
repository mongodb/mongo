#ifndef AWS_S3_REQUEST_MESSAGES_H
#define AWS_S3_REQUEST_MESSAGES_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include "aws/s3/s3.h"
#include "aws/s3/s3_client.h"
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>

struct aws_allocator;
struct aws_http_message;
struct aws_byte_buf;
struct aws_byte_cursor;
struct aws_string;
struct aws_array_list;
struct checksum_config_storage;

AWS_EXTERN_C_BEGIN

/* Copy message (but not the body) and retain all headers */
AWS_S3_API
struct aws_http_message *aws_s3_message_util_copy_http_message_no_body_all_headers(
    struct aws_allocator *allocator,
    struct aws_http_message *message);

/* Copy message (but not the body) and exclude specific headers.
 * exclude_x_amz_meta controls whether S3 user metadata headers (prefixed with "x-amz-meta) are excluded.*/
AWS_S3_API
struct aws_http_message *aws_s3_message_util_copy_http_message_no_body_filter_headers(
    struct aws_allocator *allocator,
    struct aws_http_message *message,
    const struct aws_byte_cursor *excluded_headers_arrays,
    size_t excluded_headers_size,
    bool exclude_x_amz_meta);

/* Copy headers from one message to the other and exclude specific headers.
 * exclude_x_amz_meta controls whether S3 user metadata headers (prefixed with "x-amz-meta) are excluded.*/
AWS_S3_API
void aws_s3_message_util_copy_headers(
    struct aws_http_message *source_message,
    struct aws_http_message *dest_message,
    const struct aws_byte_cursor *excluded_headers_arrays,
    size_t excluded_headers_size,
    bool exclude_x_amz_meta);

AWS_S3_API
struct aws_input_stream *aws_s3_message_util_assign_body(
    struct aws_allocator *allocator,
    struct aws_byte_buf *byte_buf,
    struct aws_http_message *out_message,
    const struct checksum_config_storage *checksum_config,
    struct aws_byte_buf *out_checksum);

/* Create an HTTP request for an S3 Ranged Get Object Request, using the given request as a basis */
AWS_S3_API
struct aws_http_message *aws_s3_ranged_get_object_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    uint64_t range_start,
    uint64_t range_end);

AWS_S3_API
int aws_s3_message_util_set_multipart_request_path(
    struct aws_allocator *allocator,
    const struct aws_string *upload_id,
    uint32_t part_number,
    bool append_uploads_suffix,
    struct aws_http_message *message);

/* Create an HTTP request for an S3 Create-Multipart-Upload request. */
AWS_S3_API
struct aws_http_message *aws_s3_create_multipart_upload_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    const struct checksum_config_storage *checksum_config);

/* Create an HTTP request for an S3 Put Object request, using the original request as a basis.  Creates and assigns a
 * body stream using the passed in buffer.  If multipart is not needed, part number and upload_id can be 0 and NULL,
 * respectively. */
AWS_S3_API
struct aws_http_message *aws_s3_upload_part_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *buffer,
    uint32_t part_number,
    const struct aws_string *upload_id,
    bool should_compute_content_md5,
    const struct checksum_config_storage *checksum_config,
    struct aws_byte_buf *encoded_checksum_output);

/* Create an HTTP request for an S3 UploadPartCopy request, using the original request as a basis.
 * If multipart is not needed, part number and upload_id can be 0 and NULL,
 * respectively. */
AWS_S3_API
struct aws_http_message *aws_s3_upload_part_copy_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *buffer,
    uint32_t part_number,
    uint64_t range_start,
    uint64_t range_end,
    const struct aws_string *upload_id,
    bool should_compute_content_md5);

/* Create an HTTP request for an S3 Complete-Multipart-Upload request. Creates the necessary XML payload using the
 * passed in array list of `struct aws_s3_mpu_part_info *`. Buffer passed in will be used to store
 * said XML payload, which will be used as the body. */
AWS_S3_API
struct aws_http_message *aws_s3_complete_multipart_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_buf *body_buffer,
    const struct aws_string *upload_id,
    const struct aws_array_list *parts,
    const struct checksum_config_storage *checksum_config);

AWS_S3_API
struct aws_http_message *aws_s3_abort_multipart_upload_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    const struct aws_string *upload_id);

/* Creates a HEAD GetObject request to get the size of the specified object. */
AWS_S3_API
struct aws_http_message *aws_s3_get_object_size_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message,
    struct aws_byte_cursor source_bucket,
    struct aws_byte_cursor source_key);

/* Creates a HEAD GetObject sub-request to get the size of the source object of a Copy meta request. */
AWS_S3_API
struct aws_http_message *aws_s3_get_source_object_size_message_new(
    struct aws_allocator *allocator,
    struct aws_http_message *base_message);

/* Add content-md5 header to the http message passed in. The MD5 will be computed from the input_buf */
AWS_S3_API
int aws_s3_message_util_add_content_md5_header(
    struct aws_allocator *allocator,
    struct aws_byte_buf *input_buf,
    struct aws_http_message *message);

AWS_S3_API
extern const struct aws_byte_cursor g_s3_create_multipart_upload_excluded_headers[];

AWS_S3_API
extern const size_t g_s3_create_multipart_upload_excluded_headers_count;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_upload_part_excluded_headers[];

AWS_S3_API
extern const size_t g_s3_upload_part_excluded_headers_count;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_complete_multipart_upload_excluded_headers[];

AWS_S3_API
extern const size_t g_s3_complete_multipart_upload_excluded_headers_count;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_abort_multipart_upload_excluded_headers[];

AWS_S3_API
extern const size_t g_s3_abort_multipart_upload_excluded_headers_count;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_list_parts_excluded_headers[];

AWS_S3_API extern const size_t g_s3_list_parts_excluded_headers_count;

AWS_S3_API
extern const struct aws_byte_cursor g_s3_list_parts_with_checksum_excluded_headers[];

AWS_S3_API
extern const size_t g_s3_list_parts_with_checksum_excluded_headers_count;

AWS_EXTERN_C_END

#endif /* AWS_S3_REQUEST_H */
