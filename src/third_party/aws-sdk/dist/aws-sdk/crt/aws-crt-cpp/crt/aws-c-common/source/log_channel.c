/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/log_channel.h>

#include <aws/common/condition_variable.h>
#include <aws/common/log_writer.h>
#include <aws/common/mutex.h>
#include <aws/common/string.h>
#include <aws/common/thread.h>

#include <stdio.h>

/*
 * Basic channel implementations - synchronized foreground, synchronized background
 */

struct aws_log_foreground_channel {
    struct aws_mutex sync;
};

static int s_foreground_channel_send(struct aws_log_channel *channel, struct aws_string *log_line) {

    struct aws_log_foreground_channel *impl = (struct aws_log_foreground_channel *)channel->impl;

    AWS_ASSERT(channel->writer->vtable->write);

    aws_mutex_lock(&impl->sync);
    (channel->writer->vtable->write)(channel->writer, log_line);
    aws_mutex_unlock(&impl->sync);

    /*
     * send is considered a transfer of ownership.  write is not a transfer of ownership.
     * So it's always the channel's responsibility to clean up all log lines that enter
     * it as soon as they are no longer needed.
     */
    aws_string_destroy(log_line);

    return AWS_OP_SUCCESS;
}

static void s_foreground_channel_clean_up(struct aws_log_channel *channel) {
    struct aws_log_foreground_channel *impl = (struct aws_log_foreground_channel *)channel->impl;

    aws_mutex_clean_up(&impl->sync);

    aws_mem_release(channel->allocator, impl);
}

static struct aws_log_channel_vtable s_foreground_channel_vtable = {
    .send = s_foreground_channel_send,
    .clean_up = s_foreground_channel_clean_up,
};

int aws_log_channel_init_foreground(
    struct aws_log_channel *channel,
    struct aws_allocator *allocator,
    struct aws_log_writer *writer) {
    struct aws_log_foreground_channel *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_log_foreground_channel));
    if (impl == NULL) {
        return AWS_OP_ERR;
    }

    if (aws_mutex_init(&impl->sync)) {
        aws_mem_release(allocator, impl);
        return AWS_OP_ERR;
    }

    channel->vtable = &s_foreground_channel_vtable;
    channel->allocator = allocator;
    channel->writer = writer;
    channel->impl = impl;

    return AWS_OP_SUCCESS;
}

struct aws_log_background_channel {
    struct aws_mutex sync;
    struct aws_thread background_thread;
    struct aws_array_list pending_log_lines;
    struct aws_condition_variable pending_line_signal;
    bool finished;
};

static int s_background_channel_send(struct aws_log_channel *channel, struct aws_string *log_line) {

    struct aws_log_background_channel *impl = (struct aws_log_background_channel *)channel->impl;

    aws_mutex_lock(&impl->sync);
    aws_array_list_push_back(&impl->pending_log_lines, &log_line);
    aws_condition_variable_notify_one(&impl->pending_line_signal);
    aws_mutex_unlock(&impl->sync);

    return AWS_OP_SUCCESS;
}

static void s_background_channel_clean_up(struct aws_log_channel *channel) {
    struct aws_log_background_channel *impl = (struct aws_log_background_channel *)channel->impl;

    aws_mutex_lock(&impl->sync);
    impl->finished = true;
    aws_condition_variable_notify_one(&impl->pending_line_signal);
    aws_mutex_unlock(&impl->sync);

    aws_thread_join(&impl->background_thread);

    aws_thread_clean_up(&impl->background_thread);
    aws_condition_variable_clean_up(&impl->pending_line_signal);
    aws_array_list_clean_up(&impl->pending_log_lines);
    aws_mutex_clean_up(&impl->sync);
    aws_mem_release(channel->allocator, impl);
}

static struct aws_log_channel_vtable s_background_channel_vtable = {
    .send = s_background_channel_send,
    .clean_up = s_background_channel_clean_up,
};

static bool s_background_wait(void *context) {
    struct aws_log_background_channel *impl = (struct aws_log_background_channel *)context;

    /*
     * Condition variable predicates are checked under mutex protection
     */
    return impl->finished || aws_array_list_length(&impl->pending_log_lines) > 0;
}

/**
 * This is where the background thread spends 99.999% of its time.
 * We broke this out into its own function so that the stacktrace clearly shows
 * what this thread is doing. We've had a lot of cases where users think this
 * thread is deadlocked because it's stuck here. We want it to be clear
 * that it's doing nothing on purpose. It's waiting for log messages...
 */
AWS_NO_INLINE
static void aws_background_logger_listen_for_messages(struct aws_log_background_channel *impl) {
    aws_condition_variable_wait_pred(&impl->pending_line_signal, &impl->sync, s_background_wait, impl);
}

static void aws_background_logger_thread(void *thread_data) {
    (void)thread_data;

    struct aws_log_channel *channel = (struct aws_log_channel *)thread_data;
    AWS_ASSERT(channel->writer->vtable->write);

    struct aws_log_background_channel *impl = (struct aws_log_background_channel *)channel->impl;

    struct aws_array_list log_lines;

    AWS_FATAL_ASSERT(aws_array_list_init_dynamic(&log_lines, channel->allocator, 10, sizeof(struct aws_string *)) == 0);

    while (true) {
        aws_mutex_lock(&impl->sync);

        aws_background_logger_listen_for_messages(impl);

        size_t line_count = aws_array_list_length(&impl->pending_log_lines);
        bool finished = impl->finished;

        if (line_count == 0) {
            aws_mutex_unlock(&impl->sync);
            if (finished) {
                break;
            }
            continue;
        }

        aws_array_list_swap_contents(&impl->pending_log_lines, &log_lines);
        aws_mutex_unlock(&impl->sync);

        /*
         * Consider copying these into a page-sized stack buffer (string) and then making the write calls
         * against it rather than the individual strings.  Might be a savings when > 1 lines (cut down on
         * write calls).
         */
        for (size_t i = 0; i < line_count; ++i) {
            struct aws_string *log_line = NULL;
            AWS_FATAL_ASSERT(aws_array_list_get_at(&log_lines, &log_line, i) == AWS_OP_SUCCESS);

            (channel->writer->vtable->write)(channel->writer, log_line);

            /*
             * send is considered a transfer of ownership.  write is not a transfer of ownership.
             * So it's always the channel's responsibility to clean up all log lines that enter
             * it as soon as they are no longer needed.
             */
            aws_string_destroy(log_line);
        }

        aws_array_list_clear(&log_lines);
    }

    aws_array_list_clean_up(&log_lines);
}

int aws_log_channel_init_background(
    struct aws_log_channel *channel,
    struct aws_allocator *allocator,
    struct aws_log_writer *writer) {
    struct aws_log_background_channel *impl = aws_mem_calloc(allocator, 1, sizeof(struct aws_log_background_channel));
    if (impl == NULL) {
        return AWS_OP_ERR;
    }

    impl->finished = false;

    if (aws_mutex_init(&impl->sync)) {
        goto clean_up_sync_init_fail;
    }

    if (aws_array_list_init_dynamic(&impl->pending_log_lines, allocator, 10, sizeof(struct aws_string *))) {
        goto clean_up_pending_log_lines_init_fail;
    }

    if (aws_condition_variable_init(&impl->pending_line_signal)) {
        goto clean_up_pending_line_signal_init_fail;
    }

    if (aws_thread_init(&impl->background_thread, allocator)) {
        goto clean_up_background_thread_init_fail;
    }

    channel->vtable = &s_background_channel_vtable;
    channel->allocator = allocator;
    channel->impl = impl;
    channel->writer = writer;

    /*
     * Logging thread should need very little stack, but let's defer this to later
     */
    struct aws_thread_options thread_options = *aws_default_thread_options();
    thread_options.name = aws_byte_cursor_from_c_str("AwsLogger"); /* 15 characters is max for Linux */

    if (aws_thread_launch(&impl->background_thread, aws_background_logger_thread, channel, &thread_options) ==
        AWS_OP_SUCCESS) {
        return AWS_OP_SUCCESS;
    }

    aws_thread_clean_up(&impl->background_thread);

clean_up_background_thread_init_fail:
    aws_condition_variable_clean_up(&impl->pending_line_signal);

clean_up_pending_line_signal_init_fail:
    aws_array_list_clean_up(&impl->pending_log_lines);

clean_up_pending_log_lines_init_fail:
    aws_mutex_clean_up(&impl->sync);

clean_up_sync_init_fail:
    aws_mem_release(allocator, impl);

    return AWS_OP_ERR;
}

void aws_log_channel_clean_up(struct aws_log_channel *channel) {
    AWS_ASSERT(channel->vtable->clean_up);
    (channel->vtable->clean_up)(channel);
}
