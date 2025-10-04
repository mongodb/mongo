/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/io/message_pool.h>

#include <aws/common/thread.h>

int aws_memory_pool_init(
    struct aws_memory_pool *mempool,
    struct aws_allocator *alloc,
    uint16_t ideal_segment_count,
    size_t segment_size) {

    mempool->alloc = alloc;
    mempool->ideal_segment_count = ideal_segment_count;
    mempool->segment_size = segment_size;
    mempool->data_ptr = aws_mem_calloc(alloc, ideal_segment_count, sizeof(void *));
    if (!mempool->data_ptr) {
        return AWS_OP_ERR;
    }

    aws_array_list_init_static(&mempool->stack, mempool->data_ptr, ideal_segment_count, sizeof(void *));

    for (uint16_t i = 0; i < ideal_segment_count; ++i) {
        void *memory = aws_mem_acquire(alloc, segment_size);
        if (memory) {
            aws_array_list_push_back(&mempool->stack, &memory);
        } else {
            goto clean_up;
        }
    }

    return AWS_OP_SUCCESS;

clean_up:
    aws_memory_pool_clean_up(mempool);
    return AWS_OP_ERR;
}

void aws_memory_pool_clean_up(struct aws_memory_pool *mempool) {
    void *cur = NULL;

    while (aws_array_list_length(&mempool->stack) > 0) {
        /* the only way this fails is not possible since I already checked the length. */
        aws_array_list_back(&mempool->stack, &cur);
        aws_array_list_pop_back(&mempool->stack);
        aws_mem_release(mempool->alloc, cur);
    }

    aws_array_list_clean_up(&mempool->stack);
    aws_mem_release(mempool->alloc, mempool->data_ptr);
}

void *aws_memory_pool_acquire(struct aws_memory_pool *mempool) {
    void *back = NULL;
    if (aws_array_list_length(&mempool->stack) > 0) {
        aws_array_list_back(&mempool->stack, &back);
        aws_array_list_pop_back(&mempool->stack);

        return back;
    }

    void *mem = aws_mem_acquire(mempool->alloc, mempool->segment_size);
    return mem;
}

void aws_memory_pool_release(struct aws_memory_pool *mempool, void *to_release) {
    size_t pool_size = aws_array_list_length(&mempool->stack);

    if (pool_size >= mempool->ideal_segment_count) {
        aws_mem_release(mempool->alloc, to_release);
        return;
    }

    aws_array_list_push_back(&mempool->stack, &to_release);
}

struct message_pool_allocator {
    struct aws_allocator base_allocator;
    struct aws_message_pool *msg_pool;
};

void *s_message_pool_mem_acquire(struct aws_allocator *allocator, size_t size) {
    (void)allocator;
    (void)size;

    /* no one should ever call this ever. */
    AWS_ASSERT(0);
    return NULL;
}

void s_message_pool_mem_release(struct aws_allocator *allocator, void *ptr) {
    struct message_pool_allocator *msg_pool_alloc = allocator->impl;

    aws_message_pool_release(msg_pool_alloc->msg_pool, (struct aws_io_message *)ptr);
}

static size_t MSG_OVERHEAD = sizeof(struct aws_io_message) + sizeof(struct message_pool_allocator);

int aws_message_pool_init(
    struct aws_message_pool *msg_pool,
    struct aws_allocator *alloc,
    struct aws_message_pool_creation_args *args) {

    msg_pool->alloc = alloc;

    size_t msg_data_size = args->application_data_msg_data_size + MSG_OVERHEAD;

    if (aws_memory_pool_init(
            &msg_pool->application_data_pool, alloc, args->application_data_msg_count, msg_data_size)) {
        return AWS_OP_ERR;
    }

    size_t small_blk_data_size = args->small_block_msg_data_size + MSG_OVERHEAD;

    if (aws_memory_pool_init(&msg_pool->small_block_pool, alloc, args->small_block_msg_count, small_blk_data_size)) {
        aws_memory_pool_clean_up(&msg_pool->application_data_pool);
        return AWS_OP_ERR;
    }

    return AWS_OP_SUCCESS;
}

void aws_message_pool_clean_up(struct aws_message_pool *msg_pool) {
    aws_memory_pool_clean_up(&msg_pool->application_data_pool);
    aws_memory_pool_clean_up(&msg_pool->small_block_pool);
    AWS_ZERO_STRUCT(*msg_pool);
}

struct message_wrapper {
    struct aws_io_message message;
    struct message_pool_allocator msg_allocator;
    uint8_t buffer_start[1];
};

struct aws_io_message *aws_message_pool_acquire(
    struct aws_message_pool *msg_pool,
    enum aws_io_message_type message_type,
    size_t size_hint) {

    struct message_wrapper *message_wrapper = NULL;
    size_t max_size = 0;
    switch (message_type) {
        case AWS_IO_MESSAGE_APPLICATION_DATA:
            if (size_hint > msg_pool->small_block_pool.segment_size - MSG_OVERHEAD) {
                message_wrapper = aws_memory_pool_acquire(&msg_pool->application_data_pool);
                max_size = msg_pool->application_data_pool.segment_size - MSG_OVERHEAD;
            } else {
                message_wrapper = aws_memory_pool_acquire(&msg_pool->small_block_pool);
                max_size = msg_pool->small_block_pool.segment_size - MSG_OVERHEAD;
            }
            break;
        default:
            AWS_ASSERT(0);
            break;
    }

    AWS_FATAL_ASSERT(message_wrapper);

    message_wrapper->message.message_type = message_type;
    message_wrapper->message.message_tag = 0;
    message_wrapper->message.user_data = NULL;
    message_wrapper->message.copy_mark = 0;
    message_wrapper->message.on_completion = NULL;
    /* the buffer shares the allocation with the message. It's the bit at the end. */
    message_wrapper->message.message_data.buffer = message_wrapper->buffer_start;
    message_wrapper->message.message_data.len = 0;
    message_wrapper->message.message_data.capacity = size_hint <= max_size ? size_hint : max_size;

    /* set the allocator ptr */
    message_wrapper->msg_allocator.base_allocator.impl = &message_wrapper->msg_allocator;
    message_wrapper->msg_allocator.base_allocator.mem_acquire = s_message_pool_mem_acquire;
    message_wrapper->msg_allocator.base_allocator.mem_realloc = NULL;
    message_wrapper->msg_allocator.base_allocator.mem_release = s_message_pool_mem_release;
    message_wrapper->msg_allocator.msg_pool = msg_pool;

    message_wrapper->message.allocator = &message_wrapper->msg_allocator.base_allocator;
    return &message_wrapper->message;
}

void aws_message_pool_release(struct aws_message_pool *msg_pool, struct aws_io_message *message) {

    memset(message->message_data.buffer, 0, message->message_data.len);
    message->allocator = NULL;

    struct message_wrapper *wrapper = AWS_CONTAINER_OF(message, struct message_wrapper, message);

    switch (message->message_type) {
        case AWS_IO_MESSAGE_APPLICATION_DATA:
            if (message->message_data.capacity > msg_pool->small_block_pool.segment_size - MSG_OVERHEAD) {
                aws_memory_pool_release(&msg_pool->application_data_pool, wrapper);
            } else {
                aws_memory_pool_release(&msg_pool->small_block_pool, wrapper);
            }
            break;
        default:
            AWS_ASSERT(0);
            aws_raise_error(AWS_IO_CHANNEL_UNKNOWN_MESSAGE_TYPE);
    }
}
