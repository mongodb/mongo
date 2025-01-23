/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <aws/checksums/private/crc64_priv.h>
#include <aws/common/config.h>

#if INTPTR_MAX == INT64_MAX && defined(AWS_HAVE_ARMv8_1)

#    include <arm_neon.h>

// Load a uint8x16_t neon register from uint8_t pointer
#    define load_u8(uint8_t_ptr) vld1q_u8((uint8_t_ptr))
// Load a poly64x2_t neon register from a uint8_t pointer
#    define load_p64_u8(uint8_t_ptr) vreinterpretq_p64_u8(load_u8(uint8_t_ptr))
// Load a poly64x2_t neon register from a uint64_t pointer
#    define load_p64(uint64_t_ptr) vreinterpretq_p64_u64(vld1q_u64((uint64_t_ptr)))
// Mask the bytes in a neon uint8x16_t register and preserve 0 to 15 least significant bytes.
#    define mask_low_u8(u8, count) vandq_u8(u8, load_u8(aws_checksums_masks_shifts[5] - (intptr_t)(count)))
// Mask the bytes in a neon uint8x16_t register and preserve 0 to 15 most significant bytes.
#    define mask_high_u8(u8, count) vandq_u8(u8, load_u8(aws_checksums_masks_shifts[3] + (intptr_t)(count)))
// Mask the bytes in a neon poly64x2_t register and preserve 0 to 15 most significant bytes.
#    define mask_high_p64(poly, count) vreinterpretq_p64_u8(mask_high_u8(vreinterpretq_u8_p64(poly), count))
// Left shift bytes in a neon uint8x16_t register - shift count from 0 to 15.
#    define left_shift_u8(u8, count) vqtbl1q_u8(u8, load_u8(aws_checksums_masks_shifts[1] - (intptr_t)(count)))
// Right shift bytes in a neon uint8x16_t register - shift count from 0 to 15.
#    define right_shift_u8(u8, count) vqtbl1q_u8(u8, load_u8(aws_checksums_masks_shifts[1] + (intptr_t)(count)))
// Left shift bytes in a neon poly64x2_t register - shift count from 0 to 15.
#    define left_shift_p64(poly, count) vreinterpretq_p64_u8(left_shift_u8(vreinterpretq_u8_p64(poly), count))
// Right shift a neon poly64x2_t register 0 to 15 bytes - imm must be an immediate constant
#    define right_shift_imm_p64(poly, imm)                                                                             \
        vreinterpretq_p64_u8(vextq_u8(vreinterpretq_u8_p64(poly), vdupq_n_u8(0), imm))
// Carryless multiply the lower 64-bit halves of two poly64x2_t neon registers
#    define pmull_lo(a, b)                                                                                             \
        (vreinterpretq_p64_p128(vmull_p64((poly64_t)vreinterpretq_p128_p64(a), (poly64_t)vreinterpretq_p128_p64(b))))
// Carryless multiply the upper 64-bit halves of two poly64x2_t neon registers
#    define pmull_hi(a, b) (vreinterpretq_p64_p128(vmull_high_p64((a), (b))))
// XOR two neon poly64x2_t registers
#    define xor_p64(a, b) vreinterpretq_p64_u8(veorq_u8(vreinterpretq_u8_p64(a), vreinterpretq_u8_p64(b)))
#    if defined(__ARM_FEATURE_SHA3)
// The presence of the ARM SHA3 feature also implies the three-way xor instruction
#        define xor3_p64(a, b, c)                                                                                      \
            vreinterpretq_p64_u64(                                                                                     \
                veor3q_u64(vreinterpretq_u64_p64(a), vreinterpretq_u64_p64(b), vreinterpretq_u64_p64(c)))
#    else
// Without SHA3, implement three-way xor with two normal xors
#        define xor3_p64(a, b, c) xor_p64(xor_p64(a, b), c)
#    endif // defined(__ARM_FEATURE_SHA3)

/** Compute CRC64NVME using ARMv8 NEON +crypto/pmull64 instructions. */
uint64_t aws_checksums_crc64nvme_arm_pmull(const uint8_t *input, int length, const uint64_t previous_crc64) {
    if (!input || length <= 0) {
        return previous_crc64;
    }

    // the amount of complexity required to handle vector instructions on
    // memory regions smaller than an xmm register does not justify the very negligible performance gains
    // we would get for using it on an input this small.
    if (length < 16) {
        return aws_checksums_crc64nvme_sw(input, length, previous_crc64);
    }

    // Invert the previous crc bits and load into the lower half of a neon register
    poly64x2_t a1 = vreinterpretq_p64_u64(vcombine_u64(vcreate_u64(~previous_crc64), vcreate_u64(0)));

    // Load the x^128 and x^192 constants - they'll (very likely) be needed
    const poly64x2_t x128 = load_p64(aws_checksums_crc64nvme_constants.x128);

    // Load the next 16 bytes of input and XOR with the previous crc
    a1 = xor_p64(a1, load_p64_u8(input));
    input += 16;
    length -= 16;

    if (length < 112) {

        const poly64x2_t x256 = load_p64(aws_checksums_crc64nvme_constants.x256);

        if (length & 64) {
            // Fold the current crc register with 64 bytes of input by multiplying 64-bit chunks by x^576 through
            // x^128
            const poly64x2_t x512 = load_p64(aws_checksums_crc64nvme_constants.x512);
            const poly64x2_t x384 = load_p64(aws_checksums_crc64nvme_constants.x384);
            poly64x2_t b1 = load_p64_u8(input + 0);
            poly64x2_t c1 = load_p64_u8(input + 16);
            poly64x2_t d1 = load_p64_u8(input + 32);
            poly64x2_t e1 = load_p64_u8(input + 48);
            a1 = xor3_p64(pmull_lo(x512, a1), pmull_hi(x512, a1), pmull_lo(x384, b1));
            b1 = xor3_p64(pmull_hi(x384, b1), pmull_lo(x256, c1), pmull_hi(x256, c1));
            c1 = xor3_p64(pmull_lo(x128, d1), pmull_hi(x128, d1), e1);
            a1 = xor3_p64(a1, b1, c1);
            input += 64;
        }

        if (length & 32) {
            // Fold the current running value with 32 bytes of input by multiplying 64-bit chunks by x^320 through
            // x^128
            poly64x2_t b1 = load_p64_u8(input + 0);
            poly64x2_t c1 = load_p64_u8(input + 16);
            a1 = xor3_p64(c1, pmull_lo(x256, a1), pmull_hi(x256, a1));
            a1 = xor3_p64(a1, pmull_lo(x128, b1), pmull_hi(x128, b1));
            input += 32;
        }
    } else { // There are 112 or more bytes of input

        const poly64x2_t x1024 = load_p64(aws_checksums_crc64nvme_constants.x1024);

        // Load another 112 bytes of input
        poly64x2_t b1 = load_p64_u8(input + 0);
        poly64x2_t c1 = load_p64_u8(input + 16);
        poly64x2_t d1 = load_p64_u8(input + 32);
        poly64x2_t e1 = load_p64_u8(input + 48);
        poly64x2_t f1 = load_p64_u8(input + 64);
        poly64x2_t g1 = load_p64_u8(input + 80);
        poly64x2_t h1 = load_p64_u8(input + 96);
        input += 112;
        length -= 112;

        // Spin through additional chunks of 128 bytes, if any
        int loops = length / 128;
        while (loops--) {
            // Fold input values in parallel by multiplying by x^1088 and x^1024 constants
            a1 = xor3_p64(pmull_lo(x1024, a1), pmull_hi(x1024, a1), load_p64_u8(input + 0));
            b1 = xor3_p64(pmull_lo(x1024, b1), pmull_hi(x1024, b1), load_p64_u8(input + 16));
            c1 = xor3_p64(pmull_lo(x1024, c1), pmull_hi(x1024, c1), load_p64_u8(input + 32));
            d1 = xor3_p64(pmull_lo(x1024, d1), pmull_hi(x1024, d1), load_p64_u8(input + 48));
            e1 = xor3_p64(pmull_lo(x1024, e1), pmull_hi(x1024, e1), load_p64_u8(input + 64));
            f1 = xor3_p64(pmull_lo(x1024, f1), pmull_hi(x1024, f1), load_p64_u8(input + 80));
            g1 = xor3_p64(pmull_lo(x1024, g1), pmull_hi(x1024, g1), load_p64_u8(input + 96));
            h1 = xor3_p64(pmull_lo(x1024, h1), pmull_hi(x1024, h1), load_p64_u8(input + 112));
            input += 128;
        }

        // Fold 128 bytes down to 64 bytes by multiplying by the x^576 and x^512 constants
        const poly64x2_t x512 = load_p64(aws_checksums_crc64nvme_constants.x512);
        a1 = xor3_p64(e1, pmull_lo(x512, a1), pmull_hi(x512, a1));
        b1 = xor3_p64(f1, pmull_lo(x512, b1), pmull_hi(x512, b1));
        c1 = xor3_p64(g1, pmull_lo(x512, c1), pmull_hi(x512, c1));
        d1 = xor3_p64(h1, pmull_lo(x512, d1), pmull_hi(x512, d1));

        if (length & 64) {
            // Fold the current 64 bytes with 64 bytes of input by multiplying by x^576 and x^512 constants
            a1 = xor3_p64(pmull_lo(x512, a1), pmull_hi(x512, a1), load_p64_u8(input + 0));
            b1 = xor3_p64(pmull_lo(x512, b1), pmull_hi(x512, b1), load_p64_u8(input + 16));
            c1 = xor3_p64(pmull_lo(x512, c1), pmull_hi(x512, c1), load_p64_u8(input + 32));
            d1 = xor3_p64(pmull_lo(x512, d1), pmull_hi(x512, d1), load_p64_u8(input + 48));
            input += 64;
        }

        // Fold 64 bytes down to 32 bytes by multiplying by the x^320 and x^256 constants
        const poly64x2_t x256 = load_p64(aws_checksums_crc64nvme_constants.x256);
        a1 = xor3_p64(c1, pmull_lo(x256, a1), pmull_hi(x256, a1));
        b1 = xor3_p64(d1, pmull_lo(x256, b1), pmull_hi(x256, b1));

        if (length & 32) {
            // Fold the current running value with 32 bytes of input by multiplying by x^320 and x^256 constants
            a1 = xor3_p64(pmull_lo(x256, a1), pmull_hi(x256, a1), load_p64_u8(input + 0));
            b1 = xor3_p64(pmull_lo(x256, b1), pmull_hi(x256, b1), load_p64_u8(input + 16));
            input += 32;
        }

        // Fold 32 bytes down to 16 bytes by multiplying by x^192 and x^128 constants
        a1 = xor3_p64(b1, pmull_lo(x128, a1), pmull_hi(x128, a1));
    }

    if (length & 16) {
        // Fold the current 16 bytes with 16 bytes of input by multiplying by x^192 and x^128 constants
        a1 = xor3_p64(pmull_lo(x128, a1), pmull_hi(x128, a1), load_p64_u8(input + 0));
        input += 16;
    }

    // There must only be 0-15 bytes of input left
    length &= 15;

    if (length == 0) {
        // Multiply the lower half of the crc register by x^128 (swapping upper and lower halves)
        poly64x2_t mul_by_x128 = pmull_lo(a1, vextq_p64(x128, x128, 1));
        // XOR the result with the right shifted upper half of the crc
        a1 = xor_p64(right_shift_imm_p64(a1, 8), mul_by_x128);
    } else {
        // Handle any trailing input from 1-15 bytes
        const poly64x2_t trailing_constants = load_p64(aws_checksums_crc64nvme_constants.trailing[length - 1]);
        // Multiply the crc by a pair of trailing length constants in order to fold it into the trailing input
        a1 = xor_p64(pmull_lo(a1, trailing_constants), pmull_hi(a1, trailing_constants));
        // Safely load ending at the last byte of trailing input and mask out any leading garbage
        poly64x2_t trailing_input = mask_high_p64(load_p64_u8(input + length - 16), length);
        // Multiply the lower half of the trailing input register by x^128 (swapping x^192 and x^128 halves)
        poly64x2_t mul_by_x128 = pmull_lo(trailing_input, vextq_p64(x128, x128, 1));
        // XOR the results with the right shifted upper half of the trailing input
        a1 = xor3_p64(a1, right_shift_imm_p64(trailing_input, 8), mul_by_x128);
    }

    // Barrett modular reduction

    // Load the Barrett mu and (bit-reflected) polynomial
    const poly64x2_t mu_poly = load_p64(aws_checksums_crc64nvme_constants.mu_poly);
    // Multiply the lower half of the crc register by mu (mu is in the lower half of mu_poly)
    poly64x2_t mul_by_mu = pmull_lo(a1, mu_poly);
    // Multiply lower half of mul_by_mu result by poly (which is swapped into the lower half)
    poly64x2_t mul_by_poly = pmull_lo(mul_by_mu, vextq_p64(mu_poly, mu_poly, 1));
    // Swap halves of mul_by_mu and add the upper halves of everything
    poly64x2_t result = xor3_p64(a1, vextq_p64(mul_by_mu, mul_by_mu, 1), mul_by_poly);

    // Reduction result is the upper half - invert the bits before returning the crc
    return ~vgetq_lane_u64(vreinterpretq_u64_p64(result), 1);
}

#endif // INTPTR_MAX == INT64_MAX && defined(AWS_HAVE_ARMv8_1)
