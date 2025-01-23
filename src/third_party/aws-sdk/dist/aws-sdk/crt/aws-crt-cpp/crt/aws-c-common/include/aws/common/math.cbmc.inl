#ifndef AWS_COMMON_MATH_CBMC_INL
#define AWS_COMMON_MATH_CBMC_INL

/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * This header is already included, but include it again to make editor
 * highlighting happier.
 */
#include <aws/common/common.h>

AWS_EXTERN_C_BEGIN

/* This header does safe operations. Supressing the checks within these functions
 * avoids unnecessary CBMC assertions
 */
#pragma CPROVER check push
#pragma CPROVER check disable "unsigned-overflow"

/**
 * Multiplies a * b. If the result overflows, returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_mul_u64_saturating(uint64_t a, uint64_t b) {
    if (__CPROVER_overflow_mult(a, b))
        return UINT64_MAX;
    return a * b;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    if (__CPROVER_overflow_mult(a, b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a * b;
    return AWS_OP_SUCCESS;
}

/**
 * Multiplies a * b. If the result overflows, returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_mul_u32_saturating(uint32_t a, uint32_t b) {
    if (__CPROVER_overflow_mult(a, b))
        return UINT32_MAX;
    return a * b;
}

/**
 * If a * b overflows, returns AWS_OP_ERR; otherwise multiplies
 * a * b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_mul_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if (__CPROVER_overflow_mult(a, b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a * b;
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b.  If the result overflows returns 2^64 - 1.
 */
AWS_STATIC_IMPL uint64_t aws_add_u64_saturating(uint64_t a, uint64_t b) {
    if (__CPROVER_overflow_plus(a, b))
        return UINT64_MAX;
    return a + b;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u64_checked(uint64_t a, uint64_t b, uint64_t *r) {
    if (__CPROVER_overflow_plus(a, b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a + b;
    return AWS_OP_SUCCESS;
}

/**
 * Adds a + b. If the result overflows returns 2^32 - 1.
 */
AWS_STATIC_IMPL uint32_t aws_add_u32_saturating(uint32_t a, uint32_t b) {
    if (__CPROVER_overflow_plus(a, b))
        return UINT32_MAX;
    return a + b;
}

/**
 * If a + b overflows, returns AWS_OP_ERR; otherwise adds
 * a + b, returns the result in *r, and returns AWS_OP_SUCCESS.
 */
AWS_STATIC_IMPL int aws_add_u32_checked(uint32_t a, uint32_t b, uint32_t *r) {
    if (__CPROVER_overflow_plus(a, b))
        return aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    *r = a + b;
    return AWS_OP_SUCCESS;
}

#pragma CPROVER check pop

AWS_EXTERN_C_END

#endif /* AWS_COMMON_MATH_CBMC_INL */
