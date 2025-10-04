#include "aws/s3/private/s3_default_meta_request.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_meta_request_impl.h"
#include "aws/s3/private/s3_request_messages.h"
#include "aws/s3/private/s3_util.h"
#include <aws/common/string.h>
#include <inttypes.h>

/* Data for aws_s3_meta_request_default's vtable->prepare_request() job */
struct aws_s3_default_prepare_request_job {
    struct aws_allocator *allocator;
    struct aws_s3_request *request;
    /* async step: read request body */
    struct aws_future_bool *step1_read_body;
    /* future to set when this whole job completes */
    struct aws_future_void *on_complete;
};

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request);

static bool s_s3_meta_request_default_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

static struct aws_future_void *s_s3_default_prepare_request(struct aws_s3_request *request);

static void s_s3_default_prepare_request_on_read_done(void *user_data);

static void s_s3_default_prepare_request_finish(
    struct aws_s3_default_prepare_request_job *request_prep,
    int error_code);

static void s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

static struct aws_s3_meta_request_vtable s_s3_meta_request_default_vtable = {
    .update = s_s3_meta_request_default_update,
    .send_request_finish = aws_s3_meta_request_send_request_finish_default,
    .prepare_request = s_s3_default_prepare_request,
    .init_signing_date_time = aws_s3_meta_request_init_signing_date_time_default,
    .sign_request = aws_s3_meta_request_sign_request_default,
    .finished_request = s_s3_meta_request_default_request_finished,
    .destroy = s_s3_meta_request_default_destroy,
    .finish = aws_s3_meta_request_finish_default,
};

/* Allocate a new default meta request. */
struct aws_s3_meta_request *aws_s3_meta_request_default_new(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    enum aws_s3_request_type request_type,
    uint64_t content_length,
    bool should_compute_content_md5,
    const struct aws_s3_meta_request_options *options) {

    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(client);
    AWS_PRECONDITION(options);
    AWS_PRECONDITION(options->message);
    AWS_PRECONDITION(request_type != AWS_S3_REQUEST_TYPE_UNKNOWN || options->operation_name.len != 0);

    struct aws_byte_cursor request_method;
    if (aws_http_message_get_request_method(options->message, &request_method)) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; could not get request method from message.");

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    if (content_length > SIZE_MAX) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "Could not create Default Meta Request; content length of %" PRIu64 " bytes is too large for platform.",
            content_length);

        aws_raise_error(AWS_ERROR_INVALID_ARGUMENT);
        return NULL;
    }

    struct aws_s3_meta_request_default *meta_request_default =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_s3_meta_request_default));

    /* Try to initialize the base type. */
    if (aws_s3_meta_request_init_base(
            allocator,
            client,
            0,
            should_compute_content_md5,
            options,
            meta_request_default,
            &s_s3_meta_request_default_vtable,
            &meta_request_default->base)) {

        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p Could not initialize base type for Default Meta Request.",
            (void *)meta_request_default);

        aws_mem_release(allocator, meta_request_default);
        return NULL;
    }

    meta_request_default->content_length = (size_t)content_length;

    /* If request_type is unknown, look it up from operation name */
    if (request_type != AWS_S3_REQUEST_TYPE_UNKNOWN) {
        meta_request_default->request_type = request_type;
    } else {
        meta_request_default->request_type = aws_s3_request_type_from_operation_name(options->operation_name);
    }

    /* If we have a static string for this operation name, use that.
     * Otherwise, copy the operation_name passed in by user. */
    struct aws_string *static_operation_name = aws_s3_request_type_to_operation_name_static_string(request_type);
    if (static_operation_name != NULL) {
        meta_request_default->operation_name = static_operation_name;
    } else {
        meta_request_default->operation_name = aws_string_new_from_cursor(allocator, &options->operation_name);
    }

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST,
        "id=%p Created new Default Meta Request. operation=%s",
        (void *)meta_request_default,
        aws_string_c_str(meta_request_default->operation_name));

    return &meta_request_default->base;
}

static void s_s3_meta_request_default_destroy(struct aws_s3_meta_request *meta_request) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    aws_string_destroy(meta_request_default->operation_name);
    aws_mem_release(meta_request->allocator, meta_request_default);
}

/* Try to get the next request that should be processed. */
static bool s_s3_meta_request_default_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request) {
    (void)flags;

    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(out_request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    struct aws_s3_request *request = NULL;
    bool work_remaining = false;

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);

        if (!aws_s3_meta_request_has_finish_result_synced(meta_request)) {

            /* If the request hasn't been sent, then create and send it now. */
            if (!meta_request_default->synced_data.request_sent) {
                if (out_request == NULL) {
                    goto has_work_remaining;
                }

                request = aws_s3_request_new(
                    meta_request,
                    0 /*request_tag*/,
                    meta_request_default->request_type,
                    1 /*part_number*/,
                    AWS_S3_REQUEST_FLAG_RECORD_RESPONSE_HEADERS);

                /* If request_type didn't map to a name, copy over the name passed in by user */
                if (request->operation_name == NULL) {
                    request->operation_name =
                        aws_string_new_from_string(meta_request->allocator, meta_request_default->operation_name);
                }

                AWS_LOGF_DEBUG(
                    AWS_LS_S3_META_REQUEST,
                    "id=%p: Meta Request Default created request %p",
                    (void *)meta_request,
                    (void *)request);

                meta_request_default->synced_data.request_sent = true;
                goto has_work_remaining;
            }

            /* If the request hasn't been completed, then wait for it to be completed. */
            if (!meta_request_default->synced_data.request_completed) {
                goto has_work_remaining;
            }

            /* If delivery hasn't been attempted yet for the response body, wait for that to happen. */
            if (meta_request->synced_data.num_parts_delivery_completed < 1) {
                goto has_work_remaining;
            }

            goto no_work_remaining;

        } else {

            /* If we are canceling, and the request hasn't been sent yet, then there is nothing to wait for. */
            if (!meta_request_default->synced_data.request_sent) {
                goto no_work_remaining;
            }

            /* If the request hasn't been completed yet, then wait for that to happen. */
            if (!meta_request_default->synced_data.request_completed) {
                goto has_work_remaining;
            }

            /* If some parts are still being delivered to the caller, then wait for those to finish. */
            if (meta_request->synced_data.num_parts_delivery_completed <
                meta_request->synced_data.num_parts_delivery_sent) {
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
            aws_s3_meta_request_set_success_synced(
                meta_request, meta_request_default->synced_data.cached_response_status);
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */

    if (work_remaining) {
        if (request != NULL) {
            AWS_ASSERT(out_request != NULL);
            *out_request = request;
        }
    } else {
        AWS_ASSERT(request == NULL);

        aws_s3_meta_request_finish(meta_request);
    }

    return work_remaining;
}

/* Given a request, prepare it for sending based on its description. */
static struct aws_future_void *s_s3_default_prepare_request(struct aws_s3_request *request) {
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request *meta_request = request->meta_request;
    AWS_PRECONDITION(meta_request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    AWS_PRECONDITION(meta_request_default);

    struct aws_future_void *asyncstep_prepare_request = aws_future_void_new(request->allocator);

    /* Store data for async job */
    struct aws_s3_default_prepare_request_job *request_prep =
        aws_mem_calloc(request->allocator, 1, sizeof(struct aws_s3_default_prepare_request_job));
    request_prep->allocator = request->allocator;
    request_prep->request = request;
    request_prep->on_complete = aws_future_void_acquire(asyncstep_prepare_request);

    if (meta_request_default->content_length > 0 && request->num_times_prepared == 0) {
        aws_byte_buf_init(&request->request_body, meta_request->allocator, meta_request_default->content_length);

        /* Kick off the async read */
        request_prep->step1_read_body =
            aws_s3_meta_request_read_body(meta_request, 0 /*offset*/, &request->request_body);
        aws_future_bool_register_callback(
            request_prep->step1_read_body, s_s3_default_prepare_request_on_read_done, request_prep);
    } else {
        /* Don't need to read body, jump directly to the last step */
        s_s3_default_prepare_request_finish(request_prep, AWS_ERROR_SUCCESS);
    }

    return asyncstep_prepare_request;
}

/* Completion callback for reading the body stream */
static void s_s3_default_prepare_request_on_read_done(void *user_data) {

    struct aws_s3_default_prepare_request_job *request_prep = user_data;
    struct aws_s3_request *request = request_prep->request;
    struct aws_s3_meta_request *meta_request = request->meta_request;

    int error_code = aws_future_bool_get_error(request_prep->step1_read_body);

    if (error_code != AWS_OP_SUCCESS) {
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Failed reading request body, error %d (%s)",
            (void *)meta_request,
            error_code,
            aws_error_str(error_code));
        goto finish;
    }

    if (request->request_body.len < request->request_body.capacity) {
        error_code = AWS_ERROR_S3_INCORRECT_CONTENT_LENGTH;
        AWS_LOGF_ERROR(
            AWS_LS_S3_META_REQUEST,
            "id=%p: Request body is smaller than 'Content-Length' header said it would be",
            (void *)meta_request);
        goto finish;
    }

finish:
    s_s3_default_prepare_request_finish(request_prep, error_code);
}

/* Finish async preparation of the request */
static void s_s3_default_prepare_request_finish(
    struct aws_s3_default_prepare_request_job *request_prep,
    int error_code) {

    struct aws_s3_request *request = request_prep->request;
    struct aws_s3_meta_request *meta_request = request->meta_request;

    if (error_code != AWS_ERROR_SUCCESS) {
        goto finish;
    }

    struct aws_http_message *message = aws_s3_message_util_copy_http_message_no_body_all_headers(
        meta_request->allocator, meta_request->initial_request_message);

    bool flexible_checksum = meta_request->checksum_config.location != AWS_SCL_NONE;
    if (!flexible_checksum && meta_request->should_compute_content_md5) {
        /* If flexible checksum used, client MUST skip Content-MD5 header computation */
        aws_s3_message_util_add_content_md5_header(meta_request->allocator, &request->request_body, message);
    }

    if (meta_request->checksum_config.validate_response_checksum) {
        struct aws_http_headers *headers = aws_http_message_get_headers(message);
        aws_http_headers_set(headers, g_request_validation_mode, g_enabled);
    }
    aws_s3_message_util_assign_body(
        meta_request->allocator,
        &request->request_body,
        message,
        &meta_request->checksum_config,
        NULL /* out_checksum */);

    aws_s3_request_setup_send_data(request, message);

    aws_http_message_release(message);

    AWS_LOGF_DEBUG(
        AWS_LS_S3_META_REQUEST, "id=%p: Meta Request prepared request %p", (void *)meta_request, (void *)request);

finish:
    if (error_code == AWS_ERROR_SUCCESS) {
        aws_future_void_set_result(request_prep->on_complete);
    } else {
        aws_future_void_set_error(request_prep->on_complete, error_code);
    }

    aws_future_bool_release(request_prep->step1_read_body);
    aws_future_void_release(request_prep->on_complete);
    aws_mem_release(request_prep->allocator, request_prep);
}

static void s_s3_meta_request_default_request_finished(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code) {
    AWS_PRECONDITION(meta_request);
    AWS_PRECONDITION(meta_request->impl);
    AWS_PRECONDITION(request);

    struct aws_s3_meta_request_default *meta_request_default = meta_request->impl;
    AWS_PRECONDITION(meta_request_default);

    if (error_code == AWS_ERROR_SUCCESS && request->send_data.response_headers != NULL) {
        if (meta_request->checksum_config.validate_response_checksum) {
            if (aws_s3_check_headers_for_checksum(
                    meta_request,
                    request->send_data.response_headers,
                    &meta_request->meta_request_level_running_response_sum,
                    &meta_request->meta_request_level_response_header_checksum,
                    true) != AWS_OP_SUCCESS) {
                error_code = aws_last_error_or_unknown();
            }
        }

        if (error_code == AWS_ERROR_SUCCESS && meta_request->headers_callback != NULL) {
            if (meta_request->headers_callback(
                    meta_request,
                    request->send_data.response_headers,
                    request->send_data.response_status,
                    meta_request->user_data)) {
                error_code = aws_last_error_or_unknown();
            }

            meta_request->headers_callback = NULL;
        }
    }

    /* BEGIN CRITICAL SECTION */
    {
        aws_s3_meta_request_lock_synced_data(meta_request);
        meta_request_default->synced_data.cached_response_status = request->send_data.response_status;
        meta_request_default->synced_data.request_completed = true;
        meta_request_default->synced_data.request_error_code = error_code;
        bool finishing_metrics = true;

        if (error_code == AWS_ERROR_SUCCESS) {
            /* Send progress_callback for delivery on io_event_loop thread.
             * For default meta-requests, we invoke the progress_callback once, after the sole HTTP request completes.
             * This is simpler than reporting incremental progress as the response body is received,
             * or the request body is streamed out, since then we'd also need to handle retries that reset
             * progress back to 0% (our existing API only lets us report forward progress). */
            if (meta_request->progress_callback != NULL) {
                struct aws_s3_meta_request_event event = {.type = AWS_S3_META_REQUEST_EVENT_PROGRESS};
                if (meta_request->type == AWS_S3_META_REQUEST_TYPE_PUT_OBJECT) {
                    /* For uploads, report request body size */
                    event.u.progress.info.bytes_transferred = request->request_body.len;
                    event.u.progress.info.content_length = request->request_body.len;
                } else {
                    /* For anything else, report response body size */
                    event.u.progress.info.bytes_transferred = request->send_data.response_body.len;
                    event.u.progress.info.content_length = request->send_data.response_body.len;
                }
                aws_s3_meta_request_add_event_for_delivery_synced(meta_request, &event);
            }

            aws_s3_meta_request_stream_response_body_synced(meta_request, request);
            /* The body of the request is queued to be streamed, don't record the end timestamp for the request
             * yet. */
            finishing_metrics = false;
        } else {
            aws_s3_meta_request_set_fail_synced(meta_request, request, error_code);
        }

        if (finishing_metrics) {
            aws_s3_request_finish_up_metrics_synced(request, meta_request);
        }

        aws_s3_meta_request_unlock_synced_data(meta_request);
    }
    /* END CRITICAL SECTION */
}
