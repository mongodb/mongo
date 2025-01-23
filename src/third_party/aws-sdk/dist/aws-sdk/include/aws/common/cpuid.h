#ifndef AWS_COMMON_CPUID_H
#define AWS_COMMON_CPUID_H
/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/common/common.h>

AWS_PUSH_SANE_WARNING_LEVEL

enum aws_cpu_feature_name {
    AWS_CPU_FEATURE_CLMUL,
    AWS_CPU_FEATURE_SSE_4_1,
    AWS_CPU_FEATURE_SSE_4_2,
    AWS_CPU_FEATURE_AVX2,
    AWS_CPU_FEATURE_AVX512,
    AWS_CPU_FEATURE_ARM_CRC,
    AWS_CPU_FEATURE_BMI2,
    AWS_CPU_FEATURE_VPCLMULQDQ,
    AWS_CPU_FEATURE_ARM_PMULL,
    AWS_CPU_FEATURE_ARM_CRYPTO,
    AWS_CPU_FEATURE_COUNT,
};

AWS_EXTERN_C_BEGIN

/**
 * Returns true if a cpu feature is supported, false otherwise.
 */
AWS_COMMON_API bool aws_cpu_has_feature(enum aws_cpu_feature_name feature_name);

AWS_EXTERN_C_END
AWS_POP_SANE_WARNING_LEVEL

#endif /* AWS_COMMON_CPUID_H */
