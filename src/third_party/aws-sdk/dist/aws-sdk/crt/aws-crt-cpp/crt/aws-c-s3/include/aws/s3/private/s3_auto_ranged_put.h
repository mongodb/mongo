#ifndef AWS_S3_AUTO_RANGED_PUT_H
#define AWS_S3_AUTO_RANGED_PUT_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_meta_request_impl.h"
#include "s3_paginator.h"

enum aws_s3_auto_ranged_put_request_tag {
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_LIST_PARTS,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_PART,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD,
    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD,

    AWS_S3_AUTO_RANGED_PUT_REQUEST_TAG_MAX,
};

struct aws_s3_auto_ranged_put {
    struct aws_s3_meta_request base;

    /* Initialized either during creation in resume flow or as result of create multipart upload during normal flow. */
    struct aws_string *upload_id;

    /* Resume token used to resume the operation */
    struct aws_s3_meta_request_resume_token *resume_token;

    uint64_t content_length;
    bool has_content_length;

    /*
     * total_num_parts_from_content_length is calculated by content_length / part_size.
     * It will be 0 if there is no content_length.
     */
    uint32_t total_num_parts_from_content_length;

    /* Only meant for use in the update function, which is never called concurrently. */
    struct {
        /*
         * Next part number to send.
         * Note: this follows s3 part number convention and counting starts with 1.
         * Throughout codebase 0 based part numbers are usually referred to as part index.
         */
        uint32_t next_part_number;
    } threaded_update_data;

    /* Members to only be used when the mutex in the base type is locked. */
    struct {
        /* Array list of `struct aws_s3_mpu_part_info *`
         * Info about each part, that we need to remember for CompleteMultipartUpload.
         * This is updated as we upload each part.
         * If resuming an upload, we first call ListParts and store the details
         * of previously uploaded parts here. In this case, the array may start with gaps
         * (e.g. if parts 1 and 3 were previously uploaded, but not part 2). */
        struct aws_array_list part_list;

        struct aws_s3_paginated_operation *list_parts_operation;
        struct aws_string *list_parts_continuation_token;

        /* Number of parts we've started work on */
        uint32_t num_parts_started;
        /* Number of parts we've started, and we have no more work to do */
        uint32_t num_parts_completed;
        uint32_t num_parts_successful;
        uint32_t num_parts_failed;
        /* When content length is not known, requests are optimistically
         * scheduled, below represents how many requests were scheduled and had no
         * work to do*/
        uint32_t num_parts_noop;

        /* Number of parts we've started, but they're not done reading from stream yet.
         * Though reads are serial (only 1 part can be reading from stream at a time)
         * we may queue up more to minimize delays between each read. */
        uint32_t num_parts_pending_read;

        struct aws_http_headers *needed_response_headers;

        /* Whether body stream is exhausted. */
        bool is_body_stream_at_end;

        int list_parts_error_code;
        int create_multipart_upload_error_code;
        int complete_multipart_upload_error_code;
        int abort_multipart_upload_error_code;

        struct {
            /* Mark a single ListParts request has started or not */
            uint32_t started : 1;
            /* Mark ListParts need to continue or not */
            uint32_t continues : 1;
            /* Mark ListParts has completed all the pages or not */
            uint32_t completed : 1;
        } list_parts_state;
        uint32_t create_multipart_upload_sent : 1;
        uint32_t create_multipart_upload_completed : 1;
        uint32_t complete_multipart_upload_sent : 1;
        uint32_t complete_multipart_upload_completed : 1;
        uint32_t abort_multipart_upload_sent : 1;
        uint32_t abort_multipart_upload_completed : 1;

    } synced_data;
};

AWS_EXTERN_C_BEGIN

/* Creates a new auto-ranged put meta request.
 * This will do a multipart upload in parallel when appropriate.
 * Note: if has_content_length is false, content_length and num_parts are ignored.
 */

AWS_S3_API struct aws_s3_meta_request *aws_s3_meta_request_auto_ranged_put_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    size_t part_size,
    bool has_content_length,
    uint64_t content_length,
    uint32_t num_parts,
    const struct aws_s3_meta_request_options *options);

AWS_EXTERN_C_END

#endif /* AWS_S3_AUTO_RANGED_PUT_H */
