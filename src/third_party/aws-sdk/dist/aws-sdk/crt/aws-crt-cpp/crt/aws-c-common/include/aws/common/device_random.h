#ifndef AWS_COMMON_DEVICE_RANDOM_H
#define AWS_COMMON_DEVICE_RANDOM_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

struct aws_byte_buf;

AWS_EXTERN_C_BEGIN

/**
 * Get an unpredictably random 64bit number, suitable for cryptographic use.
 */
AWS_COMMON_API int aws_device_random_u64(uint64_t *output);

/**
 * Get an unpredictably random 32bit number, suitable for cryptographic use.
 */
AWS_COMMON_API int aws_device_random_u32(uint32_t *output);

/**
 * Get an unpredictably random 16bit number, suitable for cryptographic use.
 */
AWS_COMMON_API int aws_device_random_u16(uint16_t *output);

/**
 * Get an unpredictably random 8bit number, suitable for cryptographic use.
 */
AWS_COMMON_API int aws_device_random_u8(uint8_t *output);

/**
 * Fill the rest of a buffer with unpredictably random bytes, suitable for cryptographic use.
 */
AWS_COMMON_API int aws_device_random_buffer(struct aws_byte_buf *output);

/**
 * Write N unpredictably random bytes to a buffer, suitable for cryptographic use.
 * If there is insufficient space in the buffer, AWS_ERROR_SHORT_BUFFER is raised
 * and the buffer will be unchanged.
 */
AWS_COMMON_API int aws_device_random_buffer_append(struct aws_byte_buf *output, size_t n);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_DEVICE_RANDOM_H */
