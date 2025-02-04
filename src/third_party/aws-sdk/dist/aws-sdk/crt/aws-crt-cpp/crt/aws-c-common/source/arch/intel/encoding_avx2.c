/**
 * Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
 * SPDX-License-Identifier: Apache-2.0.
 */

#include <emmintrin.h>
#include <immintrin.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <aws/common/common.h>

/***** Decode logic *****/

/*
 * Decodes ranges of bytes in place
 * For each byte of 'in' that is between lo and hi (inclusive), adds offset and _adds_ it to the corresponding offset in
 * out.
 */
static inline __m256i translate_range(__m256i in, uint8_t lo, uint8_t hi, uint8_t offset) {
    __m256i lovec = _mm256_set1_epi8(lo);
    __m256i hivec = _mm256_set1_epi8((char)(hi - lo));
    __m256i offsetvec = _mm256_set1_epi8(offset);

    __m256i tmp = _mm256_sub_epi8(in, lovec);
    /*
     * we'll use the unsigned min operator to do our comparison. Note that
     * there's no unsigned compare as a comparison intrinsic.
     */
    __m256i mask = _mm256_min_epu8(tmp, hivec);
    /* if mask = tmp, then keep that byte */
    mask = _mm256_cmpeq_epi8(mask, tmp);

    tmp = _mm256_add_epi8(tmp, offsetvec);
    tmp = _mm256_and_si256(tmp, mask);
    return tmp;
}

/*
 * For each 8-bit element in in, if the element equals match, add to the corresponding element in out the value decode.
 */
static inline __m256i translate_exact(__m256i in, uint8_t match, uint8_t decode) {
    __m256i mask = _mm256_cmpeq_epi8(in, _mm256_set1_epi8(match));
    return _mm256_and_si256(mask, _mm256_set1_epi8(decode));
}

/*
 * Input: a pointer to a 256-bit vector of base64 characters
 * The pointed-to-vector is replaced by a 256-bit vector of 6-bit decoded parts;
 * on decode failure, returns false, else returns true on success.
 */
static inline bool decode_vec(__m256i *in) {
    __m256i tmp1, tmp2, tmp3;

    /*
     * Base64 decoding table, see RFC4648
     *
     * Note that we use multiple vector registers to try to allow the CPU to
     * paralellize the merging ORs
     */
    tmp1 = translate_range(*in, 'A', 'Z', 0 + 1);
    tmp2 = translate_range(*in, 'a', 'z', 26 + 1);
    tmp3 = translate_range(*in, '0', '9', 52 + 1);
    tmp1 = _mm256_or_si256(tmp1, translate_exact(*in, '+', 62 + 1));
    tmp2 = _mm256_or_si256(tmp2, translate_exact(*in, '/', 63 + 1));
    tmp3 = _mm256_or_si256(tmp3, _mm256_or_si256(tmp1, tmp2));

    /*
     * We use 0 to mark decode failures, so everything is decoded to one higher
     * than normal. We'll shift this down now.
     */
    *in = _mm256_sub_epi8(tmp3, _mm256_set1_epi8(1));

    /* If any byte is now zero, we had a decode failure */
    __m256i mask = _mm256_cmpeq_epi8(tmp3, _mm256_set1_epi8(0));
    return _mm256_testz_si256(mask, mask);
}

AWS_ALIGNED_TYPEDEF(uint8_t, aligned256[32], 32);

/*
 * Input: a 256-bit vector, interpreted as 32 * 6-bit values
 * Output: a 256-bit vector, the lower 24 bytes of which contain the packed version of the input
 */
static inline __m256i pack_vec(__m256i in) {
    /*
     * Our basic strategy is to split the input vector into three vectors, for each 6-bit component
     * of each 24-bit group, shift the groups into place, then OR the vectors together. Conveniently,
     * we can do this on a (32 bit) dword-by-dword basis.
     *
     * It's important to note that we're interpreting the vector as being little-endian. That is,
     * on entry, we have dwords that look like this:
     *
     * MSB                                 LSB
     * 00DD DDDD 00CC CCCC 00BB BBBB 00AA AAAA
     *
     * And we want to translate to:
     *
     * MSB                                 LSB
     * 0000 0000 AAAA AABB BBBB CCCC CCDD DDDD
     *
     * After which point we can pack these dwords together to produce our final output.
     */
    __m256i maskA = _mm256_set1_epi32(0xFF); // low bits
    __m256i maskB = _mm256_set1_epi32(0xFF00);
    __m256i maskC = _mm256_set1_epi32(0xFF0000);
    __m256i maskD = _mm256_set1_epi32((int)0xFF000000);

    __m256i bitsA = _mm256_slli_epi32(_mm256_and_si256(in, maskA), 18);
    __m256i bitsB = _mm256_slli_epi32(_mm256_and_si256(in, maskB), 4);
    __m256i bitsC = _mm256_srli_epi32(_mm256_and_si256(in, maskC), 10);
    __m256i bitsD = _mm256_srli_epi32(_mm256_and_si256(in, maskD), 24);

    __m256i dwords = _mm256_or_si256(_mm256_or_si256(bitsA, bitsB), _mm256_or_si256(bitsC, bitsD));
    /*
     * Now we have a series of dwords with empty MSBs.
     * We need to pack them together (and shift down) with a shuffle operation.
     * Unfortunately the shuffle operation operates independently within each 128-bit lane,
     * so we'll need to do this in two steps: First we compact dwords within each lane, then
     * we do a dword shuffle to compact the two lanes together.

     * 15 14 13 12 11 10 09 08 07 06 05 04 03 02 01 00 <- byte index (little endian)
     * -- 09 0a 0b -- 06 07 08 -- 03 04 05 -- 00 01 02 <- data index
     *
     * We also reverse the order of 3-byte fragments within each lane; we've constructed
     * those fragments in little endian but the order of fragments within the overall
     * vector is in memory order (big endian)
     */
    const aligned256 shufvec_buf = {
        /* clang-format off */
        /* MSB */
        0xFF, 0xFF, 0xFF, 0xFF, /* Zero out the top 4 bytes of the lane */
        2,  1,  0,
        6,  5,  4,
        10,  9,  8,
        14, 13, 12,

        0xFF, 0xFF, 0xFF, 0xFF, /* Zero out the top 4 bytes of the lane */
        2,  1,  0,
        6,  5,  4,
        10,  9,  8,
        14, 13, 12
        /* LSB */
        /* clang-format on */
    };
    __m256i shufvec = _mm256_load_si256((__m256i const *)&shufvec_buf);

    dwords = _mm256_shuffle_epi8(dwords, shufvec);
    /*
     * Now shuffle the 32-bit words:
     * A B C 0 D E F 0 -> 0 0 A B C D E F
     */
    __m256i shuf32 = _mm256_set_epi32(0, 0, 7, 6, 5, 3, 2, 1);

    dwords = _mm256_permutevar8x32_epi32(dwords, shuf32);

    return dwords;
}

static inline bool decode(const unsigned char *in, unsigned char *out) {
    __m256i vec = _mm256_loadu_si256((__m256i const *)in);
    if (!decode_vec(&vec)) {
        return false;
    }
    vec = pack_vec(vec);

    /*
     * We'll do overlapping writes to get both the low 128 bits and the high 64-bits written.
     * Input (memory order): 0 1 2 3 4 5 - - (dwords)
     * Input (little endian) - - 5 4 3 2 1 0
     * Output in memory:
     * [0 1 2 3] [4 5]
     */
    __m128i lo = _mm256_extracti128_si256(vec, 0);
    /*
     * Unfortunately some compilers don't support _mm256_extract_epi64,
     * so we'll just copy right out of the vector as a fallback
     */

#ifdef AWS_HAVE_MM256_EXTRACT_EPI64
    uint64_t hi = _mm256_extract_epi64(vec, 2);
    const uint64_t *p_hi = &hi;
#else
    const uint64_t *p_hi = (uint64_t *)&vec + 2;
#endif

    _mm_storeu_si128((__m128i *)out, lo);
    memcpy(out + 16, p_hi, sizeof(*p_hi));

    return true;
}

size_t aws_common_private_base64_decode_sse41(const unsigned char *in, unsigned char *out, size_t len) {
    if (len % 4) {
        return SIZE_MAX;
    }

    size_t outlen = 0;
    while (len > 32) {
        if (!decode(in, out)) {
            return SIZE_MAX;
        }
        len -= 32;
        in += 32;
        out += 24;
        outlen += 24;
    }

    if (len > 0) {
        unsigned char tmp_in[32];
        unsigned char tmp_out[24];

        memset(tmp_out, 0xEE, sizeof(tmp_out));

        /* We need to ensure the vector contains valid b64 characters */
        memset(tmp_in, 'A', sizeof(tmp_in));
        memcpy(tmp_in, in, len);

        size_t final_out = (3 * len) / 4;

        /* Check for end-of-string padding (up to 2 characters) */
        for (int i = 0; i < 2; i++) {
            if (tmp_in[len - 1] == '=') {
                tmp_in[len - 1] = 'A'; /* make sure the inner loop doesn't bail out */
                len--;
                final_out--;
            }
        }

        if (!decode(tmp_in, tmp_out)) {
            return SIZE_MAX;
        }

        /* Check that there are no trailing ones bits */
        for (size_t i = final_out; i < sizeof(tmp_out); i++) {
            if (tmp_out[i]) {
                return SIZE_MAX;
            }
        }

        memcpy(out, tmp_out, final_out);
        outlen += final_out;
    }
    return outlen;
}

/***** Encode logic *****/
static inline __m256i encode_chars(__m256i in) {
    __m256i tmp1, tmp2, tmp3;

    /*
     * Base64 encoding table, see RFC4648
     *
     * We again use fan-in for the ORs here.
     */
    tmp1 = translate_range(in, 0, 25, 'A');
    tmp2 = translate_range(in, 26, 26 + 25, 'a');
    tmp3 = translate_range(in, 52, 61, '0');
    tmp1 = _mm256_or_si256(tmp1, translate_exact(in, 62, '+'));
    tmp2 = _mm256_or_si256(tmp2, translate_exact(in, 63, '/'));

    return _mm256_or_si256(tmp3, _mm256_or_si256(tmp1, tmp2));
}

/*
 * Input: A 256-bit vector, interpreted as 24 bytes (LSB) plus 8 bytes of high-byte padding
 * Output: A 256-bit vector of base64 characters
 */
static inline __m256i encode_stride(__m256i vec) {
    /*
     * First, since byte-shuffle operations operate within 128-bit subvectors, swap around the dwords
     * to balance the amount of actual data between 128-bit subvectors.
     * After this we want the LE representation to look like: -- XX XX XX -- XX XX XX
     */
    __m256i shuf32 = _mm256_set_epi32(7, 5, 4, 3, 6, 2, 1, 0);
    vec = _mm256_permutevar8x32_epi32(vec, shuf32);

    /*
     * Next, within each group of 3 bytes, we need to byteswap into little endian form so our bitshifts
     * will work properly. We also shuffle around so that each dword has one 3-byte group, plus one byte
     * (MSB) of zero-padding.
     * Because this is a byte-shuffle, indexes are within each 128-bit subvector.
     *
     * -- -- -- -- 11 10 09 08 07 06 05 04 03 02 01 00
     */

    const aligned256 shufvec_buf = {
        /* clang-format off */
        /* MSB */
        2, 1, 0, 0xFF,
        5, 4, 3, 0xFF,
        8, 7, 6, 0xFF,
        11, 10, 9, 0xFF,

        2, 1, 0, 0xFF,
        5, 4, 3, 0xFF,
        8, 7, 6, 0xFF,
        11, 10, 9, 0xFF
        /* LSB */
        /* clang-format on */
    };
    vec = _mm256_shuffle_epi8(vec, _mm256_load_si256((__m256i const *)&shufvec_buf));

    /*
     * Now shift and mask to split out 6-bit groups.
     * We'll also do a second byteswap to get back into big-endian
     */
    __m256i mask0 = _mm256_set1_epi32(0x3F);
    __m256i mask1 = _mm256_set1_epi32(0x3F << 6);
    __m256i mask2 = _mm256_set1_epi32(0x3F << 12);
    __m256i mask3 = _mm256_set1_epi32(0x3F << 18);

    __m256i digit0 = _mm256_and_si256(mask0, vec);
    __m256i digit1 = _mm256_and_si256(mask1, vec);
    __m256i digit2 = _mm256_and_si256(mask2, vec);
    __m256i digit3 = _mm256_and_si256(mask3, vec);

    /*
     * Because we want to byteswap, the low-order digit0 goes into the
     * high-order byte
     */
    digit0 = _mm256_slli_epi32(digit0, 24);
    digit1 = _mm256_slli_epi32(digit1, 10);
    digit2 = _mm256_srli_epi32(digit2, 4);
    digit3 = _mm256_srli_epi32(digit3, 18);

    vec = _mm256_or_si256(_mm256_or_si256(digit0, digit1), _mm256_or_si256(digit2, digit3));

    /* Finally translate to the base64 character set */
    return encode_chars(vec);
}

void aws_common_private_base64_encode_sse41(const uint8_t *input, uint8_t *output, size_t inlen) {
    __m256i instride, outstride;

    while (inlen >= 32) {
        /*
         * Where possible, we'll load a full vector at a time and ignore the over-read.
         * However, if we have < 32 bytes left, this would result in a potential read
         * of unreadable pages, so we use bounce buffers below.
         */
        instride = _mm256_loadu_si256((__m256i const *)input);
        outstride = encode_stride(instride);
        _mm256_storeu_si256((__m256i *)output, outstride);

        input += 24;
        output += 32;
        inlen -= 24;
    }

    while (inlen) {
        /*
         * We need to go through a bounce buffer for anything remaining, as we
         * don't want to over-read or over-write the ends of the buffers.
         */
        size_t stridelen = inlen > 24 ? 24 : inlen;
        size_t outlen = ((stridelen + 2) / 3) * 4;

        memset(&instride, 0, sizeof(instride));
        memcpy(&instride, input, stridelen);

        outstride = encode_stride(instride);
        memcpy(output, &outstride, outlen);

        if (inlen < 24) {
            if (inlen % 3 >= 1) {
                /* AA== or AAA= */
                output[outlen - 1] = '=';
            }
            if (inlen % 3 == 1) {
                /* AA== */
                output[outlen - 2] = '=';
            }

            return;
        }

        input += stridelen;
        output += outlen;
        inlen -= stridelen;
    }
}
