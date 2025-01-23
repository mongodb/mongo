#ifndef AWS_HTTP_H1_STREAM_H
#define AWS_HTTP_H1_STREAM_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/http/private/h1_encoder.h>
#include <aws/http/private/http_impl.h>
#include <aws/http/private/request_response_impl.h>
#include <aws/io/channel.h>

#ifdef _MSC_VER
#    pragma warning(disable : 4214) /* nonstandard extension used: bit field types other than int */
#endif

/* Simple view of stream's state.
 * Used to determine whether it's safe for a user to call functions that alter state. */
enum aws_h1_stream_api_state {
    AWS_H1_STREAM_API_STATE_INIT,
    AWS_H1_STREAM_API_STATE_ACTIVE,
    AWS_H1_STREAM_API_STATE_COMPLETE,
};

struct aws_h1_stream {
    struct aws_http_stream base;

    struct aws_linked_list_node node;

    /* Task that removes items from `synced_data` and does their on-thread work.
     * Runs once and wait until it's scheduled again.
     * Any function that wants to schedule this task MUST:
     * - acquire the synced_data.lock
     * - check whether `synced_data.is_cross_thread_work_scheduled` was true or false.
     * - set `synced_data.is_cross_thread_work_scheduled = true`
     * - release synced_data.lock
     * - ONLY IF `synced_data.is_cross_thread_work_scheduled` CHANGED from false to true:
     *   - increment the stream's refcount, to keep stream alive until task runs
     *   - schedule the task
     */
    struct aws_channel_task cross_thread_work_task;

    /* Message (derived from outgoing request or response) to be submitted to encoder */
    struct aws_h1_encoder_message encoder_message;

    bool is_outgoing_message_done;

    bool is_incoming_message_done;
    bool is_incoming_head_done;

    /* If true, this is the last stream the connection should process.
     * See RFC-7230 Section 6: Connection Management. */
    bool is_final_stream;

    /* Buffer for incoming data that needs to stick around. */
    struct aws_byte_buf incoming_storage_buf;

    struct {
        /* TODO: move most other members in here */

        /* List of `struct aws_h1_chunk`, used for chunked encoding.
         * Encoder completes/frees/pops front chunk when it's done sending. */
        struct aws_linked_list pending_chunk_list;

        struct aws_h1_encoder_message message;

        /* Size of stream's flow-control window.
         * Only body data (not headers, etc) counts against the stream's flow-control window. */
        uint64_t stream_window;

        /* Whether a "request handler" stream has a response to send.
         * Has mirror variable in synced_data */
        bool has_outgoing_response : 1;
    } thread_data;

    /* Any thread may touch this data, but the connection's lock must be held.
     * Sharing a lock is fine because it's rare for an HTTP/1 connection
     * to have more than one stream at a time. */
    struct {
        /* List of `struct aws_h1_chunk` which have been submitted by user,
         * but haven't yet moved to encoder_message.pending_chunk_list where the encoder will find them. */
        struct aws_linked_list pending_chunk_list;

        /* trailing headers which have been submitted by user,
         * but haven't yet moved to encoder_message where the encoder will find them. */
        struct aws_h1_trailer *pending_trailer;

        enum aws_h1_stream_api_state api_state;

        /* Sum of all aws_http_stream_update_window() calls that haven't yet moved to thread_data.stream_window */
        uint64_t pending_window_update;

        /* See `cross_thread_work_task` */
        bool is_cross_thread_work_task_scheduled : 1;

        /* Whether a "request handler" stream has a response to send.
         * Has mirror variable in thread_data */
        bool has_outgoing_response : 1;

        /* Whether the outgoing message is using chunked encoding */
        bool using_chunked_encoding : 1;

        /* Whether the final 0 length chunk has already been sent */
        bool has_final_chunk : 1;

        /* Whether the chunked trailer has already been sent */
        bool has_added_trailer : 1;
    } synced_data;
};

/* DO NOT export functions below. They're only used by other .c files in this library */

struct aws_h1_stream *aws_h1_stream_new_request(
    struct aws_http_connection *client_connection,
    const struct aws_http_make_request_options *options);

struct aws_h1_stream *aws_h1_stream_new_request_handler(const struct aws_http_request_handler_options *options);

int aws_h1_stream_activate(struct aws_http_stream *stream);
void aws_h1_stream_cancel(struct aws_http_stream *stream, int error_code);

int aws_h1_stream_send_response(struct aws_h1_stream *stream, struct aws_http_message *response);

#endif /* AWS_HTTP_H1_STREAM_H */
