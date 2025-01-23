/*
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/common/cpuid.h>
#include <stdlib.h>

#if defined(__linux__) || defined(__FreeBSD__)
#    include <sys/auxv.h>

static unsigned long s_hwcap[2];
static bool s_hwcap_cached;

struct cap_bits {
    unsigned long cap;
    unsigned long bit;
};

#    if (defined(__aarch64__))
struct cap_bits s_check_cap[AWS_CPU_FEATURE_COUNT] = {
    [AWS_CPU_FEATURE_ARM_CRC] = {0, 1 << 7 /* HWCAP_CRC32 */},
    [AWS_CPU_FEATURE_ARM_PMULL] = {0, 1 << 4 /* HWCAP_PMULL */},
    [AWS_CPU_FEATURE_ARM_CRYPTO] = {0, 1 << 3 /* HWCAP_AES */},
};
#    else
struct cap_bits s_check_cap[AWS_CPU_FEATURE_COUNT] = {
    [AWS_CPU_FEATURE_ARM_CRC] = {1, 1 << 4 /* HWCAP_CRC */},
};
#    endif

#    if (defined(__linux__))
static void s_cache_hwcap(void) {
    s_hwcap[0] = getauxval(AT_HWCAP);
    s_hwcap[1] = getauxval(AT_HWCAP2);
    s_hwcap_cached = true;
}
#    elif (defined(__FreeBSD__))
static void s_cache_hwcap(void) {
    int ret;

    ret = elf_aux_info(AT_HWCAP, &s_hwcap[0], sizeof(unsigned long));
    if (ret)
        s_hwcap[0] = 0;

    ret = elf_aux_info(AT_HWCAP2, &s_hwcap[1], sizeof(unsigned long));
    if (ret)
        s_hwcap[1] = 0;
    s_hwcap_cached = true;
}
#    else
#        error "Unknown method"
#    endif

bool aws_cpu_has_feature(enum aws_cpu_feature_name feature_name) {

    if (!s_hwcap_cached)
        s_cache_hwcap();

    switch (feature_name) {
        case AWS_CPU_FEATURE_ARM_CRC:
#    if (defined(__aarch64__))
        case AWS_CPU_FEATURE_ARM_PMULL:
        case AWS_CPU_FEATURE_ARM_CRYPTO:
#    endif // (defined(__aarch64__))
            return s_hwcap[s_check_cap[feature_name].cap] & s_check_cap[feature_name].bit;
        default:
            return false;
    }
}

#else  /* defined(__linux__) || defined(__FreeBSD__) */
bool aws_cpu_has_feature(enum aws_cpu_feature_name feature_name) {
    return false;
}
#endif /* defined(__linux__) || defined(__FreeBSD__) */
