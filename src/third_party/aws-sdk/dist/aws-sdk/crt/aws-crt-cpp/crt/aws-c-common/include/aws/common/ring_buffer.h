#ifndef AWS_COMMON_RING_BUFFER_H
#define AWS_COMMON_RING_BUFFER_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/atomics.h>

AWS_PUSH_SANE_WARNING_LEVEL

/**
 * Lockless ring buffer implementation that is thread safe assuming a single thread acquires and a single thread
 * releases. For any other use case (other than the single-threaded use-case), you must manage thread-safety manually.
 *
 * Also, a very important note: release must happen in the same order as acquire. If you do not your application, and
 * possibly computers within a thousand mile radius, may die terrible deaths, and the local drinking water will be
 * poisoned for generations with fragments of what is left of your radioactive corrupted memory.
 */
struct aws_ring_buffer {
    struct aws_allocator *allocator;
    uint8_t *allocation;
    struct aws_atomic_var head;
    struct aws_atomic_var tail;
    uint8_t *allocation_end;
};

struct aws_byte_buf;

AWS_EXTERN_C_BEGIN

/**
 * Initializes a ring buffer with an allocation of size `size`. Returns AWS_OP_SUCCESS on a successful initialization,
 * AWS_OP_ERR otherwise.
 */
AWS_COMMON_API int aws_ring_buffer_init(struct aws_ring_buffer *ring_buf, struct aws_allocator *allocator, size_t size);

/*
 * Checks whether atomic_ptr correctly points to a memory location within the bounds of the aws_ring_buffer
 */
AWS_STATIC_IMPL bool aws_ring_buffer_check_atomic_ptr(
    const struct aws_ring_buffer *ring_buf,
    const uint8_t *atomic_ptr);
/**
 * Checks whether the ring buffer is empty
 */
AWS_STATIC_IMPL bool aws_ring_buffer_is_empty(const struct aws_ring_buffer *ring_buf);

/**
 * Evaluates the set of properties that define the shape of all valid aws_ring_buffer structures.
 * It is also a cheap check, in the sense it run in constant time (i.e., no loops or recursion).
 */
AWS_STATIC_IMPL bool aws_ring_buffer_is_valid(const struct aws_ring_buffer *ring_buf);

/**
 * Cleans up the ring buffer's resources.
 */
AWS_COMMON_API void aws_ring_buffer_clean_up(struct aws_ring_buffer *ring_buf);

/**
 * Attempts to acquire `requested_size` buffer and stores the result in `dest` if successful. Returns AWS_OP_SUCCESS if
 * the requested size was available for use, AWS_OP_ERR otherwise.
 */
AWS_COMMON_API int aws_ring_buffer_acquire(
    struct aws_ring_buffer *ring_buf,
    size_t requested_size,
    struct aws_byte_buf *dest);

/**
 * Attempts to acquire `requested_size` buffer and stores the result in `dest` if successful. If not available, it will
 * attempt to acquire anywhere from 1 byte to `requested_size`. Returns AWS_OP_SUCCESS if some buffer space is available
 * for use, AWS_OP_ERR otherwise.
 */
AWS_COMMON_API int aws_ring_buffer_acquire_up_to(
    struct aws_ring_buffer *ring_buf,
    size_t minimum_size,
    size_t requested_size,
    struct aws_byte_buf *dest);

/**
 * Releases `buf` back to the ring buffer for further use. RELEASE MUST HAPPEN in the SAME ORDER AS ACQUIRE.
 * If you do not, your application, and possibly computers within a thousand mile radius, may die terrible deaths,
 * and the local drinking water will be poisoned for generations
 * with fragments of what is left of your radioactive corrupted memory.
 */
AWS_COMMON_API void aws_ring_buffer_release(struct aws_ring_buffer *ring_buffer, struct aws_byte_buf *buf);

/**
 * Returns true if the memory in `buf` was vended by this ring buffer, false otherwise.
 * Make sure `buf->buffer` and `ring_buffer->allocation` refer to the same memory region.
 */
AWS_COMMON_API bool aws_ring_buffer_buf_belongs_to_pool(
    const struct aws_ring_buffer *ring_buffer,
    const struct aws_byte_buf *buf);

AWS_EXTERN_C_END

#ifndef AWS_NO_STATIC_IMPL
#    include <aws/common/ring_buffer.inl>
#endif /* AWS_NO_STATIC_IMPL */

AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_RING_BUFFER_H */
