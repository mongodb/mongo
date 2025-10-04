#ifndef AWS_COMMON_RING_BUFFER_INL
#define AWS_COMMON_RING_BUFFER_INL
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/ring_buffer.h>

AWS_EXTERN_C_BEGIN
/*
 * Checks whether atomic_ptr correctly points to a memory location within the bounds of the aws_ring_buffer
 */
AWS_STATIC_IMPL bool aws_ring_buffer_check_atomic_ptr(
    const struct aws_ring_buffer *ring_buf,
    const uint8_t *atomic_ptr) {
    return ((atomic_ptr != NULL) && (atomic_ptr >= ring_buf->allocation && atomic_ptr <= ring_buf->allocation_end));
}

/**
 * Checks whether the ring buffer is empty
 */
AWS_STATIC_IMPL bool aws_ring_buffer_is_empty(const struct aws_ring_buffer *ring_buf) {
    uint8_t *head = (uint8_t *)aws_atomic_load_ptr(&ring_buf->head);
    uint8_t *tail = (uint8_t *)aws_atomic_load_ptr(&ring_buf->tail);
    return head == tail;
}

/**
 * Evaluates the set of properties that define the shape of all valid aws_ring_buffer structures.
 * It is also a cheap check, in the sense it run in constant time (i.e., no loops or recursion).
 */
AWS_STATIC_IMPL bool aws_ring_buffer_is_valid(const struct aws_ring_buffer *ring_buf) {
    uint8_t *head = (uint8_t *)aws_atomic_load_ptr(&ring_buf->head);
    uint8_t *tail = (uint8_t *)aws_atomic_load_ptr(&ring_buf->tail);
    bool head_in_range = aws_ring_buffer_check_atomic_ptr(ring_buf, head);
    bool tail_in_range = aws_ring_buffer_check_atomic_ptr(ring_buf, tail);
    /* if head points-to the first element of the buffer then tail must too */
    bool valid_head_tail = (head != ring_buf->allocation) || (tail == ring_buf->allocation);
    return ring_buf && (ring_buf->allocation != NULL) && head_in_range && tail_in_range && valid_head_tail &&
           (ring_buf->allocator != NULL);
}
AWS_EXTERN_C_END
#endif /* AWS_COMMON_RING_BUFFER_INL */
