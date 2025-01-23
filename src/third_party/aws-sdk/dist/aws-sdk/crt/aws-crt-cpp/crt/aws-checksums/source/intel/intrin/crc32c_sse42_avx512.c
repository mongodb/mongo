/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/checksums/private/crc_priv.h>

#include <aws/common/assert.h>
#include <aws/common/cpuid.h>
#include <aws/common/macros.h>

#include <emmintrin.h>
#include <immintrin.h>
#include <smmintrin.h>

#if defined(AWS_HAVE_AVX512_INTRINSICS) && defined(AWS_ARCH_INTEL_X64)

#    include <wmmintrin.h>

AWS_ALIGNED_TYPEDEF(const uint64_t, aligned_512_u64[8], 64);

// This macro uses casting to ensure the compiler actually uses the unaligned load instructions
#    define load_zmm(ptr) _mm512_loadu_si512((const uint8_t *)(const void *)(ptr))

/*
 * crc32c_avx512(): compute the crc32c of the buffer, where the buffer
 * length must be at least 256, and a multiple of 64. Based on:
 *
 * "Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *  V. Gopal, E. Ozturk, et al., 2009, http://download.intel.com/design/intarch/papers/323102.pdf
 */
static uint32_t s_checksums_crc32c_avx512_impl(const uint8_t *input, int length, uint32_t previous_crc) {
    AWS_ASSERT(
        length >= 256 && "invariant violated. length must be greater than 255 bytes to use avx512 to compute crc.");

    uint32_t crc = previous_crc;

    /*
     * Definitions of the bit-reflected domain constants k1,k2,k3,k4,k5,k6
     * are similar to those given at the end of the paper
     *
     * k1 = ( x ^ ( 512 * 4 + 32 ) mod P(x) << 32 )' << 1
     * k2 = ( x ^ ( 512 * 4 - 32 ) mod P(x) << 32 )' << 1
     * k3 = ( x ^ ( 512 + 32 ) mod P(x) << 32 )' << 1
     * k4 = ( x ^ ( 512 - 32 ) mod P(x) << 32 )' << 1
     * k5 = ( x ^ ( 128 + 32 ) mod P(x) << 32 )' << 1
     * k6 = ( x ^ ( 128 - 32 ) mod P(x) << 32 )' << 1
     */

    static aligned_512_u64 k1k2 = {
        0xdcb17aa4, 0xb9e02b86, 0xdcb17aa4, 0xb9e02b86, 0xdcb17aa4, 0xb9e02b86, 0xdcb17aa4, 0xb9e02b86};
    static aligned_512_u64 k3k4 = {
        0x740eef02, 0x9e4addf8, 0x740eef02, 0x9e4addf8, 0x740eef02, 0x9e4addf8, 0x740eef02, 0x9e4addf8};
    static aligned_512_u64 k9k10 = {
        0x6992cea2, 0x0d3b6092, 0x6992cea2, 0x0d3b6092, 0x6992cea2, 0x0d3b6092, 0x6992cea2, 0x0d3b6092};
    static aligned_512_u64 k1k4 = {
        0x1c291d04, 0xddc0152b, 0x3da6d0cb, 0xba4fc28e, 0xf20c0dfe, 0x493c7d27, 0x00000000, 0x00000000};

    __m512i x0, x1, x2, x3, x4, x5, x6, x7, x8, y5, y6, y7, y8;
    __m128i a1;

    /*
     * There's at least one block of 256.
     */
    x1 = load_zmm(input + 0x00);
    x2 = load_zmm(input + 0x40);
    x3 = load_zmm(input + 0x80);
    x4 = load_zmm(input + 0xC0);

    // Load the crc into a zmm register and XOR with the first 64 bytes of input
    x5 = _mm512_inserti32x4(_mm512_setzero_si512(), _mm_cvtsi32_si128((int)crc), 0);
    x1 = _mm512_xor_si512(x1, x5);

    x0 = load_zmm(k1k2);

    input += 256;
    length -= 256;

    /*
     * Parallel fold blocks of 256, if any.
     */
    while (length >= 256) {
        x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
        x6 = _mm512_clmulepi64_epi128(x2, x0, 0x00);
        x7 = _mm512_clmulepi64_epi128(x3, x0, 0x00);
        x8 = _mm512_clmulepi64_epi128(x4, x0, 0x00);

        x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
        x2 = _mm512_clmulepi64_epi128(x2, x0, 0x11);
        x3 = _mm512_clmulepi64_epi128(x3, x0, 0x11);
        x4 = _mm512_clmulepi64_epi128(x4, x0, 0x11);

        y5 = load_zmm(input + 0x00);
        y6 = load_zmm(input + 0x40);
        y7 = load_zmm(input + 0x80);
        y8 = load_zmm(input + 0xC0);

        x1 = _mm512_ternarylogic_epi64(x1, x5, y5, 0x96);
        x2 = _mm512_ternarylogic_epi64(x2, x6, y6, 0x96);
        x3 = _mm512_ternarylogic_epi64(x3, x7, y7, 0x96);
        x4 = _mm512_ternarylogic_epi64(x4, x8, y8, 0x96);

        input += 256;
        length -= 256;
    }

    /*
     * Fold 256 bytes into 64 bytes.
     */
    x0 = load_zmm(k9k10);
    x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
    x6 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
    x3 = _mm512_ternarylogic_epi64(x3, x5, x6, 0x96);

    x7 = _mm512_clmulepi64_epi128(x2, x0, 0x00);
    x8 = _mm512_clmulepi64_epi128(x2, x0, 0x11);
    x4 = _mm512_ternarylogic_epi64(x4, x7, x8, 0x96);

    x0 = load_zmm(k3k4);
    y5 = _mm512_clmulepi64_epi128(x3, x0, 0x00);
    y6 = _mm512_clmulepi64_epi128(x3, x0, 0x11);
    x1 = _mm512_ternarylogic_epi64(x4, y5, y6, 0x96);

    /*
     * Single fold blocks of 64, if any.
     */
    while (length >= 64) {
        x2 = load_zmm(input);

        x5 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
        x1 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
        x1 = _mm512_ternarylogic_epi64(x1, x2, x5, 0x96);

        input += 64;
        length -= 64;
    }

    /*
     * Fold 512-bits to 128-bits.
     */
    x0 = load_zmm(k1k4);
    x4 = _mm512_clmulepi64_epi128(x1, x0, 0x00);
    x3 = _mm512_clmulepi64_epi128(x1, x0, 0x11);
    x2 = _mm512_xor_si512(x3, x4);
    a1 = _mm_xor_si128(_mm512_extracti32x4_epi32(x1, 3), _mm512_extracti32x4_epi32(x2, 0));
    a1 = _mm_ternarylogic_epi64(a1, _mm512_extracti32x4_epi32(x2, 1), _mm512_extracti32x4_epi32(x2, 2), 0x96);

    /*
     * Fold 128-bits to 32-bits.
     */
    uint64_t val;
    val = _mm_crc32_u64(0, _mm_extract_epi64(a1, 0));
    return (uint32_t)_mm_crc32_u64(val, _mm_extract_epi64(a1, 1));
}
#endif /* #if defined(AWS_HAVE_AVX512_INTRINSICS) && (INTPTR_MAX == INT64_MAX) */

static bool detection_performed = false;
static bool detected_sse42 = false;
static bool detected_avx512 = false;
static bool detected_clmul = false;
static bool detected_vpclmulqdq = false;

uint32_t aws_checksums_crc32c_intel_avx512_with_sse_fallback(const uint8_t *input, int length, uint32_t previous_crc) {
    if (AWS_UNLIKELY(!detection_performed)) {
        detected_sse42 = aws_cpu_has_feature(AWS_CPU_FEATURE_SSE_4_2);
        detected_avx512 = aws_cpu_has_feature(AWS_CPU_FEATURE_AVX512);
        detected_clmul = aws_cpu_has_feature(AWS_CPU_FEATURE_CLMUL);
        detected_vpclmulqdq = aws_cpu_has_feature(AWS_CPU_FEATURE_VPCLMULQDQ);

        /* Simply setting the flag true to skip HW detection next time
           Not using memory barriers since the worst that can
           happen is a fallback to the non HW accelerated code. */
        detection_performed = true;
    }

    /* this is the entry point. We should only do the bit flip once. It should not be done for the subfunctions and
     * branches.*/
    uint32_t crc = ~previous_crc;

    /* For small input, forget about alignment checks - simply compute the CRC32c one byte at a time */
    if (length < (int)sizeof(slice_ptr_int_type)) {
        while (length-- > 0) {
            crc = (uint32_t)_mm_crc32_u8(crc, *input++);
        }
        return ~crc;
    }

    /* Get the 8-byte memory alignment of our input buffer by looking at the least significant 3 bits */
    int input_alignment = (uintptr_t)(input) & 0x7;

    /* Compute the number of unaligned bytes before the first aligned 8-byte chunk (will be in the range 0-7) */
    int leading = (8 - input_alignment) & 0x7;

    /* reduce the length by the leading unaligned bytes we are about to process */
    length -= leading;

    /* spin through the leading unaligned input bytes (if any) one-by-one */
    while (leading-- > 0) {
        crc = (uint32_t)_mm_crc32_u8(crc, *input++);
    }

#if defined(AWS_HAVE_AVX512_INTRINSICS) && defined(AWS_ARCH_INTEL_X64)
    int chunk_size = length & ~63;

    if (detected_avx512 && detected_vpclmulqdq && detected_clmul) {
        if (length >= 256) {
            crc = s_checksums_crc32c_avx512_impl(input, length, crc);
            /* check remaining data */
            length -= chunk_size;
            if (!length) {
                return ~crc;
            }

            /* Fall into the default crc32 for the remaining data. */
            input += chunk_size;
        }
    }
#endif

#if defined(AWS_ARCH_INTEL_X64) && !defined(_MSC_VER)
    if (detected_sse42 && detected_clmul) {
        // this function is an entry point on its own. It inverts the crc passed to it
        // does its thing and then inverts it upon return. In order to keep
        // aws_checksums_crc32c_sse42 a standalone function (which it has to be due
        // to the way its implemented) it's better that it doesn't need to know it's used
        // in a larger computation fallback.
        return aws_checksums_crc32c_clmul_sse42(input, length, ~crc);
    }
#endif

    /* Spin through remaining (aligned) 8-byte chunks using the CRC32Q quad word instruction */
    while (length >= (int)sizeof(slice_ptr_int_type)) {
        crc = (uint32_t)crc_intrin_fn(crc, *(slice_ptr_int_type *)(input));
        input += sizeof(slice_ptr_int_type);
        length -= (int)sizeof(slice_ptr_int_type);
    }

    /* Finish up with any trailing bytes using the CRC32B single byte instruction one-by-one */
    while (length-- > 0) {
        crc = (uint32_t)_mm_crc32_u8(crc, *input);
        input++;
    }

    return ~crc;
}
