/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */
#include <aws/checksums/crc.h>
#include <aws/checksums/private/crc_priv.h>
#include <aws/checksums/private/crc_util.h>

#include <aws/common/cpuid.h>

large_buffer_apply_impl(crc32, uint32_t)

    static uint32_t (*s_crc32c_fn_ptr)(const uint8_t *input, int length, uint32_t previous_crc32c) = 0;
static uint32_t (*s_crc32_fn_ptr)(const uint8_t *input, int length, uint32_t previous_crc32) = 0;

uint32_t aws_checksums_crc32(const uint8_t *input, int length, uint32_t previous_crc32) {
    if (AWS_UNLIKELY(!s_crc32_fn_ptr)) {
#if defined(AWS_USE_CPU_EXTENSIONS) && defined(AWS_ARCH_ARM64)
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_ARM_CRC)) {
            s_crc32_fn_ptr = aws_checksums_crc32_armv8;
        } else {
            s_crc32_fn_ptr = aws_checksums_crc32_sw;
        }
#else
        s_crc32_fn_ptr = aws_checksums_crc32_sw;
#endif
    }
    return s_crc32_fn_ptr(input, length, previous_crc32);
}

uint32_t aws_checksums_crc32_ex(const uint8_t *input, size_t length, uint32_t previous_crc32) {
    return aws_large_buffer_apply_crc32(aws_checksums_crc32, input, length, previous_crc32);
}

uint32_t aws_checksums_crc32c(const uint8_t *input, int length, uint32_t previous_crc32c) {
    if (AWS_UNLIKELY(!s_crc32c_fn_ptr)) {
#if defined(AWS_USE_CPU_EXTENSIONS) && defined(AWS_ARCH_INTEL_X64)
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_SSE_4_2)) {
            s_crc32c_fn_ptr = aws_checksums_crc32c_intel_avx512_with_sse_fallback;
        } else {
            s_crc32c_fn_ptr = aws_checksums_crc32c_sw;
        }
#elif defined(AWS_USE_CPU_EXTENSIONS) && defined(AWS_ARCH_ARM64)
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_ARM_CRC)) {
            s_crc32c_fn_ptr = aws_checksums_crc32c_armv8;
        } else {
            s_crc32c_fn_ptr = aws_checksums_crc32c_sw;
        }
#else
        s_crc32c_fn_ptr = aws_checksums_crc32c_sw;
#endif
    }

    return s_crc32c_fn_ptr(input, length, previous_crc32c);
}

uint32_t aws_checksums_crc32c_ex(const uint8_t *input, size_t length, uint32_t previous_crc32) {
    return aws_large_buffer_apply_crc32(aws_checksums_crc32c, input, length, previous_crc32);
}
