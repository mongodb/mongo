#ifndef AWS_S3_COPY_OBJECT_H
#define AWS_S3_COPY_OBJECT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_meta_request_impl.h"

enum aws_s3_copy_object_request_tag {
    AWS_S3_COPY_OBJECT_REQUEST_TAG_GET_OBJECT_SIZE,
    AWS_S3_COPY_OBJECT_REQUEST_TAG_BYPASS,
    AWS_S3_COPY_OBJECT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD,
    AWS_S3_COPY_OBJECT_REQUEST_TAG_MULTIPART_COPY,
    AWS_S3_COPY_OBJECT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD,
    AWS_S3_COPY_OBJECT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD,

    AWS_S3_COPY_OBJECT_REQUEST_TAG_MAX,
};

struct aws_s3_copy_object {
    struct aws_s3_meta_request base;

    /* Usable after the Create Multipart Upload request succeeds. */
    struct aws_string *upload_id;

    /* Only meant for use in the update function, which is never called concurrently. */
    struct {
        uint32_t next_part_number;
    } threaded_update_data;

    /* Members to only be used when the mutex in the base type is locked. */
    struct {
        /* Array-list of `struct aws_s3_mpu_part_info *`.
         * If copying via multipart upload, we fill in this info as each part gets copied,
         * and it's used to generate the final CompleteMultipartUpload. */
        struct aws_array_list part_list;

        /* obtained through a HEAD request against the source object */
        uint64_t content_length;
        size_t part_size;

        uint32_t total_num_parts;
        uint32_t num_parts_sent;
        uint32_t num_parts_completed;
        uint32_t num_parts_successful;
        uint32_t num_parts_failed;

        struct aws_http_headers *needed_response_headers;

        int create_multipart_upload_error_code;
        int complete_multipart_upload_error_code;
        int abort_multipart_upload_error_code;

        uint32_t head_object_sent : 1;
        uint32_t head_object_completed : 1;
        uint32_t copy_request_bypass_sent : 1;
        uint32_t copy_request_bypass_completed : 1;
        uint32_t create_multipart_upload_sent : 1;
        uint32_t create_multipart_upload_completed : 1;
        uint32_t complete_multipart_upload_sent : 1;
        uint32_t complete_multipart_upload_completed : 1;
        uint32_t abort_multipart_upload_sent : 1;
        uint32_t abort_multipart_upload_completed : 1;

    } synced_data;
};

/* Creates a new CopyObject meta request. This will perform either
 * 1) A CopyObject S3 API call if the source object length is < 1 GB or
 * 2) a multipart copy in parallel otherwise.
 */
struct aws_s3_meta_request *aws_s3_meta_request_copy_object_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    const struct aws_s3_meta_request_options *options);

#endif
