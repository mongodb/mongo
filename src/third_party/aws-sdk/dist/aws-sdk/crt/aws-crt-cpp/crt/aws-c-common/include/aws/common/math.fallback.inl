#ifndef AWS_COMMON_MATH_FALLBACK_INL
#define AWS_COMMON_MATH_FALLBACK_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * This header is already included, but include it again to make editor
 * highlighting happier.
 */
#include <aws/common/common.h>
#include <aws/common/math.h>

AWS_EXTERN_C_BEGIN
/**
 * Multiplies a * b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_mul_u64_saturating(uint64_t a, uint64_t b) {
    if (a > 0 && b > 0 && a > (UINT64_MAX / b))
        return UINT64_MAX;
    return a * b;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    if (a > 0 && b > 0 && a > (UINT64_MAX / b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a * b;
    return AWS_OP_SUCCESS;
}

/**
 * Multiplies a * b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_mul_u32_saturating(uint32_t a, uint32_t b) {
    if (a > 0 && b > 0 && a > (UINT32_MAX / b))
        return UINT32_MAX;
    return a * b;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if (a > 0 && b > 0 && a > (UINT32_MAX / b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a * b;
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b.  If the result overflows returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_add_u64_saturating(uint64_t a, uint64_t b) {
    if ((b > 0) && (a > (UINT64_MAX - b)))
        return UINT64_MAX;
    return a + b;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    if ((b > 0) && (a > (UINT64_MAX - b)))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a + b;
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b. If the result overflows returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_add_u32_saturating(uint32_t a, uint32_t b) {
    if ((b > 0) && (a > (UINT32_MAX - b)))
        return UINT32_MAX;
    return a + b;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if ((b > 0) && (a > (UINT32_MAX - b)))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a + b;
    return AWS_OP_SUCCESS;
}

/*
 * These are pure C implementations of the count leading/trailing zeros calls
 * They should not be necessary unless using a really esoteric compiler with
 * no intrinsics for these functions whatsoever.
 */
#if !defined(__clang__) && !defined(__GNUC__)
/**
 * Search from the MSB to LSB, looking for a 1
 */
AWS_STATIC_IMPL size_t aws_clz_u32(uint32_t n) {
    return aws_clz_i32((int32_t)n);
}

AWS_STATIC_IMPL size_t aws_clz_i32(int32_t n) {
    size_t idx = 0;
    if (n == 0) {
        return sizeof(n) * 8;
    }
    /* sign bit is the first bit */
    if (n < 0) {
        return 0;
    }
    while (n >= 0) {
        ++idx;
        n <<= 1;
    }
    return idx;
}

AWS_STATIC_IMPL size_t aws_clz_u64(uint64_t n) {
    return aws_clz_i64((int64_t)n);
}

AWS_STATIC_IMPL size_t aws_clz_i64(int64_t n) {
    size_t idx = 0;
    if (n == 0) {
        return sizeof(n) * 8;
    }
    /* sign bit is the first bit */
    if (n < 0) {
        return 0;
    }
    while (n >= 0) {
        ++idx;
        n <<= 1;
    }
    return idx;
}

AWS_STATIC_IMPL size_t aws_clz_size(size_t n) {
#    if SIZE_BITS == 64
    return aws_clz_u64(n);
#    else
    return aws_clz_u32(n);
#    endif
}

/**
 * Search from the LSB to MSB, looking for a 1
 */
AWS_STATIC_IMPL size_t aws_ctz_u32(uint32_t n) {
    return aws_ctz_i32((int32_t)n);
}

AWS_STATIC_IMPL size_t aws_ctz_i32(int32_t n) {
    int32_t idx = 0;
    const int32_t max_bits = (int32_t)(SIZE_BITS / sizeof(uint8_t));
    if (n == 0) {
        return sizeof(n) * 8;
    }
    while (idx < max_bits) {
        if (n & (1 << idx)) {
            break;
        }
        ++idx;
    }
    return (size_t)idx;
}

AWS_STATIC_IMPL size_t aws_ctz_u64(uint64_t n) {
    return aws_ctz_i64((int64_t)n);
}

AWS_STATIC_IMPL size_t aws_ctz_i64(int64_t n) {
    int64_t idx = 0;
    const int64_t max_bits = (int64_t)(SIZE_BITS / sizeof(uint8_t));
    if (n == 0) {
        return sizeof(n) * 8;
    }
    while (idx < max_bits) {
        if (n & (1ULL << idx)) {
            break;
        }
        ++idx;
    }
    return (size_t)idx;
}

AWS_STATIC_IMPL size_t aws_ctz_size(size_t n) {
#    if SIZE_BITS == 64
    return aws_ctz_u64(n);
#    else
    return aws_ctz_u32(n);
#    endif
}

#endif

AWS_EXTERN_C_END

#endif /*  AWS_COMMON_MATH_FALLBACK_INL */
