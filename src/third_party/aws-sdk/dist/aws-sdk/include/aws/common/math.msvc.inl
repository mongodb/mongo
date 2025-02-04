#ifndef AWS_COMMON_MATH_MSVC_INL
#define AWS_COMMON_MATH_MSVC_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * This header is already included, but include it again to make editor
 * highlighting happier.
 */
#include <aws/common/common.h>
#include <aws/common/cpuid.h>
#include <aws/common/math.h>

/* This file generates level 4 compiler warnings in Visual Studio 2017 and older */
#pragma warning(push, 3)
#include <intrin.h>
#pragma warning(pop)

AWS_EXTERN_C_BEGIN
/**
 * Multiplies a * b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_mul_u64_saturating(uint64_t a, uint64_t b) {
    uint64_t out;
    uint64_t ret_val = _umul128(a, b, &out);
    return (out == 0) ? ret_val : UINT64_MAX;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    uint64_t out;
    *r = _umul128(a, b, &out);

    if (out != 0) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

static uint32_t (*s_mul_u32_saturating_fn_ptr)(uint32_t a, uint32_t b) = NULL;

static uint32_t s_mulx_u32_saturating(uint32_t a, uint32_t b) {
    uint32_t high_32;
    uint32_t ret_val = _mulx_u32(a, b, &high_32);
    return (high_32 == 0) ? ret_val : UINT32_MAX;
}

static uint32_t s_emulu_saturating(uint32_t a, uint32_t b) {
    uint64_t result = __emulu(a, b);
    return (result > UINT32_MAX) ? UINT32_MAX : (uint32_t)result;
}
/**
 * Multiplies a * b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_mul_u32_saturating(uint32_t a, uint32_t b) {
    if (AWS_UNLIKELY(!s_mul_u32_saturating_fn_ptr)) {
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_BMI2)) {
            s_mul_u32_saturating_fn_ptr = s_mulx_u32_saturating;
        } else {
            /* If BMI2 unavailable, use __emulu instead */
            s_mul_u32_saturating_fn_ptr = s_emulu_saturating;
        }
    }
    return s_mul_u32_saturating_fn_ptr(a, b);
}

static int (*s_mul_u32_checked_fn_ptr)(uint32_t a, uint32_t b, uint32_t *r) = NULL;

static int s_mulx_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    uint32_t high_32;
    *r = _mulx_u32(a, b, &high_32);

    if (high_32 != 0) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

static int s_emulu_checked(uint32_t a, uint32_t b, uint32_t *r) {
    uint64_t result = __emulu(a, b);
    if (result > UINT32_MAX) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    *r = (uint32_t)result;
    return AWS_OP_SUCCESS;
}
/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if (AWS_UNLIKELY(!s_mul_u32_checked_fn_ptr)) {
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_BMI2)) {
            s_mul_u32_checked_fn_ptr = s_mulx_u32_checked;
        } else {
            /* If BMI2 unavailable, use __emulu instead */
            s_mul_u32_checked_fn_ptr = s_emulu_checked;
        }
    }
    return s_mul_u32_checked_fn_ptr(a, b, r);
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
#if !defined(_MSC_VER) || _MSC_VER < 1920
    /* Fallback MSVC 2017 and older, _addcarry doesn't work correctly for those compiler */
    if ((b > 0) && (a > (UINT64_MAX - b))) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    *r = a + b;
    return AWS_OP_SUCCESS;
#else
    if (_addcarry_u64((uint8_t)0, a, b, r)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
#endif
}

/**
 * Adds a + b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_add_u64_saturating(uint64_t a, uint64_t b) {
#if !defined(_MSC_VER) || _MSC_VER < 1920
    /* Fallback MSVC 2017 and older, _addcarry doesn't work correctly for those compiler */
    if ((b > 0) && (a > (UINT64_MAX - b))) {
        return UINT64_MAX;
    }
    return a + b;
#else
    uint64_t res = 0;
    if (_addcarry_u64((uint8_t)0, a, b, &res)) {
        res = UINT64_MAX;
    }
    return res;
#endif
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
#if !defined(_MSC_VER) || _MSC_VER < 1920
    /* Fallback MSVC 2017 and older, _addcarry doesn't work correctly for those compiler */
    if ((b > 0) && (a > (UINT32_MAX - b))) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    *r = a + b;
    return AWS_OP_SUCCESS;
#else
    if (_addcarry_u32((uint8_t)0, a, b, r)) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
#endif
}

/**
 * Adds a + b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_add_u32_saturating(uint32_t a, uint32_t b) {
#if !defined(_MSC_VER) || _MSC_VER < 1920
    /* Fallback MSVC 2017 and older, _addcarry doesn't work correctly for those compiler */
    if ((b > 0) && (a > (UINT32_MAX - b)))
        return UINT32_MAX;
    return a + b;
#else
    uint32_t res = 0;
    if (_addcarry_u32((uint8_t)0, a, b, &res)) {
        res = UINT32_MAX;
    }
    return res;
#endif
}

/**
 * Search from the MSB to LSB, looking for a 1
 */
AWS_STATIC_IMPL size_t aws_clz_u32(uint32_t n) {
    unsigned long idx = 0;
    if (_BitScanReverse(&idx, n)) {
        return 31 - idx;
    }
    return 32;
}

AWS_STATIC_IMPL size_t aws_clz_i32(int32_t n) {
    unsigned long idx = 0;
    if (_BitScanReverse(&idx, (unsigned long)n)) {
        return 31 - idx;
    }
    return 32;
}

AWS_STATIC_IMPL size_t aws_clz_u64(uint64_t n) {
    unsigned long idx = 0;
    if (_BitScanReverse64(&idx, n)) {
        return 63 - idx;
    }
    return 64;
}

AWS_STATIC_IMPL size_t aws_clz_i64(int64_t n) {
    unsigned long idx = 0;
    if (_BitScanReverse64(&idx, (uint64_t)n)) {
        return 63 - idx;
    }
    return 64;
}

AWS_STATIC_IMPL size_t aws_clz_size(size_t n) {
#if SIZE_BITS == 64
    return aws_clz_u64(n);
#else
    return aws_clz_u32(n);
#endif
}

/**
 * Search from the LSB to MSB, looking for a 1
 */
AWS_STATIC_IMPL size_t aws_ctz_u32(uint32_t n) {
    unsigned long idx = 0;
    if (_BitScanForward(&idx, n)) {
        return idx;
    }
    return 32;
}

AWS_STATIC_IMPL size_t aws_ctz_i32(int32_t n) {
    unsigned long idx = 0;
    if (_BitScanForward(&idx, (uint32_t)n)) {
        return idx;
    }
    return 32;
}

AWS_STATIC_IMPL size_t aws_ctz_u64(uint64_t n) {
    unsigned long idx = 0;
    if (_BitScanForward64(&idx, n)) {
        return idx;
    }
    return 64;
}

AWS_STATIC_IMPL size_t aws_ctz_i64(int64_t n) {
    unsigned long idx = 0;
    if (_BitScanForward64(&idx, (uint64_t)n)) {
        return idx;
    }
    return 64;
}

AWS_STATIC_IMPL size_t aws_ctz_size(size_t n) {
#if SIZE_BITS == 64
    return aws_ctz_u64(n);
#else
    return aws_ctz_u32(n);
#endif
}

AWS_EXTERN_C_END
#endif /* WS_COMMON_MATH_MSVC_INL */
