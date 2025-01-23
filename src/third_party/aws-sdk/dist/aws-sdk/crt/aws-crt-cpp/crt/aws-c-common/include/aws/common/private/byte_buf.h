#ifndef AWS_COMMON_PRIVATE_BYTE_BUF_H
#define AWS_COMMON_PRIVATE_BYTE_BUF_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/byte_buf.h>

/**
 * If index >= bound, bound > (SIZE_MAX / 2), or index > (SIZE_MAX / 2), returns
 * 0. Otherwise, returns UINTPTR_MAX.  This function is designed to return the correct
 * value even under CPU speculation conditions, and is intended to be used for
 * SPECTRE mitigation purposes.
 */
AWS_COMMON_API size_t aws_nospec_mask(size_t index, size_t bound);

/**
 * Expand the buffer appropriately to meet the requested capacity.
 *
 * If the the buffer's capacity is currently larger than the request capacity, the
 * function does nothing (no shrink is performed).
 */
AWS_COMMON_API
int aws_byte_buf_reserve_smart(struct aws_byte_buf *buffer, size_t requested_capacity);

/**
 * Convenience function that attempts to increase the capacity of a buffer relative to the current
 * length appropriately.
 *
 * If the the buffer's capacity is currently larger than the request capacity, the
 * function does nothing (no shrink is performed).
 */
AWS_COMMON_API
int aws_byte_buf_reserve_smart_relative(struct aws_byte_buf *buffer, size_t additional_length);

#endif /* AWS_COMMON_PRIVATE_BYTE_BUF_H */
