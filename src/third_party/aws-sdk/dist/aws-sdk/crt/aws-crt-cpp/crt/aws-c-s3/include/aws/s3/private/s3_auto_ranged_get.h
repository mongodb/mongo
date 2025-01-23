#ifndef AWS_S3_AUTO_RANGED_GET_H
#define AWS_S3_AUTO_RANGED_GET_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_meta_request_impl.h"

enum aws_s3_auto_ranged_get_request_type {
    AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_HEAD_OBJECT,
    AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_RANGE,
    AWS_S3_AUTO_RANGE_GET_REQUEST_TYPE_GET_OBJECT_WITH_PART_NUMBER_1,
};

struct aws_s3_auto_ranged_get {
    struct aws_s3_meta_request base;

    enum aws_s3_checksum_algorithm validation_algorithm;

    struct aws_string *etag;

    bool initial_message_has_start_range;
    bool initial_message_has_end_range;
    uint64_t initial_range_start;
    uint64_t initial_range_end;

    uint64_t object_size_hint;
    bool object_size_hint_available;

    /* Members to only be used when the mutex in the base type is locked. */
    struct {
        /* The starting byte of the data that we will be retrieved from the object.
         * (ignore this if object_range_empty) */
        uint64_t object_range_start;

        /* The last byte of the data that will be retrieved from the object.
         * (ignore this if object_range_empty)
         * Note this is inclusive: https://developer.mozilla.org/en-US/docs/Web/HTTP/Range_requests
         * So if begin=0 and end=0 then 1 byte is being downloaded. */
        uint64_t object_range_end;

        uint64_t first_part_size;

        /* The total number of parts that are being used in downloading the object range. Note that "part" here
         * currently refers to a range-get, and does not require a "part" on the service side. */
        uint32_t total_num_parts;

        uint32_t num_parts_requested;
        uint32_t num_parts_completed;
        uint32_t num_parts_successful;
        uint32_t num_parts_failed;
        uint32_t num_parts_checksum_validated;

        uint32_t object_range_known : 1;

        /* True if object_range_known, and it's found to be empty.
         * If this is true, ignore object_range_start and object_range_end */
        uint32_t object_range_empty : 1;
        uint32_t head_object_sent : 1;
        uint32_t head_object_completed : 1;
        uint32_t read_window_warning_issued : 1;
    } synced_data;

    uint32_t initial_message_has_range_header : 1;
    uint32_t initial_message_has_if_match_header : 1;
};

AWS_EXTERN_C_BEGIN

/* Creates a new auto-ranged get meta request.  This will do multiple parallel ranged-gets when appropriate. */
AWS_S3_API struct aws_s3_meta_request *aws_s3_meta_request_auto_ranged_get_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    size_t part_size,
    const struct aws_s3_meta_request_options *options);

AWS_EXTERN_C_END

#endif /* AWS_S3_AUTO_RANGED_GET_H */
