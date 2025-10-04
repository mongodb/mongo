/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/checksums/crc.h>
#include <aws/checksums/private/crc64_priv.h>
#include <aws/checksums/private/crc_util.h>
#include <aws/common/cpuid.h>

large_buffer_apply_impl(crc64, uint64_t)

    AWS_ALIGNED_TYPEDEF(uint8_t, checksums_maxks_shifts_type[6][16], 16);
// Intel PSHUFB / ARM VTBL patterns for left/right shifts and masks
checksums_maxks_shifts_type aws_checksums_masks_shifts = {
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}, //
    {0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f}, // left/right
                                                                                                      // shifts
    {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}, //
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //
    {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}, // byte masks
    {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}, //
};

AWS_ALIGNED_TYPEDEF(aws_checksums_crc64_constants_t, checksums_constants, 16);

/* clang-format off */

// Pre-computed bit-reflected constants for CRC64NVME
// The actual exponents are reduced by 1 to compensate for bit-reflection (e.g. x^1024 is actually x^1023)
checksums_constants aws_checksums_crc64nvme_constants = {
    .x2048 =
        {0x37ccd3e14069cabc,
         0xa043808c0f782663, // x^2112 mod P(x) / x^2048 mod P(x)
         0x37ccd3e14069cabc,
         0xa043808c0f782663, // duplicated 3 times to support 64 byte avx512 loads
         0x37ccd3e14069cabc,
         0xa043808c0f782663,
         0x37ccd3e14069cabc,
         0xa043808c0f782663},
    .x1536 =
        {0x758ee09da263e275,
         0x6d2d13de8038b4ca, // x^1600 mod P(x) / x^1536 mod P(x)
         0x758ee09da263e275,
         0x6d2d13de8038b4ca, // duplicated 3 times to support 64 byte avx512 loads
         0x758ee09da263e275,
         0x6d2d13de8038b4ca,
         0x758ee09da263e275,
         0x6d2d13de8038b4ca},
    .x1024 =
        {0xa1ca681e733f9c40,
         0x5f852fb61e8d92dc, // x^1088 mod P(x) / x^1024 mod P(x)
         0xa1ca681e733f9c40,
         0x5f852fb61e8d92dc, // duplicated 3 times to support 64 byte avx512 loads
         0xa1ca681e733f9c40,
         0x5f852fb61e8d92dc,
         0xa1ca681e733f9c40,
         0x5f852fb61e8d92dc},
    .x512 =
        {0x0c32cdb31e18a84a,
         0x62242240ace5045a, // x^576 mod P(x) / x^512 mod P(x)
         0x0c32cdb31e18a84a,
         0x62242240ace5045a, // duplicated 3 times to support 64 byte avx512 loads
         0x0c32cdb31e18a84a,
         0x62242240ace5045a,
         0x0c32cdb31e18a84a,
         0x62242240ace5045a},
    .x384 = {0xbdd7ac0ee1a4a0f0, 0xa3ffdc1fe8e82a8b},    //  x^448 mod P(x) / x^384 mod P(x)
    .x256 = {0xb0bc2e589204f500, 0xe1e0bb9d45d7a44c},    //  x^320 mod P(x) / x^256 mod P(x)
    .x128 = {0xeadc41fd2ba3d420, 0x21e9761e252621ac},    //  x^192 mod P(x) / x^128 mod P(x)
    .mu_poly = {0x27ecfa329aef9f77, 0x34d926535897936b}, // Barrett mu / polynomial P(x) (bit-reflected)
    .trailing =
        {
            // trailing input constants for data lengths of 1-15 bytes
            {0x04f28def5347786c, 0x7f6ef0c830358979}, // 1 trailing bytes:  x^72 mod P(x) /   x^8 mod P(x)
            {0x49e1df807414fdef, 0x8776a97d73bddf69}, // 2 trailing bytes:  x^80 mod P(x) /  x^15 mod P(x)
            {0x52734ea3e726fc54, 0xff6e4e1f4e4038be}, // 3 trailing bytes:  x^88 mod P(x) /  x^24 mod P(x)
            {0x668ab3bbc976d29d, 0x8211147cbaf96306}, // 4 trailing bytes:  x^96 mod P(x) /  x^32 mod P(x)
            {0xf2fa1fae5f5c1165, 0x373d15f784905d1e}, // 5 trailing bytes: x^104 mod P(x) /  x^40 mod P(x)
            {0x9065cb6e6d39918a, 0xe9742a79ef04a5d4}, // 6 trailing bytes: x^110 mod P(x) /  x^48 mod P(x)
            {0xc23dfbc6ca591ca3, 0xfc5d27f6bf353971}, // 7 trailing bytes: x^110 mod P(x) /  x^56 mod P(x)
            {0xeadc41fd2ba3d420, 0x21e9761e252621ac}, // 8 trailing bytes: x^120 mod P(x) /  x^64 mod P(x)
            {0xf12b2236ec577cd6, 0x04f28def5347786c}, // 9 trailing bytes: x^128 mod P(x) /  x^72 mod P(x)
            {0x0298996e905d785a, 0x49e1df807414fdef}, // 10 trailing bytes: x^144 mod P(x) /  x^80 mod P(x)
            {0xf779b03b943ff311, 0x52734ea3e726fc54}, // 11 trailing bytes: x^152 mod P(x) /  x^88 mod P(x)
            {0x07797643831fd90b, 0x668ab3bbc976d29d}, // 12 trailing bytes: x^160 mod P(x) /  x^96 mod P(x)
            {0x27a8849a7bc97a27, 0xf2fa1fae5f5c1165}, // 13 trailing bytes: x^168 mod P(x) / x^104 mod P(x)
            {0xb937a2d843183b7c, 0x9065cb6e6d39918a}, // 14 trailing bytes: x^176 mod P(x) / x^112 mod P(x)
            {0x31bce594cbbacd2d, 0xc23dfbc6ca591ca3}, // 15 trailing bytes: x^184 mod P(x) / x^120 mod P(x)
        },
};
/* clang-format on */

static uint64_t (*s_crc64nvme_fn_ptr)(const uint8_t *input, int length, uint64_t prev_crc64) = 0;

uint64_t aws_checksums_crc64nvme(const uint8_t *input, int length, uint64_t prev_crc64) {
    if (AWS_UNLIKELY(!s_crc64nvme_fn_ptr)) {
#if defined(AWS_USE_CPU_EXTENSIONS) && defined(AWS_ARCH_INTEL_X64) && !(defined(_MSC_VER) && _MSC_VER < 1920)
#    if defined(AWS_HAVE_AVX512_INTRINSICS)
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_AVX512) && aws_cpu_has_feature(AWS_CPU_FEATURE_VPCLMULQDQ)) {
            s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_intel_avx512;
        } else
#    endif
#    if defined(AWS_HAVE_CLMUL) && defined(AWS_HAVE_AVX2_INTRINSICS)
            if (aws_cpu_has_feature(AWS_CPU_FEATURE_CLMUL) && aws_cpu_has_feature(AWS_CPU_FEATURE_AVX2)) {
            s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_intel_clmul;
        } else {
            s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_sw;
        }
#    endif
#    if !(defined(AWS_HAVE_AVX512_INTRINSICS) || (defined(AWS_HAVE_CLMUL) && defined(AWS_HAVE_AVX2_INTRINSICS)))
        s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_sw;
#    endif

#elif defined(AWS_USE_CPU_EXTENSIONS) && defined(AWS_ARCH_ARM64) && defined(AWS_HAVE_ARMv8_1)
        if (aws_cpu_has_feature(AWS_CPU_FEATURE_ARM_CRYPTO) && aws_cpu_has_feature(AWS_CPU_FEATURE_ARM_PMULL)) {
            s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_arm_pmull;
        } else {
            s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_sw;
        }
#else // this branch being taken means it's not arm64 and not intel with avx extensions
        s_crc64nvme_fn_ptr = aws_checksums_crc64nvme_sw;
#endif
    }

    return s_crc64nvme_fn_ptr(input, length, prev_crc64);
}

uint64_t aws_checksums_crc64nvme_ex(const uint8_t *input, size_t length, uint64_t previous_crc64) {
    return aws_large_buffer_apply_crc64(aws_checksums_crc64nvme, input, length, previous_crc64);
}
