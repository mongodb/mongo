/*
 * Copyright 2010-2020 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/event-stream/event_stream.h>
#include <aws/event-stream/event_stream_channel_handler.h>

#include <aws/checksums/crc.h>

#include <aws/io/channel.h>

#include <inttypes.h>

static const size_t s_default_payload_size = 1024;

/* an event stream message has overhead of
 *  [msg len (uint32_t)]
 *  [headers len (uint32_t)]
 *  [prelude crc (uint32_t)]
 *  ... headers and payload ....
 *  [message crc (uint32_t)]
 */
static const size_t s_message_overhead_size = AWS_EVENT_STREAM_PRELUDE_LENGTH + AWS_EVENT_STREAM_TRAILER_LENGTH;

struct aws_event_stream_channel_handler {
    struct aws_channel_handler handler;
    struct aws_byte_buf message_buf;
    uint32_t running_crc;
    uint32_t current_message_len;
    aws_event_stream_channel_handler_on_message_received_fn *on_message_received;
    void *user_data;
    size_t initial_window_size;
    bool manual_window_management;
};

static int s_process_read_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {

    AWS_LOGF_TRACE(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "id=%p: received message of size %zu",
        (void *)handler,
        message->message_data.len);
    struct aws_event_stream_channel_handler *event_stream_handler = handler->impl;

    struct aws_byte_cursor message_cursor = aws_byte_cursor_from_buf(&message->message_data);

    int error_code = AWS_ERROR_SUCCESS;
    while (message_cursor.len) {
        AWS_LOGF_TRACE(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "id=%p: processing chunk of size %zu",
            (void *)handler,
            message_cursor.len);

        /* first read only the prelude so we can do checks before reading the entire buffer. */
        if (event_stream_handler->message_buf.len < AWS_EVENT_STREAM_PRELUDE_LENGTH) {
            size_t remaining_prelude = AWS_EVENT_STREAM_PRELUDE_LENGTH - event_stream_handler->message_buf.len;
            size_t to_copy = aws_min_size(message_cursor.len, remaining_prelude);
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: processing prelude, %zu bytes of an expected 12.",
                (void *)handler,
                to_copy);

            if (!aws_byte_buf_write(&event_stream_handler->message_buf, message_cursor.ptr, to_copy)) {
                error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: writing to prelude buffer failed with error %s",
                    (void *)handler,
                    aws_error_debug_str(error_code));
                goto finished;
            }

            aws_byte_cursor_advance(&message_cursor, to_copy);
        }

        /* we need to get the prelude so we can get the message length to know how much to read and also
         * to check the prelude CRC to protect against bit-flips causing us to read to much memory */
        if (event_stream_handler->message_buf.len == AWS_EVENT_STREAM_PRELUDE_LENGTH) {
            AWS_LOGF_TRACE(AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "id=%p: processing prelude buffer", (void *)handler);

            struct aws_byte_cursor prelude_cursor = aws_byte_cursor_from_buf(&event_stream_handler->message_buf);

            event_stream_handler->running_crc =
                aws_checksums_crc32(prelude_cursor.ptr, sizeof(uint32_t) + sizeof(uint32_t), 0);
            AWS_LOGF_DEBUG(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: calculated prelude CRC of %" PRIu32,
                (void *)handler,
                event_stream_handler->running_crc);

            aws_byte_cursor_read_be32(&prelude_cursor, &event_stream_handler->current_message_len);

            AWS_LOGF_DEBUG(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: read total message length of %" PRIu32,
                (void *)handler,
                event_stream_handler->current_message_len);
            if (event_stream_handler->current_message_len > AWS_EVENT_STREAM_MAX_MESSAGE_SIZE) {
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: message length of %" PRIu32 " exceeds the max size of %zu",
                    (void *)handler,
                    event_stream_handler->current_message_len,
                    (size_t)AWS_EVENT_STREAM_MAX_MESSAGE_SIZE);
                aws_raise_error(AWS_ERROR_EVENT_STREAM_MESSAGE_FIELD_SIZE_EXCEEDED);
                error_code = aws_last_error();
                goto finished;
            }

            /* advance past the headers field since we don't really care about it at this point */
            aws_byte_cursor_advance(&prelude_cursor, sizeof(uint32_t));

            uint32_t prelude_crc = 0;
            aws_byte_cursor_read_be32(&prelude_cursor, &prelude_crc);
            AWS_LOGF_DEBUG(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: read prelude CRC of %" PRIu32,
                (void *)handler,
                prelude_crc);

            /* make sure the checksum matches before processing any further */
            if (event_stream_handler->running_crc != prelude_crc) {
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: prelude CRC mismatch. calculated %" PRIu32 " but the crc for the message was %" PRIu32,
                    (void *)handler,
                    event_stream_handler->running_crc,
                    prelude_crc);
                aws_raise_error(AWS_ERROR_EVENT_STREAM_PRELUDE_CHECKSUM_FAILURE);
                error_code = aws_last_error();
                goto finished;
            }
        }

        /* read whatever is remaining from the message */
        if (event_stream_handler->message_buf.len < event_stream_handler->current_message_len) {
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "id=%p: processing remaining message buffer", (void *)handler);
            size_t remaining = event_stream_handler->current_message_len - event_stream_handler->message_buf.len;
            size_t to_copy = aws_min_size(message_cursor.len, remaining);
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: of the remaining %zu, processing %zu from the "
                "current message.",
                (void *)handler,
                remaining,
                to_copy);

            struct aws_byte_cursor to_append = aws_byte_cursor_advance(&message_cursor, to_copy);
            if (aws_byte_buf_append_dynamic(&event_stream_handler->message_buf, &to_append)) {
                error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: Appending to the message buffer failed with error %s.",
                    (void *)handler,
                    aws_error_debug_str(error_code));

                goto finished;
            }
        }

        /* If we read the entire message, parse it and give it back to the subscriber. Keep in mind, once we're to this
         * point the aws_event_stream API handles the rest of the message parsing and validation. */
        if (event_stream_handler->message_buf.len == event_stream_handler->current_message_len) {
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: An entire message has been read. Parsing the message now.",
                (void *)handler);
            struct aws_event_stream_message received_message;
            AWS_ZERO_STRUCT(received_message);

            if (aws_event_stream_message_from_buffer(
                    &received_message, event_stream_handler->handler.alloc, &event_stream_handler->message_buf)) {
                error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: Parsing the message failed with error %s.",
                    (void *)handler,
                    aws_error_debug_str(error_code));
                goto finished;
            }

            size_t message_size = event_stream_handler->message_buf.len;
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "id=%p: Invoking on_message_received callback.", (void *)handler);
            event_stream_handler->on_message_received(
                &received_message, AWS_ERROR_SUCCESS, event_stream_handler->user_data);
            aws_event_stream_message_clean_up(&received_message);
            event_stream_handler->current_message_len = 0;
            event_stream_handler->running_crc = 0;
            aws_byte_buf_reset(&event_stream_handler->message_buf, true);

            if (!event_stream_handler->manual_window_management) {
                aws_channel_slot_increment_read_window(slot, message_size);
            }
        }
    }

finished:
    if (error_code) {
        event_stream_handler->on_message_received(NULL, error_code, event_stream_handler->user_data);
        aws_channel_shutdown(slot->channel, error_code);
    }
    aws_mem_release(message->allocator, message);
    return AWS_OP_SUCCESS;
}

struct message_write_data {
    struct aws_allocator *allocator;
    struct aws_channel_task task;
    struct aws_event_stream_channel_handler *handler;
    struct aws_event_stream_message *message;
    aws_event_stream_channel_handler_on_message_written_fn *on_message_written;
    void *user_data;
};

static void s_on_message_write_completed_fn(
    struct aws_channel *channel,
    struct aws_io_message *message,
    int err_code,
    void *user_data) {
    (void)channel;
    (void)message;

    struct message_write_data *message_data = user_data;
    AWS_LOGF_TRACE(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "channel=%p: Message write completed. Invoking "
        "on_message_written callback.",
        (void *)channel);
    message_data->on_message_written(message_data->message, err_code, message_data->user_data);
    aws_mem_release(message_data->allocator, message_data);
}

static void s_write_handler_message(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;

    struct message_write_data *message_data = arg;

    AWS_LOGF_TRACE(AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "static: Write message task invoked.");
    if (status == AWS_TASK_STATUS_RUN_READY) {
        struct aws_event_stream_message *message = message_data->message;
        struct aws_event_stream_channel_handler *handler = message_data->handler;

        struct aws_byte_cursor message_cur = aws_byte_cursor_from_array(
            aws_event_stream_message_buffer(message), aws_event_stream_message_total_length(message));

        while (message_cur.len) {
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                "id=%p: writing message chunk of size %zu.",
                (void *)&handler->handler,
                message_cur.len);

            /* io messages from the pool are allowed to be smaller than the requested size. */
            struct aws_io_message *io_message = aws_channel_acquire_message_from_pool(
                handler->handler.slot->channel, AWS_IO_MESSAGE_APPLICATION_DATA, message_cur.len);

            if (!io_message) {
                int error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: Error occurred while acquiring io message %s.",
                    (void *)&handler->handler,
                    aws_error_debug_str(error_code));

                message_data->on_message_written(message, error_code, message_data->user_data);
                aws_mem_release(message_data->allocator, message_data);
                aws_channel_shutdown(handler->handler.slot->channel, error_code);
                break;
            }

            aws_byte_buf_write_to_capacity(&io_message->message_data, &message_cur);

            /* if that was the end of the buffer we want to write, attach the completion callback to that io message */
            if (message_cur.len == 0) {
                AWS_LOGF_TRACE(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: Message completely written to all io buffers.",
                    (void *)&handler->handler);
                io_message->on_completion = s_on_message_write_completed_fn;
                io_message->user_data = message_data;
            }

            /* note if this fails the io message will not be queued and as a result will not have it's completion
             * callback invoked. */
            if (aws_channel_slot_send_message(handler->handler.slot, io_message, AWS_CHANNEL_DIR_WRITE)) {
                aws_mem_release(io_message->allocator, io_message);
                int error_code = aws_last_error();
                AWS_LOGF_ERROR(
                    AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
                    "id=%p: Error occurred while sending message to channel %s.",
                    (void *)&handler->handler,
                    aws_error_debug_str(error_code));
                message_data->on_message_written(message, error_code, message_data->user_data);
                aws_mem_release(message_data->allocator, message_data);
                aws_channel_shutdown(handler->handler.slot->channel, error_code);
                break;
            }
            AWS_LOGF_TRACE(
                AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "id=%p: Message sent to channel", (void *)&handler->handler);
        }
    } else {
        AWS_LOGF_WARN(AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "static: Channel was shutdown. Message not sent");
        message_data->on_message_written(
            message_data->message, AWS_ERROR_IO_OPERATION_CANCELLED, message_data->user_data);
        aws_mem_release(message_data->allocator, message_data);
    }
}

int aws_event_stream_channel_handler_write_message(
    struct aws_channel_handler *channel_handler,
    struct aws_event_stream_message *message,
    aws_event_stream_channel_handler_on_message_written_fn *on_message_written,
    void *user_data) {
    AWS_PRECONDITION(channel_handler);
    AWS_PRECONDITION(message);
    AWS_PRECONDITION(on_message_written);

    struct aws_event_stream_channel_handler *handler = channel_handler->impl;

    struct message_write_data *write_data =
        aws_mem_calloc(handler->handler.alloc, 1, sizeof(struct message_write_data));

    if (!write_data) {
        AWS_LOGF_ERROR(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "id=%p: Error occurred while allocating callback data %s.",
            (void *)channel_handler,
            aws_error_debug_str(aws_last_error()));
        aws_channel_shutdown(channel_handler->slot->channel, aws_last_error());
        return AWS_OP_ERR;
    }

    write_data->handler = handler;
    write_data->user_data = user_data;
    write_data->message = message;
    write_data->on_message_written = on_message_written;
    write_data->allocator = handler->handler.alloc;

    AWS_LOGF_TRACE(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "id=%p: Scheduling message write task", (void *)channel_handler);
    aws_channel_task_init(
        &write_data->task, s_write_handler_message, write_data, "aws_event_stream_channel_handler_write_message");
    aws_channel_schedule_task_now_serialized(handler->handler.slot->channel, &write_data->task);

    return AWS_OP_SUCCESS;
}

void *aws_event_stream_channel_handler_get_user_data(struct aws_channel_handler *channel_handler) {
    struct aws_event_stream_channel_handler *handler = channel_handler->impl;
    return handler->user_data;
}

struct window_update_data {
    struct aws_allocator *allocator;
    struct aws_channel_task task;
    struct aws_event_stream_channel_handler *handler;
    size_t window_update_size;
};

static void s_update_window_task(struct aws_channel_task *task, void *arg, enum aws_task_status status) {
    (void)task;
    struct window_update_data *update_data = arg;

    if (status == AWS_TASK_STATUS_RUN_READY) {
        AWS_LOGF_DEBUG(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "static: updating window. increment of %zu",
            update_data->window_update_size);
        aws_channel_slot_increment_read_window(update_data->handler->handler.slot, update_data->window_update_size);
    }

    aws_mem_release(update_data->allocator, update_data);
}

void aws_event_stream_channel_handler_increment_read_window(
    struct aws_channel_handler *channel_handler,
    size_t window_update_size) {
    AWS_PRECONDITION(channel_handler);

    struct aws_event_stream_channel_handler *handler = channel_handler->impl;

    if (!handler->manual_window_management) {
        return;
    }

    AWS_LOGF_DEBUG(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "id=%p: A user requested window update and manual window management is specified. Updating size of %zu",
        (void *)channel_handler,
        window_update_size);

    if (aws_channel_thread_is_callers_thread(handler->handler.slot->channel)) {
        if (aws_channel_slot_increment_read_window(handler->handler.slot, window_update_size)) {
            aws_channel_shutdown(handler->handler.slot->channel, aws_last_error());
            return;
        }
    }

    struct window_update_data *update_data =
        aws_mem_calloc(handler->handler.alloc, 1, sizeof(struct window_update_data));

    if (!update_data) {
        AWS_LOGF_ERROR(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "id=%p: Error occurred while allocating update window data %s.",
            (void *)channel_handler,
            aws_error_debug_str(aws_last_error()));
        aws_channel_shutdown(handler->handler.slot->channel, aws_last_error());
        return;
    }

    update_data->allocator = handler->handler.alloc;
    update_data->handler = handler;
    update_data->window_update_size = window_update_size;

    aws_channel_task_init(
        &update_data->task,
        s_update_window_task,
        update_data,
        "aws_event_stream_channel_handler_increment_read_window");
    aws_channel_schedule_task_now(handler->handler.slot->channel, &update_data->task);
}

static int s_process_write_message(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    struct aws_io_message *message) {
    (void)handler;
    (void)slot;
    (void)message;
    AWS_FATAL_ASSERT(!"The event-stream-channel-handler is not designed to be a mid-channel handler.");
    return aws_raise_error(AWS_ERROR_UNIMPLEMENTED);
}

static int s_increment_read_window(struct aws_channel_handler *handler, struct aws_channel_slot *slot, size_t size) {
    (void)handler;
    return aws_channel_slot_increment_read_window(slot, size);
}

static size_t s_initial_window_size(struct aws_channel_handler *handler) {
    struct aws_event_stream_channel_handler *message_handler = handler->impl;
    return message_handler->initial_window_size;
}

static size_t s_message_overhead(struct aws_channel_handler *handler) {
    (void)handler;
    return s_message_overhead_size;
}

static void s_destroy(struct aws_channel_handler *handler) {
    AWS_LOGF_DEBUG(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "id=%p: destroying event-stream message channel handler.",
        (void *)handler);
    struct aws_event_stream_channel_handler *event_stream_handler = handler->impl;
    aws_byte_buf_clean_up(&event_stream_handler->message_buf);
    aws_mem_release(handler->alloc, event_stream_handler);
}

static int s_shutdown(
    struct aws_channel_handler *handler,
    struct aws_channel_slot *slot,
    enum aws_channel_direction dir,
    int error_code,
    bool free_scarce_resources_immediately) {
    (void)handler;
    AWS_LOGF_DEBUG(
        AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
        "id=%p: shutdown called on event-stream channel handler with error %s.",
        (void *)handler,
        aws_error_debug_str(error_code));

    return aws_channel_slot_on_handler_shutdown_complete(slot, dir, error_code, free_scarce_resources_immediately);
}

static struct aws_channel_handler_vtable vtable = {
    .destroy = s_destroy,
    .increment_read_window = s_increment_read_window,
    .initial_window_size = s_initial_window_size,
    .process_read_message = s_process_read_message,
    .process_write_message = s_process_write_message,
    .message_overhead = s_message_overhead,
    .shutdown = s_shutdown,
};

struct aws_channel_handler *aws_event_stream_channel_handler_new(
    struct aws_allocator *allocator,
    const struct aws_event_stream_channel_handler_options *handler_options) {
    AWS_PRECONDITION(allocator);
    AWS_PRECONDITION(handler_options);
    AWS_PRECONDITION(handler_options->on_message_received);

    AWS_LOGF_INFO(AWS_LS_EVENT_STREAM_CHANNEL_HANDLER, "static: creating new event-stream message channel handler.");

    struct aws_event_stream_channel_handler *event_stream_handler =
        aws_mem_calloc(allocator, 1, sizeof(struct aws_event_stream_channel_handler));

    if (!event_stream_handler) {
        AWS_LOGF_ERROR(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "static: Error occurred while allocating handler %s.",
            aws_error_debug_str(aws_last_error()));
        return NULL;
    }

    AWS_LOGF_DEBUG(AWS_LS_EVENT_STREAM_RPC_CLIENT, "static: new handler is %p", (void *)&event_stream_handler->handler);

    if (aws_byte_buf_init(
            &event_stream_handler->message_buf, allocator, s_default_payload_size + s_message_overhead_size)) {
        AWS_LOGF_ERROR(
            AWS_LS_EVENT_STREAM_CHANNEL_HANDLER,
            "id=%p: Error occurred while allocating scratch buffer %s.",
            (void *)&event_stream_handler->handler,
            aws_error_debug_str(aws_last_error()));
        aws_mem_release(allocator, event_stream_handler);
        return NULL;
    }

    event_stream_handler->on_message_received = handler_options->on_message_received;
    event_stream_handler->user_data = handler_options->user_data;
    event_stream_handler->initial_window_size =
        handler_options->initial_window_size > 0 ? handler_options->initial_window_size : SIZE_MAX;
    event_stream_handler->manual_window_management = handler_options->manual_window_management;
    event_stream_handler->handler.vtable = &vtable;
    event_stream_handler->handler.alloc = allocator;
    event_stream_handler->handler.impl = event_stream_handler;

    return &event_stream_handler->handler;
}
