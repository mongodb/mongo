#ifndef AWS_COMMON_ZERO_INL
#define AWS_COMMON_ZERO_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/stdbool.h>
#include <aws/common/stdint.h>
#include <aws/common/zero.h>
#include <string.h>

AWS_EXTERN_C_BEGIN
/**
 * Returns whether each byte is zero.
 */
AWS_STATIC_IMPL
bool aws_is_mem_zeroed(const void *buf, size_t bufsize) {
    /* Optimization idea: vectorized instructions to check more than 64 bits at a time. */

    /* Check 64 bits at a time */
    const uint64_t *buf_u64 = (const uint64_t *)buf;
    const size_t num_u64_checks = bufsize / 8;
    size_t i;
    for (i = 0; i < num_u64_checks; ++i) {
        if (buf_u64[i]) {
            return false;
        }
    }

    /* Update buf to where u64 checks left off */
    buf = buf_u64 + num_u64_checks;
    bufsize = bufsize % 8;

    /* Check 8 bits at a time */
    const uint8_t *buf_u8 = (const uint8_t *)buf;
    for (i = 0; i < bufsize; ++i) {
        if (buf_u8[i]) {
            return false;
        }
    }

    return true;
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_ZERO_INL */
