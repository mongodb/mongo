/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

/*
 * MSVC wants us to use the non-portable _dupenv_s instead; since we need
 * to remain portable, tell MSVC to suppress this warning.
 */
#define _CRT_SECURE_NO_WARNINGS

#include <aws/common/cpuid.h>
#include <stdlib.h>

extern void aws_run_cpuid(uint32_t eax, uint32_t ecx, uint32_t *abcd);

typedef bool(has_feature_fn)(void);

static bool s_has_clmul(void) {
    uint32_t abcd[4];
    uint32_t clmul_mask = 0x00000002;
    aws_run_cpuid(1, 0, abcd);

    if ((abcd[2] & clmul_mask) != clmul_mask)
        return false;

    return true;
}

static bool s_has_sse41(void) {
    uint32_t abcd[4];
    uint32_t sse41_mask = 0x00080000;
    aws_run_cpuid(1, 0, abcd);

    if ((abcd[2] & sse41_mask) != sse41_mask)
        return false;

    return true;
}

static bool s_has_sse42(void) {
    uint32_t abcd[4];
    uint32_t sse42_mask = 0x00100000;
    aws_run_cpuid(1, 0, abcd);

    if ((abcd[2] & sse42_mask) != sse42_mask)
        return false;

    return true;
}

static bool s_has_avx2(void) {
    uint32_t abcd[4];

    /* Check AVX2:
     * CPUID.(EAX=07H, ECX=0H):EBX.AVX2[bit 5]==1 */
    uint32_t avx2_mask = (1 << 5);
    aws_run_cpuid(7, 0, abcd);
    if ((abcd[1] & avx2_mask) != avx2_mask) {
        return false;
    }

    /* Also check AVX:
     * CPUID.(EAX=01H, ECX=0H):ECX.AVX[bit 28]==1
     *
     * NOTE: It SHOULD be impossible for a CPU to support AVX2 without supporting AVX.
     * But we've received crash reports where the AVX2 feature check passed
     * and then an AVX instruction caused an "invalid instruction" crash.
     *
     * We diagnosed these machines by asking users to run the sample program from:
     * https://docs.microsoft.com/en-us/cpp/intrinsics/cpuid-cpuidex?view=msvc-160
     * and observed the following results:
     *
     *      AVX not supported
     *      AVX2 supported
     *
     * We don't know for sure what was up with those machines, but this extra
     * check should stop them from running our AVX/AVX2 code paths. */
    uint32_t avx1_mask = (1 << 28);
    aws_run_cpuid(1, 0, abcd);
    if ((abcd[2] & avx1_mask) != avx1_mask) {
        return false;
    }

    return true;
}

static bool s_has_avx512(void) {
    uint32_t abcd[4];

    /* Check AVX512F:
     * CPUID.(EAX=07H, ECX=0H):EBX.AVX512[bit 16]==1 */
    uint32_t avx512_mask = (1 << 16);
    aws_run_cpuid(7, 0, abcd);
    if ((abcd[1] & avx512_mask) != avx512_mask) {
        return false;
    }

    return true;
}

static bool s_has_bmi2(void) {
    uint32_t abcd[4];

    /* Check BMI2:
     * CPUID.(EAX=07H, ECX=0H):EBX.BMI2[bit 8]==1 */
    uint32_t bmi2_mask = (1 << 8);
    aws_run_cpuid(7, 0, abcd);
    if ((abcd[1] & bmi2_mask) != bmi2_mask) {
        return false;
    }

    return true;
}

static bool s_has_vpclmulqdq(void) {
    uint32_t abcd[4];
    /* Check VPCLMULQDQ:
     * CPUID.(EAX=07H, ECX=0H):ECX.VPCLMULQDQ[bit 10]==1 */
    uint32_t vpclmulqdq_mask = (1 << 10);
    aws_run_cpuid(7, 0, abcd);
    if ((abcd[2] & vpclmulqdq_mask) != vpclmulqdq_mask) {
        return false;
    }
    return true;
}

has_feature_fn *s_check_cpu_feature[AWS_CPU_FEATURE_COUNT] = {
    [AWS_CPU_FEATURE_CLMUL] = s_has_clmul,
    [AWS_CPU_FEATURE_SSE_4_1] = s_has_sse41,
    [AWS_CPU_FEATURE_SSE_4_2] = s_has_sse42,
    [AWS_CPU_FEATURE_AVX2] = s_has_avx2,
    [AWS_CPU_FEATURE_AVX512] = s_has_avx512,
    [AWS_CPU_FEATURE_BMI2] = s_has_bmi2,
    [AWS_CPU_FEATURE_VPCLMULQDQ] = s_has_vpclmulqdq,
};

bool aws_cpu_has_feature(enum aws_cpu_feature_name feature_name) {
    if (s_check_cpu_feature[feature_name])
        return s_check_cpu_feature[feature_name]();
    return false;
}

#define CPUID_AVAILABLE 0
#define CPUID_UNAVAILABLE 1
static int cpuid_state = 2;

bool aws_common_private_has_avx2(void) {
    if (AWS_LIKELY(cpuid_state == 0)) {
        return true;
    }
    if (AWS_LIKELY(cpuid_state == 1)) {
        return false;
    }

    /* Provide a hook for testing fallbacks and benchmarking */
    const char *env_avx2_enabled = getenv("AWS_COMMON_AVX2");
    if (env_avx2_enabled) {
        int is_enabled = atoi(env_avx2_enabled);
        cpuid_state = !is_enabled;
        return is_enabled;
    }

    bool available = aws_cpu_has_feature(AWS_CPU_FEATURE_AVX2);
    cpuid_state = available ? CPUID_AVAILABLE : CPUID_UNAVAILABLE;

    return available;
}
