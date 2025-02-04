/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include "aws/s3/private/s3_copy_object.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>

/* Objects with size smaller than the constant below are bypassed as S3 CopyObject instead of multipart copy */
static const size_t s_multipart_copy_minimum_object_size = GB_TO_BYTES(1);

static const size_t s_complete_multipart_upload_init_body_size_bytes = 512;
static const size_t s_abort_multipart_upload_init_body_size_bytes = 512;

/* TODO: make this configurable or at least expose it. */
const size_t s_min_copy_part_size = MB_TO_BYTES(128);

static const struct aws_byte_cursor s_create_multipart_upload_copy_headers[] = {
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-algorithm"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-customer-key-MD5"),
    AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL("x-amz-server-side-encryption-context"),
};

static void s_s3_meta_request_copy_object_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_copy_object_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

static struct aws_future_void *s_s3_copy_object_prepare_request(struct aws_s3_request *request);

static void s_s3_copy_object_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static void s_s3_copy_object_sign_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    aws_signing_complete_fn *on_signing_complete,
    void *user_data);

static struct aws_s3_meta_request_vtable s_s3_copy_object_vtable = {
    .update = s_s3_copy_object_update,
    .send_request_finish = aws_s3_meta_request_send_request_finish_default,
    .prepare_request = s_s3_copy_object_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = s_s3_copy_object_sign_request,
    .finished_request = s_s3_copy_object_request_finished,
    .destroy = s_s3_meta_request_copy_object_destroy,
    .finish = aws_s3_meta_request_finish_default,
};

/* Allocate a new copy object meta request */
struct aws_s3_meta_request *aws_s3_meta_request_copy_object_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    const struct aws_s3_meta_request_options *options) {

    /* These should already have been validated by the caller. */
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);

    struct aws_s3_copy_object *copy_object = aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_copy_object));

    /* part size and content length will be fetched later using a HEAD object request */
    const size_t UNKNOWN_PART_SIZE = 0;
    const size_t UNKNOWN_CONTENT_LENGTH = 0;
    const int UNKNOWN_NUM_PARTS = 0;

    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            UNKNOWN_PART_SIZE,
            false,
            options,
            copy_object,
            &s_s3_copy_object_vtable,
            &copy_object->base)) {
        aws_mem_release(allocator, copy_object);
        return NULL;
    }

    aws_array_list_init_dynamic(
        &copy_object->synced_data.part_list, allocator, 0, sizeof(struct aws_s3_mpu_part_info *));

    copy_object->synced_data.content_length = UNKNOWN_CONTENT_LENGTH;
    copy_object->synced_data.total_num_parts = UNKNOWN_NUM_PARTS;
    copy_object->threaded_update_data.next_part_number = 1;

    AWS_LOGF_DEBUG(AWS_LS_S3_META_REQUEST, "id=%p Created new CopyObject Meta Request.", (void *)&copy_object->base);

    return &copy_object->base;
}

static void s_s3_meta_request_copy_object_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_copy_object *copy_object = meta_request->impl;

    aws_string_destroy(copy_object->upload_id);
    copy_object->upload_id = NULL;

    for (size_t part_index = 0; part_index < aws_array_list_length(&copy_object->synced_data.part_list); ++part_index) {
        struct aws_s3_mpu_part_info *part = NULL;
        aws_array_list_get_at(&copy_object->synced_data.part_list, &part, part_index);
        aws_string_destroy(part->etag);
        aws_byte_buf_clean_up(&part->checksum_base64);
        aws_mem_release(meta_request->allocator, part);
    }

    aws_array_list_clean_up(&copy_object->synced_data.part_list);
    aws_http_headers_release(copy_object->synced_data.needed_response_headers);
    aws_mem_release(meta_request->allocator, copy_object);
}

static bool s_s3_copy_object_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    struct aws_s3_copy_object *copy_object = meta_request->impl;

    aws_s3_meta_request_lock_synced_data(meta_request);

    if (!aws_s3_meta_request_has_finish_result_synced(meta_request)) {

        /* If we haven't already sent the GetObject HEAD request to get the source object size, do so now. */
        if (!copy_object->synced_data.head_object_sent) {
            request = aws_s3_request_new(
                meta_request,
                AWS_S3_COPY_OBJECT_REQUEST_TAG_GET_OBJECT_SIZE,
                AWS_S3_REQUEST_TYPE_HEAD_OBJECT,
                0 /*part_number*/,
                AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

            copy_object->synced_data.head_object_sent = true;

            goto has_work_remaining;
        }

        if (!copy_object->synced_data.head_object_completed) {
            /* we have not received the object size response yet */
            goto has_work_remaining;
        }

        if (copy_object->synced_data.content_length < s_multipart_copy_minimum_object_size) {
            /* object is too small to use multipart upload: forwards the original CopyObject request to S3 instead. */
            if (!copy_object->synced_data.copy_request_bypass_sent) {
                request = aws_s3_request_new(
                    meta_request,
                    AWS_S3_COPY_OBJECT_REQUEST_TAG_BYPASS,
                    AWS_S3_REQUEST_TYPE_COPY_OBJECT,
                    1 /*part_number*/,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p: Meta Request CopyObject created bypass request %p",
                    (void *)meta_request,
                    (void *)request);

                copy_object->synced_data.copy_request_bypass_sent = true;
                goto has_work_remaining;
            }

            /* If the bypass request hasn't been completed, then wait for it to be completed. */
            if (!copy_object->synced_data.copy_request_bypass_completed) {
                goto has_work_remaining;
            } else {
                goto no_work_remaining;
            }
        }

        /* Object size is large enough to use multipart copy. If we haven't already sent a create-multipart-upload
         * message, do so now. */
        if (!copy_object->synced_data.create_multipart_upload_sent) {
            request = aws_s3_request_new(
                meta_request,
                AWS_S3_COPY_OBJECT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD,
                AWS_S3_REQUEST_TYPE_CREATE_MULTIPART_UPLOAD,
                0 /*part_number*/,
                AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

            copy_object->synced_data.create_multipart_upload_sent = true;
            goto has_work_remaining;
        }

        /* If the create-multipart-upload message hasn't been completed, then there is still additional work to do, but
         * it can't be done yet. */
        if (!copy_object->synced_data.create_multipart_upload_completed) {
            goto has_work_remaining;
        }

        /* If we haven't sent all of the parts yet, then set up to send a new part now. */
        if (copy_object->synced_data.num_parts_sent < copy_object->synced_data.total_num_parts) {

            if ((flags & AWS_S3_META_REQUEST_UPDATE_FLAG_CONSERVATIVE) != 0) {
                uint32_t num_parts_in_flight =
                    (copy_object->synced_data.num_parts_sent - copy_object->synced_data.num_parts_completed);

                /* TODO: benchmark if there is need to limit the amount of upload part copy in flight requests */
                if (num_parts_in_flight > 0) {
                    goto has_work_remaining;
                }
            }

            /* Allocate a request for another part. */
            request = aws_s3_request_new(
                meta_request,
                AWS_S3_COPY_OBJECT_REQUEST_TAG_MULTIPART_COPY,
                AWS_S3_REQUEST_TYPE_UPLOAD_PART_COPY,
                copy_object->threaded_update_data.next_part_number,
                AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

            ++copy_object->threaded_update_data.next_part_number;
            ++copy_object->synced_data.num_parts_sent;

            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p: Returning request %p for part %d",
                (void *)meta_request,
                (void *)request,
                request->part_number);

            goto has_work_remaining;
        }

        /* There is one more request to send after all of the parts (the complete-multipart-upload) but it can't be done
         * until all of the parts have been completed.*/
        if (copy_object->synced_data.num_parts_completed != copy_object->synced_data.total_num_parts) {
            goto has_work_remaining;
        }

        /* If the complete-multipart-upload request hasn't been set yet, then send it now. */
        if (!copy_object->synced_data.complete_multipart_upload_sent) {
            request = aws_s3_request_new(
                meta_request,
                AWS_S3_COPY_OBJECT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD,
                AWS_S3_REQUEST_TYPE_COMPLETE_MULTIPART_UPLOAD,
                0 /*part_number*/,
                AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

            copy_object->synced_data.complete_multipart_upload_sent = true;
            goto has_work_remaining;
        }

        /* Wait for the complete-multipart-upload request to finish. */
        if (!copy_object->synced_data.complete_multipart_upload_completed) {
            goto has_work_remaining;
        }

        goto no_work_remaining;
    } else {

        /* If the create multipart upload hasn't been sent, then there is nothing left to do when canceling. */
        if (!copy_object->synced_data.create_multipart_upload_sent) {
            goto no_work_remaining;
        }

        /* If the create-multipart-upload request is still in flight, wait for it to finish. */
        if (!copy_object->synced_data.create_multipart_upload_completed) {
            goto has_work_remaining;
        }

        /* If the number of parts completed is less than the number of parts sent, then we need to wait until all of
         * those parts are done sending before aborting. */
        if (copy_object->synced_data.num_parts_completed < copy_object->synced_data.num_parts_sent) {
            goto has_work_remaining;
        }

        /* If the complete-multipart-upload is already in flight, then we can't necessarily send an abort. */
        if (copy_object->synced_data.complete_multipart_upload_sent &&
            !copy_object->synced_data.complete_multipart_upload_completed) {
            goto has_work_remaining;
        }

        /* If the complete-multipart-upload completed successfully, then there is nothing to abort since the transfer
         * has already finished. */
        if (copy_object->synced_data.complete_multipart_upload_completed &&
            copy_object->synced_data.complete_multipart_upload_error_code == AWS_ERROR_SUCCESS) {
            goto no_work_remaining;
        }

        /* If we made it here, and the abort-multipart-upload message hasn't been sent yet, then do so now. */
        if (!copy_object->synced_data.abort_multipart_upload_sent) {
            if (copy_object->upload_id == NULL) {
                goto no_work_remaining;
            }

            request = aws_s3_request_new(
                meta_request,
                AWS_S3_COPY_OBJECT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD,
                AWS_S3_REQUEST_TYPE_ABORT_MULTIPART_UPLOAD,
                0 /*part_number*/,
                AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS | AWS_S3_REQUEST_FLAG_ALWAYS_SEND);

            copy_object->synced_data.abort_multipart_upload_sent = true;

            goto has_work_remaining;
        }

        /* Wait for the multipart upload to be completed. */
        if (!copy_object->synced_data.abort_multipart_upload_completed) {
            goto has_work_remaining;
        }

        goto no_work_remaining;
    }

has_work_remaining:
    work_remaining = true;

no_work_remaining:
    /* If some events are still being delivered to caller, then wait for those to finish */
    if (!work_remaining && aws_s3_meta_request_are_events_out_for_delivery_synced(meta_request)) {
        work_remaining = true;
    }

    if (!work_remaining) {
        aws_s3_meta_request_set_success_synced(meta_request, AWS_HTTP_STATUS_CODE_200_OK);
    }

    aws_s3_meta_request_unlock_synced_data(meta_request);

    if (work_remaining) {
        *out_request = request;
    } else {
        AWS_ASSERT(request == NULL);

        aws_s3_meta_request_finish(meta_request);
    }

    return work_remaining;
}

/* Given a request, prepare it for sending based on its description. */
static struct aws_future_void *s_s3_copy_object_prepare_request(struct aws_s3_request *request) {
    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_copy_object *copy_object = meta_request->impl;
    AWS_PRECONDITION(copy_object);

    aws_s3_meta_request_lock_synced_data(meta_request);

    struct aws_http_message *message = NULL;
    bool success = false;

    switch (request->request_tag) {

        /* Prepares the GetObject HEAD request to get the source object size. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_GET_OBJECT_SIZE: {
            message = aws_s3_get_source_object_size_message_new(
                meta_request->allocator, meta_request->initial_request_message);
            break;
        }

        /* The S3 object is not large enough for multi-part copy. Bypasses a copy of the original CopyObject request to
         * S3. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_BYPASS: {
            message = aws_s3_message_util_copy_http_message_no_body_all_headers(
                meta_request->allocator, meta_request->initial_request_message);
            break;
        }

        /* Prepares the CreateMultipartUpload sub-request. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD: {
            uint64_t part_size_uint64 = copy_object->synced_data.content_length / (uint64_t)g_s3_max_num_upload_parts;

            if (part_size_uint64 > SIZE_MAX) {
                AWS_LOGF_ERROR(
                    AWS_LS_S3_META_REQUEST,
                    "Could not create multipart copy meta request; required part size of %" PRIu64
                    " bytes is too large for platform.",
                    part_size_uint64);

                aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
                goto finish;
            }

            uint64_t max_part_size = GB_TO_BYTES((uint64_t)5);
            if (max_part_size > SIZE_MAX) {
                max_part_size = SIZE_MAX;
            }
            uint32_t num_parts = 0;
            size_t part_size = 0;

            aws_s3_calculate_optimal_mpu_part_size_and_num_parts(
                copy_object->synced_data.content_length, s_min_copy_part_size, max_part_size, &part_size, &num_parts);

            copy_object->synced_data.total_num_parts = num_parts;
            copy_object->synced_data.part_size = part_size;

            /* Fill part_list */
            aws_array_list_ensure_capacity(&copy_object->synced_data.part_list, num_parts);
            while (aws_array_list_length(&copy_object->synced_data.part_list) < num_parts) {
                struct aws_s3_mpu_part_info *part =
                    aws_mem_calloc(meta_request->allocator, 1, sizeof(struct aws_s3_mpu_part_info));
                aws_array_list_push_back(&copy_object->synced_data.part_list, &part);
            }

            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "Starting multi-part Copy using part size=%zu, total_num_parts=%zu",
                part_size,
                (size_t)num_parts);

            /* Create the message to create a new multipart upload. */
            message = aws_s3_create_multipart_upload_message_new(
                meta_request->allocator, meta_request->initial_request_message, &meta_request->checksum_config);

            break;
        }

        /* Prepares the UploadPartCopy sub-request. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_MULTIPART_COPY: {
            /* Create a new uploadPartCopy message to upload a part. */
            /* compute sub-request range */
            /* note that range-end is inclusive */
            uint64_t range_start = (request->part_number - 1) * copy_object->synced_data.part_size;
            uint64_t range_end = range_start + copy_object->synced_data.part_size - 1;
            if (range_end >= copy_object->synced_data.content_length) {
                /* adjust size of last part */
                range_end = copy_object->synced_data.content_length - 1;
            }

            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "Starting UploadPartCopy for partition %" PRIu32 ", range_start=%" PRIu64 ", range_end=%" PRIu64
                ", full object length=%" PRIu64,
                request->part_number,
                range_start,
                range_end,
                copy_object->synced_data.content_length);

            message = aws_s3_upload_part_copy_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                &request->request_body,
                request->part_number,
                range_start,
                range_end,
                copy_object->upload_id,
                meta_request->should_compute_content_md5);
            break;
        }

        /* Prepares the CompleteMultiPartUpload sub-request. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD: {

            if (request->num_times_prepared == 0) {
                aws_byte_buf_init(
                    &request->request_body, meta_request->allocator, s_complete_multipart_upload_init_body_size_bytes);
            } else {
                aws_byte_buf_reset(&request->request_body, false);
            }

            AWS_FATAL_ASSERT(copy_object->upload_id);
            AWS_ASSERT(request->request_body.capacity > 0);
            aws_byte_buf_reset(&request->request_body, false);

            /* Build the message to complete our multipart upload, which includes a payload describing all of our
             * completed parts. */
            message = aws_s3_complete_multipart_message_new(
                meta_request->allocator,
                meta_request->initial_request_message,
                &request->request_body,
                copy_object->upload_id,
                &copy_object->synced_data.part_list,
                NULL);

            break;
        }

        /* Prepares the AbortMultiPartUpload sub-request. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD: {
            AWS_FATAL_ASSERT(copy_object->upload_id);
            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p Abort multipart upload request for upload id %s.",
                (void *)meta_request,
                aws_string_c_str(copy_object->upload_id));

            if (request->num_times_prepared == 0) {
                aws_byte_buf_init(
                    &request->request_body, meta_request->allocator, s_abort_multipart_upload_init_body_size_bytes);
            } else {
                aws_byte_buf_reset(&request->request_body, false);
            }

            /* Build the message to abort our multipart upload */
            message = aws_s3_abort_multipart_upload_message_new(
                meta_request->allocator, meta_request->initial_request_message, copy_object->upload_id);

            break;
        }
    }

    aws_s3_meta_request_unlock_synced_data(meta_request);

    if (message == NULL) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not allocate message for request with tag %d for CopyObject meta request.",
            (void *)meta_request,
            request->request_tag);
        goto finish;
    }

    aws_s3_request_setup_send_data(request, message);

    aws_http_message_release(message);

    /* Success! */
    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p: Prepared request %p for part %d",
        (void *)meta_request,
        (void *)request,
        request->part_number);

    success = true;

finish:;
    struct aws_future_void *future = aws_future_void_new(meta_request->allocator);
    if (success) {
        aws_future_void_set_result(future);
    } else {
        aws_future_void_set_error(future, aws_last_error_or_unknown());
    }
    return future;
}

/* For UploadPartCopy requests, etag is sent in the request body, within XML entity quotes */
static struct aws_string *s_etag_new_from_upload_part_copy_response(
    struct aws_allocator *allocator,
    struct aws_byte_buf *response_body) {

    struct aws_byte_cursor xml_doc = aws_byte_cursor_from_buf(response_body);
    struct aws_byte_cursor etag_within_xml_quotes = {0};
    const char *xml_path[] = {"CopyPartResult", "ETag", NULL};
    aws_xml_get_body_at_path(allocator, xml_doc, xml_path, &etag_within_xml_quotes);

    struct aws_byte_buf etag_within_quotes_byte_buf = aws_replace_quote_entities(allocator, etag_within_xml_quotes);

    struct aws_string *stripped_etag =
        aws_strip_quotes(allocator, aws_byte_cursor_from_buf(&etag_within_quotes_byte_buf));

    aws_byte_buf_clean_up(&etag_within_quotes_byte_buf);

    return stripped_etag;
}

static void s_s3_copy_object_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    struct aws_s3_copy_object *copy_object = meta_request->impl;
    aws_s3_meta_request_lock_synced_data(meta_request);
    switch (request->request_tag) {

        case AWS_S3_COPY_OBJECT_REQUEST_TAG_GET_OBJECT_SIZE: {
            if (error_code == AWS_ERROR_SUCCESS) {
                struct aws_byte_cursor content_length_cursor;
                if (!aws_http_headers_get(
                        request->send_data.response_headers, g_content_length_header_name, &content_length_cursor)) {

                    if (!aws_byte_cursor_utf8_parse_u64(
                            content_length_cursor, &copy_object->synced_data.content_length)) {
                        copy_object->synced_data.head_object_completed = true;
                    } else {
                        /* HEAD request returned an invalid content-length */
                        aws_s3_meta_request_set_fail_synced(
                            meta_request, request, AWS_ERROR_S3_INVALID_CONTENT_LENGTH_HEADER);
                    }
                } else {
                    /* HEAD request didn't return content-length header */
                    aws_s3_meta_request_set_fail_synced(
                        meta_request, request, AWS_ERROR_S3_INVALID_CONTENT_LENGTH_HEADER);
                }
            } else {
                aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            }

            break;
        }

        /* The S3 object is not large enough for multi-part copy. A copy of the original CopyObject request
         * was bypassed to S3 and is now finished. */
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_BYPASS: {

            /* Invoke headers callback if it was requested for this meta request */
            if (meta_request->headers_callback != NULL) {

                /* Invoke the callback without lock */
                aws_s3_meta_request_unlock_synced_data(meta_request);
                /* Notify the user of the headers. */
                if (meta_request->headers_callback(
                        meta_request,
                        request->send_data.response_headers,
                        request->send_data.response_status,
                        meta_request->user_data)) {

                    error_code = aws_last_error_or_unknown();
                }
                meta_request->headers_callback = NULL;
                /* Grab the lock again after the callback */
                aws_s3_meta_request_lock_synced_data(meta_request);
            }

            /* Signals completion of the meta request */
            if (error_code == AWS_ERROR_SUCCESS) {

                /* Send progress_callback for delivery on io_event_loop thread */
                if (meta_request->progress_callback != NULL) {
                    struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_PROGRESS};
                    event.u.progress.info.bytes_transferred = copy_object->synced_data.content_length;
                    event.u.progress.info.content_length = copy_object->synced_data.content_length;
                    aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
                }

                copy_object->synced_data.copy_request_bypass_completed = true;
            } else {
                /* Bypassed CopyObject request failed */
                aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            }
            break;
        }

        case AWS_S3_COPY_OBJECT_REQUEST_TAG_CREATE_MULTIPART_UPLOAD: {
            struct aws_http_headers *needed_response_headers = NULL;

            if (error_code == AWS_ERROR_SUCCESS) {
                needed_response_headers = aws_http_headers_new(meta_request->allocator);
                const size_t copy_header_count = AWS_ARRAY_SIZE(s_create_multipart_upload_copy_headers);

                /* Copy any headers now that we'll need for the final, transformed headers later. */
                for (size_t header_index = 0; header_index < copy_header_count; ++header_index) {
                    const struct aws_byte_cursor *header_name = &s_create_multipart_upload_copy_headers[header_index];
                    struct aws_byte_cursor header_value;
                    AWS_ZERO_STRUCT(header_value);

                    if (!aws_http_headers_get(request->send_data.response_headers, *header_name, &header_value)) {
                        aws_http_headers_set(needed_response_headers, *header_name, header_value);
                    }
                }

                struct aws_byte_cursor xml_doc = aws_byte_cursor_from_buf(&request->send_data.response_body);

                /* Find the upload id for this multipart upload. */
                struct aws_byte_cursor upload_id = {0};
                const char *xml_path[] = {"InitiateMultipartUploadResult", "UploadId", NULL};
                aws_xml_get_body_at_path(meta_request->allocator, xml_doc, xml_path, &upload_id);

                if (upload_id.len == 0) {
                    AWS_LOGF_ERROR(
                        AWS_LS_S3_META_REQUEST,
                        "id=%p Could not find upload-id in create-multipart-upload response",
                        (void *)meta_request);

                    aws_raise_error(AWS_ERROR_S3_MISSING_UPLOAD_ID);
                    error_code = AWS_ERROR_S3_MISSING_UPLOAD_ID;
                } else {
                    /* Store the multipart upload id. */
                    copy_object->upload_id = aws_string_new_from_cursor(meta_request->allocator, &upload_id);
                }
            }

            AWS_ASSERT(copy_object->synced_data.needed_response_headers == NULL);
            copy_object->synced_data.needed_response_headers = needed_response_headers;

            copy_object->synced_data.create_multipart_upload_completed = true;
            copy_object->synced_data.create_multipart_upload_error_code = error_code;

            if (error_code != AWS_ERROR_SUCCESS) {
                aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            }
            break;
        }

        case AWS_S3_COPY_OBJECT_REQUEST_TAG_MULTIPART_COPY: {
            size_t part_number = request->part_number;
            AWS_FATAL_ASSERT(part_number > 0);
            size_t part_index = part_number - 1;

            ++copy_object->synced_data.num_parts_completed;

            AWS_LOGF_DEBUG(
                AWS_LS_S3_META_REQUEST,
                "id=%p: %d out of %d parts have completed.",
                (void *)meta_request,
                copy_object->synced_data.num_parts_completed,
                copy_object->synced_data.total_num_parts);

            if (error_code == AWS_ERROR_SUCCESS) {
                struct aws_string *etag = s_etag_new_from_upload_part_copy_response(
                    meta_request->allocator, &request->send_data.response_body);

                AWS_ASSERT(etag != NULL);

                ++copy_object->synced_data.num_parts_successful;

                /* Send progress_callback for delivery on io_event_loop thread. */
                if (meta_request->progress_callback != NULL) {
                    struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_PROGRESS};
                    event.u.progress.info.bytes_transferred = copy_object->synced_data.part_size;
                    event.u.progress.info.content_length = copy_object->synced_data.content_length;
                    aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
                }

                struct aws_s3_mpu_part_info *part = NULL;
                aws_array_list_get_at(&copy_object->synced_data.part_list, &part, part_index);
                AWS_ASSERT(part != NULL);
                part->etag = etag;

            } else {
                ++copy_object->synced_data.num_parts_failed;
                aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            }

            break;
        }

        case AWS_S3_COPY_OBJECT_REQUEST_TAG_COMPLETE_MULTIPART_UPLOAD: {
            if (error_code == AWS_ERROR_SUCCESS && meta_request->headers_callback != NULL) {

                /* Copy over any response headers that we've previously determined are needed for this final
                 * response.
                 */
                copy_http_headers(
                    copy_object->synced_data.needed_response_headers, request->send_data.response_headers);

                struct aws_byte_cursor xml_doc = aws_byte_cursor_from_buf(&request->send_data.response_body);

                /* Grab the ETag for the entire object, and set it as a header. */
                struct aws_byte_cursor etag_header_value = {0};
                const char *xml_path[] = {"CompleteMultipartUploadResult", "ETag", NULL};
                aws_xml_get_body_at_path(meta_request->allocator, xml_doc, xml_path, &etag_header_value);
                if (etag_header_value.len > 0) {
                    struct aws_byte_buf etag_header_value_byte_buf =
                        aws_replace_quote_entities(meta_request->allocator, etag_header_value);

                    aws_http_headers_set(
                        request->send_data.response_headers,
                        g_etag_header_name,
                        aws_byte_cursor_from_buf(&etag_header_value_byte_buf));

                    aws_byte_buf_clean_up(&etag_header_value_byte_buf);
                }

                /* Notify the user of the headers. */
                /* Invoke the callback without lock */
                aws_s3_meta_request_unlock_synced_data(meta_request);
                if (meta_request->headers_callback(
                        meta_request,
                        request->send_data.response_headers,
                        request->send_data.response_status,
                        meta_request->user_data)) {

                    error_code = aws_last_error_or_unknown();
                }
                meta_request->headers_callback = NULL;
                /* Grab the lock again after the callback */
                aws_s3_meta_request_lock_synced_data(meta_request);
            }

            copy_object->synced_data.complete_multipart_upload_completed = true;
            copy_object->synced_data.complete_multipart_upload_error_code = error_code;

            if (error_code != AWS_ERROR_SUCCESS) {
                aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
            }

            break;
        }
        case AWS_S3_COPY_OBJECT_REQUEST_TAG_ABORT_MULTIPART_UPLOAD: {
            copy_object->synced_data.abort_multipart_upload_error_code = error_code;
            copy_object->synced_data.abort_multipart_upload_completed = true;
            break;
        }
    }

    aws_s3_request_finish_up_metrics_synced(request, meta_request);

    aws_s3_meta_request_unlock_synced_data(meta_request);
}

static void s_s3_copy_object_sign_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    aws_signing_complete_fn *on_signing_complete,
    void *user_data) {

    /**
     * https://docs.aws.amazon.com/AmazonS3/latest/API/API_UploadPartCopy.html
     * https://docs.aws.amazon.com/AmazonS3/latest/API/API_CopyObject.html
     * For CopyObject and UploadPartCopy, the request has to be signed with IAM credentials for directory buckets.
     * Disable S3 express signing for those types.
     */
    bool disable_s3_express_signing = request->request_tag == AWS_S3_COPY_OBJECT_REQUEST_TAG_BYPASS ||
                                      request->request_tag == AWS_S3_COPY_OBJECT_REQUEST_TAG_MULTIPART_COPY;
    aws_s3_meta_request_sign_request_default_impl(
        meta_request, request, on_signing_complete, user_data, disable_s3_express_signing);
}
