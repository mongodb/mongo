/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/private/tls_channel_handler_shared.h>

#include <aws/common/clock.h>
#include <aws/io/tls_channel_handler.h>

static void s_tls_timeout_task_fn(struct aws_channel_task *channel_task, void *arg, enum aws_task_status status) {
    (void)channel_task;

    if (status != AWS_TASK_STATUS_RUN_READY) {
        return;
    }

    struct aws_tls_channel_handler_shared *tls_handler_shared = arg;
    if (tls_handler_shared->stats.handshake_status != AWS_TLS_NEGOTIATION_STATUS_ONGOING) {
        return;
    }

    struct aws_channel *channel = tls_handler_shared->handler->slot->channel;
    aws_channel_shutdown(channel, AWS_IO_TLS_NEGOTIATION_TIMEOUT);
}

void aws_tls_channel_handler_shared_init(
    struct aws_tls_channel_handler_shared *tls_handler_shared,
    struct aws_channel_handler *handler,
    struct aws_tls_connection_options *options) {
    tls_handler_shared->handler = handler;
    tls_handler_shared->tls_timeout_ms = options->timeout_ms;
    aws_crt_statistics_tls_init(&tls_handler_shared->stats);
    aws_channel_task_init(&tls_handler_shared->timeout_task, s_tls_timeout_task_fn, tls_handler_shared, "tls_timeout");
}

void aws_tls_channel_handler_shared_clean_up(struct aws_tls_channel_handler_shared *tls_handler_shared) {
    (void)tls_handler_shared;
}

void aws_on_drive_tls_negotiation(struct aws_tls_channel_handler_shared *tls_handler_shared) {
    if (tls_handler_shared->stats.handshake_status == AWS_TLS_NEGOTIATION_STATUS_NONE) {
        tls_handler_shared->stats.handshake_status = AWS_TLS_NEGOTIATION_STATUS_ONGOING;

        uint64_t now = 0;
        aws_channel_current_clock_time(tls_handler_shared->handler->slot->channel, &now);
        tls_handler_shared->stats.handshake_start_ns = now;

        if (tls_handler_shared->tls_timeout_ms > 0) {
            uint64_t timeout_ns =
                now + aws_timestamp_convert(
                          tls_handler_shared->tls_timeout_ms, AWS_TIMESTAMP_MILLIS, AWS_TIMESTAMP_NANOS, NULL);

            aws_channel_schedule_task_future(
                tls_handler_shared->handler->slot->channel, &tls_handler_shared->timeout_task, timeout_ns);
        }
    }
}

void aws_on_tls_negotiation_completed(struct aws_tls_channel_handler_shared *tls_handler_shared, int error_code) {
    tls_handler_shared->stats.handshake_status =
        (error_code == AWS_ERROR_SUCCESS) ? AWS_TLS_NEGOTIATION_STATUS_SUCCESS : AWS_TLS_NEGOTIATION_STATUS_FAILURE;
    aws_channel_current_clock_time(
        tls_handler_shared->handler->slot->channel, &tls_handler_shared->stats.handshake_end_ns);
}

bool aws_tls_options_buf_is_set(const struct aws_byte_buf *buf) {
    return buf->allocator != NULL;
}
