/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/channel.h>
#include <aws/io/tls_channel_handler.h>

struct alpn_handler {
    aws_tls_on_protocol_negotiated on_protocol_negotiated;
    void *user_data;
};

static int s_alpn_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    if (message->message_tag != AWS_TLS_NEGOTIATED_PROTOCOL_MESSAGE) {
        return aws_raise_error(AWS_IO_MISSING_ALPN_MESSAGE);
    }

    struct aws_tls_negotiated_protocol_message *protocol_message =
        (struct aws_tls_negotiated_protocol_message *)message->message_data.buffer;

    struct aws_channel_slot *new_slot = aws_channel_slot_new(slot->channel);

    struct alpn_handler *alpn_handler = (struct alpn_handler *)handler->impl;

    if (!new_slot) {
        return AWS_OP_ERR;
    }

    struct aws_channel_handler *new_handler =
        alpn_handler->on_protocol_negotiated(new_slot, &protocol_message->protocol, alpn_handler->user_data);

    if (!new_handler) {
        aws_mem_release(handler->alloc, (void *)new_slot);
        return aws_raise_error(AWS_IO_UNHANDLED_ALPN_PROTOCOL_MESSAGE);
    }

    aws_channel_slot_replace(slot, new_slot);
    aws_channel_slot_set_handler(new_slot, new_handler);
    return AWS_OP_SUCCESS;
}

static int s_alpn_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool abort_immediately) {
    (void)handler;
    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, abort_immediately);
}

static size_t s_alpn_get_initial_window_size(struct aws_channel_handler *handler) {
    (void)handler;
    return sizeof(struct aws_tls_negotiated_protocol_message);
}

static void s_alpn_destroy(struct aws_channel_handler *handler) {
    struct alpn_handler *alpn_handler = (struct alpn_handler *)handler->impl;
    aws_mem_release(handler->alloc, alpn_handler);
    aws_mem_release(handler->alloc, handler);
}

static size_t s_alpn_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return 0;
}

static struct aws_channel_handler_vtable s_alpn_handler_vtable = {
    .initial_window_size = s_alpn_get_initial_window_size,
    .increment_read_window = NULL,
    .shutdown = s_alpn_shutdown,
    .process_write_message = NULL,
    .process_read_message = s_alpn_process_read_message,
    .destroy = s_alpn_destroy,
    .message_overhead = s_alpn_message_overhead,
};

struct aws_channel_handler *aws_tls_alpn_handler_new(
    struct aws_allocator *allocator,
    aws_tls_on_protocol_negotiated on_protocol_negotiated,
    void *user_data) {
    struct aws_channel_handler *channel_handler =
        (struct aws_channel_handler *)aws_mem_calloc(allocator, 1, sizeof(struct aws_channel_handler));

    if (!channel_handler) {
        return NULL;
    }

    struct alpn_handler *alpn_handler =
        (struct alpn_handler *)aws_mem_calloc(allocator, 1, sizeof(struct alpn_handler));

    if (!alpn_handler) {
        aws_mem_release(allocator, (void *)channel_handler);
        return NULL;
    }

    alpn_handler->on_protocol_negotiated = on_protocol_negotiated;
    alpn_handler->user_data = user_data;
    channel_handler->impl = alpn_handler;
    channel_handler->alloc = allocator;

    channel_handler->vtable = &s_alpn_handler_vtable;

    return channel_handler;
}
