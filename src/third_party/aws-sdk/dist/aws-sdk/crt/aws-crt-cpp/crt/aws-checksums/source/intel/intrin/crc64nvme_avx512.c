/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/checksums/private/crc64_priv.h>

#if defined(AWS_HAVE_AVX512_INTRINSICS) && defined(AWS_ARCH_INTEL_X64)

#    include <emmintrin.h>
#    include <immintrin.h>
#    include <smmintrin.h>
#    include <wmmintrin.h>

#    define load_xmm(ptr) _mm_loadu_si128((const __m128i *)(const void *)(ptr))
#    define mask_high_bytes(xmm, count)                                                                                \
        _mm_and_si128((xmm), load_xmm(aws_checksums_masks_shifts[3] + (intptr_t)(count)))
#    define cmull_xmm_hi(xmm1, xmm2) _mm_clmulepi64_si128((xmm1), (xmm2), 0x11)
#    define cmull_xmm_lo(xmm1, xmm2) _mm_clmulepi64_si128((xmm1), (xmm2), 0x00)
#    define cmull_xmm_pair(xmm1, xmm2) _mm_xor_si128(cmull_xmm_hi((xmm1), (xmm2)), cmull_xmm_lo((xmm1), (xmm2)))
#    define xor_xmm(xmm1, xmm2, xmm3)                                                                                  \
        _mm_ternarylogic_epi64((xmm1), (xmm2), (xmm3), 0x96) // The constant 0x96 produces a 3-way XOR

#    define load_zmm(ptr) _mm512_loadu_si512((const uint8_t *)(const void *)(ptr))
#    define cmull_zmm_hi(zmm1, zmm2) _mm512_clmulepi64_epi128((zmm1), (zmm2), 0x11)
#    define cmull_zmm_lo(zmm1, zmm2) _mm512_clmulepi64_epi128((zmm1), (zmm2), 0x00)
#    define cmull_zmm_pair(zmm1, zmm2) _mm512_xor_si512(cmull_zmm_hi((zmm1), (zmm2)), cmull_zmm_lo((zmm1), (zmm2)))
#    define xor_zmm(zmm1, zmm2, zmm3)                                                                                  \
        _mm512_ternarylogic_epi64((zmm1), (zmm2), (zmm3), 0x96) // The constant 0x96 produces a 3-way XOR

uint64_t aws_checksums_crc64nvme_intel_avx512(const uint8_t *input, int length, const uint64_t previous_crc64) {

    if (length < 512) {
        return aws_checksums_crc64nvme_intel_clmul(input, length, previous_crc64);
    }

    // The following code assumes a minimum of 256 bytes of input

    // Load the (inverted) CRC into a ZMM register
    __m512i x1 = _mm512_inserti32x4(_mm512_setzero_si512(), _mm_cvtsi64_si128((int64_t)~previous_crc64), 0);
    // Load the first 64 bytes into a zmm register and XOR with the (inverted) crc
    x1 = _mm512_xor_si512(x1, load_zmm(input));
    // Load 192 more bytes of input
    __m512i x2 = load_zmm(input + 0x40);
    __m512i x3 = load_zmm(input + 0x80);
    __m512i x4 = load_zmm(input + 0xc0);
    input += 256;
    length -= 256;

    const __m512i kp_2048 = load_zmm(aws_checksums_crc64nvme_constants.x2048);
    const __m512i kp_512 = load_zmm(aws_checksums_crc64nvme_constants.x512);

    int loops = length / 256;
    length &= 255;

    // Parallel fold blocks of 256 bytes, if any
    while (loops--) {
        x1 = xor_zmm(cmull_zmm_lo(kp_2048, x1), cmull_zmm_hi(kp_2048, x1), load_zmm(input + 0x00));
        x2 = xor_zmm(cmull_zmm_lo(kp_2048, x2), cmull_zmm_hi(kp_2048, x2), load_zmm(input + 0x40));
        x3 = xor_zmm(cmull_zmm_lo(kp_2048, x3), cmull_zmm_hi(kp_2048, x3), load_zmm(input + 0x80));
        x4 = xor_zmm(cmull_zmm_lo(kp_2048, x4), cmull_zmm_hi(kp_2048, x4), load_zmm(input + 0xc0));
        input += 256;
    }

    // Fold 2048 bits into 512 bits
    const __m512i kp_1536 = load_zmm(aws_checksums_crc64nvme_constants.x1536);
    const __m512i kp_1024 = load_zmm(aws_checksums_crc64nvme_constants.x1024);

    x1 = xor_zmm(cmull_zmm_lo(kp_1536, x1), cmull_zmm_hi(kp_1536, x1), cmull_zmm_lo(kp_1024, x2));
    x2 = xor_zmm(cmull_zmm_hi(kp_1024, x2), cmull_zmm_lo(kp_512, x3), cmull_zmm_hi(kp_512, x3));
    x1 = xor_zmm(x1, x2, x4);

    // Fold blocks of 512 bits, if any
    loops = length / 64;
    length &= 63;
    while (loops--) {
        x1 = xor_zmm(cmull_zmm_lo(kp_512, x1), cmull_zmm_hi(kp_512, x1), load_zmm(input));
        input += 64;
    }

    // Load 64 bytes of constants: x^448, x^384, x^320, x^256, x^192, x^128, N/A, N/A
    const __m512i kp_384 = load_zmm(aws_checksums_crc64nvme_constants.x384);

    // Fold 512 bits to 128 bits
    x2 = cmull_zmm_pair(kp_384, x1);
    __m128i a1 = _mm_xor_si128(_mm512_extracti32x4_epi32(x1, 3), _mm512_extracti32x4_epi32(x2, 0));
    a1 = xor_xmm(a1, _mm512_extracti32x4_epi32(x2, 1), _mm512_extracti32x4_epi32(x2, 2));

    // Single fold blocks of 128 bits, if any
    loops = length / 16;
    __m128i kp_128 = _mm512_extracti32x4_epi32(kp_384, 2);
    while (loops--) {
        a1 = xor_xmm(cmull_xmm_lo(kp_128, a1), cmull_xmm_hi(kp_128, a1), load_xmm(input));
        input += 16;
    }

    // The remaining length can be only 0-15 bytes
    length &= 15;

    // Load the x^128 constant (note that we don't need x^192).
    const __m128i x128 = _mm_set_epi64x(0, aws_checksums_crc64nvme_constants.x128[1]);
    if (length == 0) {
        // Multiply the lower half of the crc register by x^128 and XOR the result with the upper half of the crc.
        a1 = _mm_xor_si128(_mm_bsrli_si128(a1, 8), cmull_xmm_lo(a1, x128));
    } else {
        // Handle any trailing input from 1-15 bytes.
        __m128i trailing_constants = load_xmm(aws_checksums_crc64nvme_constants.trailing[length - 1]);
        // Multiply the crc by a pair of trailing length constants in order to fold it into the trailing input.
        a1 = cmull_xmm_pair(a1, trailing_constants);
        // Safely load ending at the trailing input and mask out any leading garbage
        __m128i trailing_input = mask_high_bytes(load_xmm(input + length - 16), length);
        // Multiply the lower half of the trailing input register by x^128
        __m128i mul_by_x128 = cmull_xmm_lo(trailing_input, x128);
        // XOR the results with the upper half of the trailing input
        a1 = xor_xmm(a1, _mm_bsrli_si128(trailing_input, 8), mul_by_x128);
    }

    // Barrett modular reduction
    const __m128i mu_poly = load_xmm(&aws_checksums_crc64nvme_constants.mu_poly);
    // Multiply the lower half of input by mu
    __m128i mul_by_mu = _mm_clmulepi64_si128(mu_poly, a1, 0x00);
    // Multiply the lower half of the mul_by_mu result by poly (it's in the upper half)
    __m128i mul_by_poly = _mm_clmulepi64_si128(mu_poly, mul_by_mu, 0x01);
    // Left shift mul_by_mu to get the low half into the upper half and XOR all the upper halves
    __m128i reduced = xor_xmm(a1, _mm_bslli_si128(mul_by_mu, 8), mul_by_poly);
    // After the XORs, the CRC falls in the upper half of the register - invert the bits before returning the crc
    return ~(uint64_t)_mm_extract_epi64(reduced, 1);
}

#endif /* defined(AWS_HAVE_AVX512_INTRINSICS) && defined(AWS_ARCH_INTEL_X64)*/
