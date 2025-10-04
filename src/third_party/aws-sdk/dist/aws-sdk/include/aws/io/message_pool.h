#ifndef AWS_IO_MESSAGE_POOL_H
#define AWS_IO_MESSAGE_POOL_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/array_list.h>
#include <aws/io/io.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_memory_pool {
    struct aws_allocator *alloc;
    struct aws_array_list stack;
    uint16_t ideal_segment_count;
    size_t segment_size;
    void *data_ptr;
};

struct aws_message_pool {
    struct aws_allocator *alloc;
    struct aws_memory_pool application_data_pool;
    struct aws_memory_pool small_block_pool;
};

struct aws_message_pool_creation_args {
    size_t application_data_msg_data_size;
    uint8_t application_data_msg_count;
    size_t small_block_msg_data_size;
    uint8_t small_block_msg_count;
};

AWS_EXTERN_C_BEGIN

AWS_IO_API
int aws_memory_pool_init(
    struct aws_memory_pool *mempool,
    struct aws_allocator *alloc,
    uint16_t ideal_segment_count,
    size_t segment_size);

AWS_IO_API
void aws_memory_pool_clean_up(struct aws_memory_pool *mempool);

/**
 * Acquires memory from the pool if available, otherwise, it attempts to allocate and returns the result.
 */
AWS_IO_API
void *aws_memory_pool_acquire(struct aws_memory_pool *mempool);

/**
 * Releases memory to the pool if space is available, otherwise frees `to_release`
 */
AWS_IO_API
void aws_memory_pool_release(struct aws_memory_pool *mempool, void *to_release);

/**
 * Initializes message pool using 'msg_pool' as the backing pool, 'args' is copied.
 */
AWS_IO_API
int aws_message_pool_init(
    struct aws_message_pool *msg_pool,
    struct aws_allocator *alloc,
    struct aws_message_pool_creation_args *args);

AWS_IO_API
void aws_message_pool_clean_up(struct aws_message_pool *msg_pool);

/**
 * Acquires a message from the pool if available, otherwise, it attempts to allocate. If a message is acquired,
 * note that size_hint is just a hint. the return value's capacity will be set to the actual buffer size.
 */
AWS_IO_API
struct aws_io_message *aws_message_pool_acquire(
    struct aws_message_pool *msg_pool,
    enum aws_io_message_type message_type,
    size_t size_hint);

/**
 * Releases message to the pool if space is available, otherwise frees `message`
 * @param message
 */
AWS_IO_API
void aws_message_pool_release(struct aws_message_pool *msg_pool, struct aws_io_message *message);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_IO_MESSAGE_POOL_H */
