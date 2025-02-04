#ifndef AWS_COMMON_MATH_GCC_ARM64_ASM_INL
#define AWS_COMMON_MATH_GCC_ARM64_ASM_INL

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

/* clang-format off */

AWS_EXTERN_C_BEGIN

/**
 * Multiplies a * b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_mul_u64_saturating(uint64_t a, uint64_t b) {
    /* We can use inline assembly to do this efficiently on arm64 by doing
       a high-mul and checking  the upper 64 bits of a 64x64->128b multiply
       are zero */
    uint64_t tmp = 0, res = 0;
    __asm__("umulh %x[hmul], %x[arga], %x[argb]\n"
            "mul %x[res], %x[arga], %x[argb]\n"
            "cmp %x[hmul], #0\n"
            "csinv %x[res], %x[res], xzr, eq\n"
            : /* inout: hmul is upper 64b, r is the result */ [hmul] "+&r"(tmp), [res]"+&r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    return res;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    /* We can use inline assembly to do this efficiently on arm64 by doing
       a high-mul and checking  the upper 64 bits of a 64x64->128b multiply
       are zero */

    uint64_t tmp, res;
    __asm__("umulh %x[hmul], %x[arga], %x[argb]\n"
            "mul %x[res], %x[arga], %x[argb]\n"
            : /* inout: hmul is upper 64b, r is the result */ [hmul] "=&r"(tmp), [res]"=&r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b));

    *r = res;
    if (tmp) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

/**
 * Multiplies a * b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_mul_u32_saturating(uint32_t a, uint32_t b) {
    /* We can use inline assembly to do this efficiently on arm64 by doing
       a high-mul and checking  the upper 32 bits of a 32x32->64b multiply
       are zero */

    uint64_t res = 0;
    __asm__("umull %x[res], %w[arga], %w[argb]\n"
            "cmp xzr, %x[res], lsr #32\n"
            "csinv %w[res], %w[res], wzr, eq\n"
            : /* inout: res contains both lower/upper 32b */ [res]"+&r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    return res & 0xffffffff;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    /* We can use inline assembly to do this efficiently on arm64 by doing
       a high-mul and checking  the upper 32 bits of a 32x32->64b multiply
       are zero */

    uint64_t res;
    __asm__("umull %x[res], %w[arga], %w[argb]\n"
            : /* inout: res is both upper/lower 32b */ [res]"=r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b));

    *r = res & 0xffffffff;
    if (res >> 32) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    /* We can use inline assembly to do this efficiently on arm64 by doing a
     * 64b + 64b add and checking the carry out */

    uint64_t res, flag;
    __asm__("adds %x[res], %x[arga], %x[argb]\n"
            "csinv %x[flag], xzr, xzr, cc\n"
            : /* inout: res is the result of addition; flag is -1 if carry happened */ 
	      [res]"=&r"(res), [flag] "=r"(flag)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    *r = res;
    if (flag) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_add_u64_saturating(uint64_t a, uint64_t b) {
    /* We can use inline assembly to do this efficiently on arm64 by doing a
     * 64b + 64b add and checking the carry out */

    uint64_t res;
    __asm__("adds %x[res], %x[arga], %x[argb]\n"
            "csinv %x[res], %x[res], xzr, cc\n"
            : /* inout: res is the result */ [res]"=&r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    return res;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    /* We can use inline assembly to do this efficiently on arm64 by doing a
     * 32b + 32b add and checking the carry out */

    uint32_t res, flag;
    __asm__("adds %w[res], %w[arga], %w[argb]\n"
            "csinv %w[flag], wzr, wzr, cc\n"
            : /* inout: res is 32b result */ [res]"=&r"(res), [flag] "=r"(flag)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    *r = res;
    if (flag) {
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    }
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_add_u32_saturating(uint32_t a, uint32_t b) {
    /* We can use inline assembly to do this efficiently on arm64 by doing a
     * 32b + 32b add and checking the carry out */

    uint32_t res = 0;
    __asm__("adds %w[res], %w[arga], %w[argb]\n"
            "csinv %w[res], %w[res], wzr, cc\n"
            : /* inout:  res is the result */ [res]"+&r"(res)
            : /* in: a and b */ [arga] "r"(a), [argb] "r"(b)
            : /* clobbers: cc (cmp clobbers condition codes) */ "cc");

    return res;
}

AWS_EXTERN_C_END

/* clang-format on */

#endif /* AWS_COMMON_MATH_GCC_ARM64_ASM_INL */
