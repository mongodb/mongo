#ifndef AWS_COMMON_MATH_INL
#define AWS_COMMON_MATH_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>
#include <aws/common/config.h>
#include <aws/common/math.h>

#include <limits.h>
#include <stdlib.h>

#if defined(AWS_HAVE_GCC_OVERFLOW_MATH_EXTENSIONS) && (defined(__clang__) || !defined(__cplusplus))
/*
 * GCC and clang have these super convenient overflow checking builtins...
 * but (in the case of GCC) they're only available when building C source.
 * We'll fall back to one of the other inlinable variants (or a non-inlined version)
 * if we are building this header on G++.
 */
#    include <aws/common/math.gcc_overflow.inl>
#elif defined(__x86_64__) && defined(AWS_HAVE_GCC_INLINE_ASM)
#    include <aws/common/math.gcc_x64_asm.inl>
#elif defined(__aarch64__) && defined(AWS_HAVE_GCC_INLINE_ASM)
#    include <aws/common/math.gcc_arm64_asm.inl>
#elif defined(AWS_HAVE_MSVC_INTRINSICS_X64)
#    include <aws/common/math.msvc.inl>
#elif defined(CBMC)
#    include <aws/common/math.cbmc.inl>
#else
#    ifndef AWS_HAVE_GCC_OVERFLOW_MATH_EXTENSIONS
/* Fall back to the pure-C implementations */
#        include <aws/common/math.fallback.inl>
#    else
/*
 * We got here because we are building in C++ mode but we only support overflow extensions
 * in C mode. Because the fallback is _slow_ (involving a division), we'd prefer to make a
 * non-inline call to the fast C intrinsics.
 */
#    endif /*  AWS_HAVE_GCC_OVERFLOW_MATH_EXTENSIONS */
#endif     /*  defined(AWS_HAVE_GCC_OVERFLOW_MATH_EXTENSIONS) && (defined(__clang__) || !defined(__cplusplus)) */

#if defined(__clang__) || defined(__GNUC__)
#    include <aws/common/math.gcc_builtin.inl>
#endif

AWS_EXTERN_C_BEGIN

#ifdef _MSC_VER
#    pragma warning(push)
#    pragma warning(disable : 4127) /*Disable "conditional expression is constant" */
#elif defined(__GNUC__)
#    pragma GCC diagnostic push
#    if defined(__cplusplus) && !defined(__clang__)
#        pragma GCC diagnostic ignored "-Wuseless-cast" /* Warning is C++ only (not C), and GCC only (not clang) */
#    endif
#endif

AWS_STATIC_IMPL uint64_t aws_sub_u64_saturating(uint64_t a, uint64_t b) {
    return a <= b ? 0 : a - b;
}

AWS_STATIC_IMPL int aws_sub_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    if (a < b) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    *r = a - b;
    return AWS_OP_SUCCESS;
}

AWS_STATIC_IMPL uint32_t aws_sub_u32_saturating(uint32_t a, uint32_t b) {
    return a <= b ? 0 : a - b;
}

AWS_STATIC_IMPL int aws_sub_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if (a < b) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    *r = a - b;
    return AWS_OP_SUCCESS;
}

/**
 * Multiplies a * b. If the result overflows, returns SIZE_MAX.
 */
AWS_STATIC_IMPL size_t aws_mul_size_saturating(size_t a, size_t b) {
#if SIZE_BITS == 32
    return (size_t)aws_mul_u32_saturating(a, b);
#elif SIZE_BITS == 64
    return (size_t)aws_mul_u64_saturating(a, b);
#else
#    error "Target not supported"
#endif
}

/**
 * Multiplies a * b and returns the result in *r. If the result
 * overflows, returns AWS_OP_ERR; otherwise returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_size_checked(size_t a, size_t b, size_t *r) {
#if SIZE_BITS == 32
    return aws_mul_u32_checked(a, b, (uint32_t *)r);
#elif SIZE_BITS == 64
    return aws_mul_u64_checked(a, b, (uint64_t *)r);
#else
#    error "Target not supported"
#endif
}

/**
 * Adds a + b.  If the result overflows returns SIZE_MAX.
 */
AWS_STATIC_IMPL size_t aws_add_size_saturating(size_t a, size_t b) {
#if SIZE_BITS == 32
    return (size_t)aws_add_u32_saturating(a, b);
#elif SIZE_BITS == 64
    return (size_t)aws_add_u64_saturating(a, b);
#else
#    error "Target not supported"
#endif
}

/**
 * Adds a + b and returns the result in *r. If the result
 * overflows, returns AWS_OP_ERR; otherwise returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_size_checked(size_t a, size_t b, size_t *r) {
#if SIZE_BITS == 32
    return aws_add_u32_checked(a, b, (uint32_t *)r);
#elif SIZE_BITS == 64
    return aws_add_u64_checked(a, b, (uint64_t *)r);
#else
#    error "Target not supported"
#endif
}

AWS_STATIC_IMPL size_t aws_sub_size_saturating(size_t a, size_t b) {
#if SIZE_BITS == 32
    return (size_t)aws_sub_u32_saturating(a, b);
#elif SIZE_BITS == 64
    return (size_t)aws_sub_u64_saturating(a, b);
#else
#    error "Target not supported"
#endif
}

AWS_STATIC_IMPL int aws_sub_size_checked(size_t a, size_t b, size_t *r) {
#if SIZE_BITS == 32
    return aws_sub_u32_checked(a, b, (uint32_t *)r);
#elif SIZE_BITS == 64
    return aws_sub_u64_checked(a, b, (uint64_t *)r);
#else
#    error "Target not supported"
#endif
}

/**
 * Function to check if x is power of 2
 */
AWS_STATIC_IMPL bool aws_is_power_of_two(const size_t x) {
    /* First x in the below expression is for the case when x is 0 */
    return x && (!(x & (x - 1)));
}

/**
 * Function to find the smallest result that is power of 2 >= n. Returns AWS_OP_ERR if this cannot
 * be done without overflow
 */
AWS_STATIC_IMPL int aws_round_up_to_power_of_two(size_t n, size_t *result) {
    if (n == 0) {
        *result = 1;
        return AWS_OP_SUCCESS;
    }
    if (n > SIZE_MAX_POWER_OF_TWO) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }

    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
#if SIZE_BITS == 64
    n |= n >> 32;
#endif
    n++;
    *result = n;
    return AWS_OP_SUCCESS;
}

#ifdef _MSC_VER
#    pragma warning(pop)
#elif defined(__GNUC__)
#    pragma GCC diagnostic pop
#endif /* _MSC_VER */

AWS_STATIC_IMPL uint8_t aws_min_u8(uint8_t a, uint8_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL uint8_t aws_max_u8(uint8_t a, uint8_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL int8_t aws_min_i8(int8_t a, int8_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL int8_t aws_max_i8(int8_t a, int8_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL uint16_t aws_min_u16(uint16_t a, uint16_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL uint16_t aws_max_u16(uint16_t a, uint16_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL int16_t aws_min_i16(int16_t a, int16_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL int16_t aws_max_i16(int16_t a, int16_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL uint32_t aws_min_u32(uint32_t a, uint32_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL uint32_t aws_max_u32(uint32_t a, uint32_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL int32_t aws_min_i32(int32_t a, int32_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL int32_t aws_max_i32(int32_t a, int32_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL uint64_t aws_min_u64(uint64_t a, uint64_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL uint64_t aws_max_u64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL int64_t aws_min_i64(int64_t a, int64_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL int64_t aws_max_i64(int64_t a, int64_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL size_t aws_min_size(size_t a, size_t b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL size_t aws_max_size(size_t a, size_t b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL int aws_min_int(int a, int b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL int aws_max_int(int a, int b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL float aws_min_float(float a, float b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL float aws_max_float(float a, float b) {
    return a > b ? a : b;
}

AWS_STATIC_IMPL double aws_min_double(double a, double b) {
    return a < b ? a : b;
}

AWS_STATIC_IMPL double aws_max_double(double a, double b) {
    return a > b ? a : b;
}

AWS_EXTERN_C_END

#endif /* AWS_COMMON_MATH_INL */
