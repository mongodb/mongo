#ifndef AWS_S3_DEFAULT_META_REQUEST_H
#define AWS_S3_DEFAULT_META_REQUEST_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_meta_request_impl.h"

struct aws_s3_client;

struct aws_s3_meta_request_default {
    struct aws_s3_meta_request base;

    size_t content_length;

    /* Actual type for the single request (may be AWS_S3_REQUEST_TYPE_UNKNOWN) */
    enum aws_s3_request_type request_type;

    /* S3 operation name for the single request */
    struct aws_string *operation_name;

    /* Members to only be used when the mutex in the base type is locked. */
    struct {
        int cached_response_status;
        int request_error_code;

        uint32_t request_sent : 1;
        uint32_t request_completed : 1;

    } synced_data;
};

/* Creates a new default meta request. This will send the request as is and pass back the response. */
struct aws_s3_meta_request *aws_s3_meta_request_default_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    enum aws_s3_request_type request_type,
    uint64_t content_length,
    bool should_compute_content_md5,
    const struct aws_s3_meta_request_options *options);

#endif
