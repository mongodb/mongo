#ifndef AWS_S3_META_REQUEST_IMPL_H
#define AWS_S3_META_REQUEST_IMPL_H

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/auth/signing.h>
#include <aws/common/atomics.h>
#include <aws/common/linked_list.h>
#include <aws/common/mutex.h>
#include <aws/common/ref_count.h>
#include <aws/common/task_scheduler.h>
#include <aws/http/request_response.h>

#include "aws/s3/private/s3_checksums.h"
#include "aws/s3/private/s3_client_impl.h"
#include "aws/s3/private/s3_request.h"

struct aws_s3_client;
struct aws_s3_connection;
struct aws_s3_meta_request;
struct aws_s3_request;
struct aws_http_headers;
struct aws_http_make_request_options;
struct aws_retry_strategy;

enum aws_s3_meta_request_state {
    AWS_S3_META_REQUEST_STATE_ACTIVE,
    AWS_S3_META_REQUEST_STATE_FINISHED,
};

enum aws_s3_meta_request_update_flags {
    /* The client potentially has multiple meta requests that it can spread across connections, and the given meta
       request can selectively not return a request if there is a performance reason to do so.*/
    AWS_S3_META_REQUEST_UPDATE_FLAG_CONSERVATIVE = 0x00000002,
};

typedef void(aws_s3_meta_request_prepare_request_callback_fn)(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code,
    void *user_data);

struct aws_s3_prepare_request_payload {
    struct aws_allocator *allocator;
    struct aws_s3_request *request;
    struct aws_task task;
    /* async step: wait for vtable->prepare_request() call to complete */
    struct aws_future_void *asyncstep_prepare_request;
    /* callback to invoke when all request preparation work is complete */
    aws_s3_meta_request_prepare_request_callback_fn *callback;
    void *user_data;
};

/* An event to be delivered on the meta-request's io_event_loop thread. */
struct aws_s3_meta_request_event {
    enum aws_s3_meta_request_event_type {
        AWS_S3_META_REQUEST_EVENT_RESPONSE_BODY, /* body_callback */
        AWS_S3_META_REQUEST_EVENT_PROGRESS,      /* progress_callback */
        AWS_S3_META_REQUEST_EVENT_TELEMETRY,     /* telemetry_callback */
    } type;

    union {
        /* data for AWS_S3_META_REQUEST_EVENT_RESPONSE_BODY */
        struct {
            struct aws_s3_request *completed_request;
        } response_body;

        /* data for AWS_S3_META_REQUEST_EVENT_PROGRESS */
        struct {
            struct aws_s3_meta_request_progress info;
        } progress;

        /* data for AWS_S3_META_REQUEST_EVENT_TELEMETRY */
        struct {
            struct aws_s3_request_metrics *metrics;
        } telemetry;
    } u;
};

struct aws_s3_meta_request_vtable {
    /* Update the meta request.  out_request is required to be non-null. Returns true if there is any work in
     * progress, false if there is not. */
    bool (*update)(struct aws_s3_meta_request *meta_request, uint32_t flags, struct aws_s3_request **out_request);

    /* Run vtable->prepare_request() on the meta-request's event loop.
     * We do this because body streaming is slow, and we don't want it on our networking threads.
     * The callback may fire on any thread (an async sub-step may run on another thread). */
    void (*schedule_prepare_request)(
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        aws_s3_meta_request_prepare_request_callback_fn *callback,
        void *user_data);

    /* Given a request, asynchronously prepare it for sending
     * (creating the correct HTTP message, reading from a stream (if necessary), computing hashes, etc.).
     * Returns a future, which may complete on any thread (and may complete synchronously). */
    struct aws_future_void *(*prepare_request)(struct aws_s3_request *request);

    void (*init_signing_date_time)(struct aws_s3_meta_request *meta_request, struct aws_date_time *date_time);

    /* Sign the given request. */
    void (*sign_request)(
        struct aws_s3_meta_request *meta_request,
        struct aws_s3_request *request,
        aws_signing_complete_fn *on_signing_complete,
        void *user_data);

    /* Called when any sending of the request is finished, including for each retry. */
    void (*send_request_finish)(struct aws_s3_connection *connection, struct aws_http_stream *stream, int error_code);

    /* Called when the request is done being sent, and will not be retried/sent again. */
    void (*finished_request)(struct aws_s3_meta_request *meta_request, struct aws_s3_request *request, int error_code);

    /* Called by the derived meta request when the meta request is completely finished. */
    void (*finish)(struct aws_s3_meta_request *meta_request);

    /* Handle de-allocation of the meta request. */
    void (*destroy)(struct aws_s3_meta_request *);

    /* Pause the given request */
    int (*pause)(struct aws_s3_meta_request *meta_request, struct aws_s3_meta_request_resume_token **resume_token);
};

/**
 * This represents one meta request, ie, one accelerated file transfer.  One S3 meta request can represent multiple S3
 * requests.
 */
struct aws_s3_meta_request {
    struct aws_allocator *allocator;

    struct aws_ref_count ref_count;

    void *impl;

    struct aws_s3_meta_request_vtable *vtable;

    /* Initial HTTP Message that this meta request is based on. */
    struct aws_http_message *initial_request_message;

    /* The meta request's outgoing body comes from one of these:
     * 1) request_body_async_stream: if set, then async stream 1 part at a time
     * 2) request_body_parallel_stream: if set, then stream multiple parts in parallel
     * 3) request_body_using_async_writes: if set, then synchronously copy async_write data from 1 part at a time
     * 4) initial_request_message's body_stream: else synchronously stream parts */
    struct aws_async_input_stream *request_body_async_stream;
    struct aws_parallel_input_stream *request_body_parallel_stream;
    bool request_body_using_async_writes;

    /* Part size to use for uploads and downloads.  Passed down by the creating client. */
    const size_t part_size;

    struct aws_cached_signing_config_aws *cached_signing_config;

    /* Client that created this meta request which also processes this request. After the meta request is finished, this
     * reference is removed.*/
    struct aws_s3_client *client;

    struct aws_s3_endpoint *endpoint;

    /* Event loop to schedule IO work related on, ie, reading from streams, streaming parts back to the caller, etc...
     * After the meta request is finished, this will be reset along with the client reference.*/
    struct aws_event_loop *io_event_loop;

    /* User data to be passed to each customer specified callback.*/
    void *user_data;

    /* Customer specified callbacks. */
    aws_s3_meta_request_headers_callback_fn *headers_callback;
    aws_s3_meta_request_receive_body_callback_fn *body_callback;
    aws_s3_meta_request_finish_fn *finish_callback;
    aws_s3_meta_request_shutdown_fn *shutdown_callback;
    aws_s3_meta_request_progress_fn *progress_callback;
    aws_s3_meta_request_telemetry_fn *telemetry_callback;
    aws_s3_meta_request_upload_review_fn *upload_review_callback;

    enum aws_s3_meta_request_type type;
    struct aws_string *s3express_session_host;

    struct {
        struct aws_mutex lock;

        /* Priority queue for pending streaming requests.  We use a priority queue to keep parts in order so that we
         * can stream them to the caller in order. */
        struct aws_priority_queue pending_body_streaming_requests;

        /* Current state of the meta request. */
        enum aws_s3_meta_request_state state;

        /* The sum of initial_read_window, plus all window_increment() calls. This number never goes down. */
        uint64_t read_window_running_total;

        /* The next expected streaming part number needed to continue streaming part bodies. (For example, this will
         * initially be 1 for part 1, and after that part is received, it will be 2, then 3, etc.. )*/
        uint32_t next_streaming_part;

        /* Number of parts scheduled for delivery. */
        uint32_t num_parts_delivery_sent;

        /* Total number of parts that have been attempted to be delivered. (Will equal the sum of succeeded and
         * failed.)*/
        uint32_t num_parts_delivery_completed;

        /* Task for delivering events on the meta-request's io_event_loop thread.
         * We do this to ensure a meta-request's callbacks are fired sequentially and non-overlapping.
         * If `event_delivery_array` has items in it, then this task is scheduled.
         * If `event_delivery_active` is true, then this task is actively running.
         * Delivery is not 100% complete until `event_delivery_array` is empty AND `event_delivery_active` is false
         * (use aws_s3_meta_request_are_events_out_for_delivery_synced()  to check) */
        struct aws_task event_delivery_task;

        /* Array of `struct aws_s3_meta_request_event` to deliver when the `event_delivery_task` runs. */
        struct aws_array_list event_delivery_array;

        /* When true, events are actively being delivered to the user. */
        bool event_delivery_active;

        /* The end finish result of the meta request. */
        struct aws_s3_meta_request_result finish_result;

        /* True if the finish result has been set. */
        uint32_t finish_result_set : 1;

        /* To track aws_s3_requests with cancellable HTTP streams */
        struct aws_linked_list cancellable_http_streams_list;

        /* Data for async-writes. */
        struct {
            /* Whether a part request can be sent (we have 1 part's worth of data, or EOF) */
            bool ready_to_send;

            /* True once user passes `eof` to their final write() call */
            bool eof;

            /* Holds buffered data we can't immediately send.
             * The length will always be less than part-size */
            struct aws_byte_buf buffered_data;
            struct aws_s3_buffer_pool_ticket *buffered_data_ticket;

            /* Waker callback.
             * Stored if a poll_write() call returns result.is_pending
             * because we already had 1 part's worth of data.
             * Invoked when we're ready to accept another poll_write() call. */
            aws_simple_completion_callback *waker;
            void *waker_user_data;
        } async_write;

    } synced_data;

    /* Anything in this structure should only ever be accessed by the client on its process work event loop task. */
    struct {

        /* Linked list node for the meta requests linked list in the client. */
        /* Note: this needs to be first for using AWS_CONTAINER_OF with the nested structure. */
        struct aws_linked_list_node node;

        /* True if this meta request is currently in the client's list. */
        bool scheduled;

    } client_process_work_threaded_data;

    /* Anything in this structure should only ever be accessed by the meta-request from its io_event_loop thread. */
    struct {
        /* When delivering events, we swap contents with `synced_data.event_delivery_array`.
         * This is an optimization, we could have just copied the array when the task runs,
         * but swapping two array-lists back and forth avoids an allocation. */
        struct aws_array_list event_delivery_array;
    } io_threaded_data;

    const bool should_compute_content_md5;

    /* deep copy of the checksum config. */
    struct checksum_config_storage checksum_config;

    /* checksum found in either a default get request, or in the initial head request of a multipart get */
    struct aws_byte_buf meta_request_level_response_header_checksum;

    /* running checksum of all the parts of a default get, or ranged get meta request*/
    struct aws_s3_checksum *meta_request_level_running_response_sum;

    /* The receiving file handler */
    FILE *recv_file;
    struct aws_string *recv_filepath;
    bool recv_file_delete_on_failure;
};

/* Info for each part, that we need to remember until we send CompleteMultipartUpload */
struct aws_s3_mpu_part_info {
    uint64_t size;
    struct aws_string *etag;
    struct aws_byte_buf checksum_base64;
    bool was_previously_uploaded;
};

AWS_EXTERN_C_BEGIN

/* Initialize the base meta request structure. */
AWS_S3_API
int aws_s3_meta_request_init_base(
    struct aws_allocator *allocator,
    struct aws_s3_client *client,
    size_t part_size,
    bool should_compute_content_md5,
    const struct aws_s3_meta_request_options *options,
    void *impl,
    struct aws_s3_meta_request_vtable *vtable,
    struct aws_s3_meta_request *base_type);

/* Returns true if the meta request is still in the "active" state. */
AWS_S3_API
bool aws_s3_meta_request_is_active(struct aws_s3_meta_request *meta_request);

/* Returns true if the meta request is in the "finished" state. */
AWS_S3_API
bool aws_s3_meta_request_is_finished(struct aws_s3_meta_request *meta_request);

/* Returns true if the meta request has a finish result, which indicates that the meta request has trying to finish or
 * has already finished. */
AWS_S3_API
bool aws_s3_meta_request_has_finish_result(struct aws_s3_meta_request *meta_request);

AWS_S3_API
void aws_s3_meta_request_lock_synced_data(struct aws_s3_meta_request *meta_request);

AWS_S3_API
void aws_s3_meta_request_unlock_synced_data(struct aws_s3_meta_request *meta_request);

/* Called by the client to retrieve the next request and update the meta request's internal state. out_request is
 * optional, and can be NULL if just desiring to update internal state. */
AWS_S3_API
bool aws_s3_meta_request_update(
    struct aws_s3_meta_request *meta_request,
    uint32_t flags,
    struct aws_s3_request **out_request);

AWS_S3_API
void aws_s3_meta_request_prepare_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    aws_s3_meta_request_prepare_request_callback_fn *callback,
    void *user_data);

AWS_S3_API
void aws_s3_meta_request_send_request(struct aws_s3_meta_request *meta_request, struct aws_s3_connection *connection);

AWS_S3_API
void aws_s3_meta_request_init_signing_date_time_default(
    struct aws_s3_meta_request *meta_request,
    struct aws_date_time *date_time);

AWS_S3_API
void aws_s3_meta_request_sign_request_default_impl(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    aws_signing_complete_fn *on_signing_complete,
    void *user_data,
    bool disable_s3_express_signing);

AWS_S3_API
void aws_s3_meta_request_sign_request_default(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    aws_signing_complete_fn *on_signing_complete,
    void *user_data);

/* Default implementation for when a request finishes a particular send. */
AWS_S3_API
void aws_s3_meta_request_send_request_finish_default(
    struct aws_s3_connection *connection,
    struct aws_http_stream *stream,
    int error_code);

/* Called by the client when a request is completely finished and not doing any further retries. */
AWS_S3_API
void aws_s3_meta_request_finished_request(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request,
    int error_code);

/* Called to place the request in the meta request's priority queue for streaming back to the caller.  Once all requests
 * with a part number less than the given request has been received, the given request and the previous requests will
 * be scheduled for streaming.  */
AWS_S3_API
void aws_s3_meta_request_stream_response_body_synced(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *request);

/* Add an event for delivery on the meta-request's io_event_loop thread.
 * These events usually correspond to callbacks that must fire sequentially and non-overlapping,
 * such as delivery of a part's response body. */
void aws_s3_meta_request_add_event_for_delivery_synced(
    struct aws_s3_meta_request *meta_request,
    const struct aws_s3_meta_request_event *event);

/* Returns whether any events are out for delivery.
 * The meta-request's finish callback must not be invoked until this returns false. */
bool aws_s3_meta_request_are_events_out_for_delivery_synced(struct aws_s3_meta_request *meta_request);

/* Cancel the requests with cancellable HTTP stream for the meta request */
void aws_s3_meta_request_cancel_cancellable_requests_synced(struct aws_s3_meta_request *meta_request, int error_code);

/* Asynchronously read from the meta request's input stream. Should always be done outside of any mutex,
 * as reading from the stream could cause user code to call back into aws-c-s3.
 * This will fill the buffer to capacity, unless end of stream is reached.
 * It may read from the underlying stream multiple times, if that's what it takes to fill the buffer.
 * Returns a future whose result bool indicates whether end of stream was reached.
 * This future may complete on any thread, and may complete synchronously.
 *
 * Read from offset to fill the buffer
 */
AWS_S3_API
struct aws_future_bool *aws_s3_meta_request_read_body(
    struct aws_s3_meta_request *meta_request,
    uint64_t offset,
    struct aws_byte_buf *buffer);

/* Set the meta request finish result as failed. This is meant to be called sometime before aws_s3_meta_request_finish.
 * Subsequent calls to this function or to aws_s3_meta_request_set_success_synced will not overwrite the end result of
 * the meta request. */
AWS_S3_API
void aws_s3_meta_request_set_fail_synced(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_request *failed_request,
    int error_code);

/* Set the meta request finish result as successful. This is meant to be called sometime before
 * aws_s3_meta_request_finish. Subsequent calls this function or to aws_s3_meta_request_set_fail_synced will not
 * overwrite the end result of the meta request. */
AWS_S3_API
void aws_s3_meta_request_set_success_synced(struct aws_s3_meta_request *meta_request, int response_status);

/* Returns true if the finish result has been set (ie: either aws_s3_meta_request_set_fail_synced or
 * aws_s3_meta_request_set_success_synced have been called.) */
AWS_S3_API
bool aws_s3_meta_request_has_finish_result_synced(struct aws_s3_meta_request *meta_request);

/* Virtual function called by the meta request derived type when it's completely finished and there is no other work to
 * be done. */
AWS_S3_API
void aws_s3_meta_request_finish(struct aws_s3_meta_request *meta_request);

/* Default implementation of the meta request finish function. */
AWS_S3_API
void aws_s3_meta_request_finish_default(struct aws_s3_meta_request *meta_request);

/* Sets up a meta request result structure. */
AWS_S3_API
void aws_s3_meta_request_result_setup(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_meta_request_result *result,
    struct aws_s3_request *failed_request,
    int response_status,
    int error_code);

/* Cleans up a meta request result structure. */
AWS_S3_API
void aws_s3_meta_request_result_clean_up(
    struct aws_s3_meta_request *meta_request,
    struct aws_s3_meta_request_result *result);

AWS_S3_API
bool aws_s3_meta_request_checksum_config_has_algorithm(
    struct aws_s3_meta_request *meta_request,
    enum aws_s3_checksum_algorithm algorithm);

AWS_EXTERN_C_END

#endif /* AWS_S3_META_REQUEST_IMPL_H */
