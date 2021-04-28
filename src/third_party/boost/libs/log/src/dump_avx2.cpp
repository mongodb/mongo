/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   dump_avx2.cpp
 * \author Andrey Semashev
 * \date   05.05.2013
 *
 * \brief  This header is the Boost.Log library implementation, see the library documentation
 *         at http://www.boost.org/doc/libs/release/libs/log/doc/html/index.html.
 */

// NOTE: You should generally avoid including headers as much as possible here, because this file
//       is compiled with special compiler options, and any included header may result in generation of
//       unintended code with these options and violation of ODR.
#include <boost/log/detail/config.hpp>
#include <ostream>
#include <immintrin.h>
#include <boost/cstdint.hpp>
#include <boost/log/detail/header.hpp>

#if defined(__x86_64) || defined(__x86_64__) || \
    defined(__amd64__) || defined(__amd64) || \
    defined(_M_X64)
#define BOOST_LOG_AUX_X86_64
#endif

namespace boost {

BOOST_LOG_OPEN_NAMESPACE

namespace aux {

extern const char g_hex_char_table[2][16];

template< typename CharT >
extern void dump_data_generic(const void* data, std::size_t size, std::basic_ostream< CharT >& strm);

BOOST_LOG_ANONYMOUS_NAMESPACE {

enum
{
    packs_per_stride = 32,
    stride = packs_per_stride * 32
};

union ymm_constant
{
    uint8_t as_bytes[32];
    __m256i as_mm;

    BOOST_FORCEINLINE operator __m256i () const { return as_mm; }
};

static const ymm_constant mm_shuffle_pattern1 = {{ 0x80, 0, 1, 0x80, 2, 3, 0x80, 4, 5, 0x80, 6, 7, 0x80, 8, 9, 0x80,       0x80, 0, 1, 0x80, 2, 3, 0x80, 4, 5, 0x80, 6, 7, 0x80, 8, 9, 0x80 }};
static const ymm_constant mm_shuffle_pattern2 = {{ 0, 1, 0x80, 2, 3, 0x80, 4, 5, 0x80, 6, 7, 0x80, 8, 9, 0x80, 10,         0, 1, 0x80, 2, 3, 0x80, 4, 5, 0x80, 6, 7, 0x80, 8, 9, 0x80, 10 }};
static const ymm_constant mm_shuffle_pattern3 = {{ 5, 0x80, 6, 7, 0x80, 8, 9, 0x80, 10, 11, 0x80, 12, 13, 0x80, 14, 15,    5, 0x80, 6, 7, 0x80, 8, 9, 0x80, 10, 11, 0x80, 12, 13, 0x80, 14, 15 }};
static const ymm_constant mm_shuffle_pattern13 = {{ 0x80, 0, 1, 0x80, 2, 3, 0x80, 4, 5, 0x80, 6, 7, 0x80, 8, 9, 0x80,      5, 0x80, 6, 7, 0x80, 8, 9, 0x80, 10, 11, 0x80, 12, 13, 0x80, 14, 15 }};

#if defined(BOOST_LOG_AUX_X86_64)

// x86-64 architecture has more registers which we can utilize to pass constants
#define BOOST_LOG_AUX_MM_CONSTANT_ARGS_DECL __m256i mm_15, __m256i mm_9, __m256i mm_char_0, __m256i mm_char_space,
#define BOOST_LOG_AUX_MM_CONSTANT_ARGS mm_15, mm_9, mm_char_0, mm_char_space,
#define BOOST_LOG_AUX_MM_CONSTANTS \
    const __m256i mm_15 = _mm256_set1_epi32(0x0F0F0F0F);\
    const __m256i mm_9 = _mm256_set1_epi32(0x09090909);\
    const __m256i mm_char_0 = _mm256_set1_epi32(0x30303030);\
    const __m256i mm_char_space = _mm256_set1_epi32(0x20202020);

#else

// MSVC in 32-bit mode is not able to pass all constants to dump_pack, and is also not able to align them on the stack, so we have to fetch them from global constants
static const ymm_constant mm_15 = {{ 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F,   0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F, 0x0F }};
static const ymm_constant mm_9 = {{ 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09,   0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09, 0x09 }};
static const ymm_constant mm_char_0 = {{ 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30,   0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30, 0x30 }};
static const ymm_constant mm_char_space = {{ 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20,   0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20, 0x20 }};
#define BOOST_LOG_AUX_MM_CONSTANT_ARGS_DECL
#define BOOST_LOG_AUX_MM_CONSTANT_ARGS
#define BOOST_LOG_AUX_MM_CONSTANTS

#endif

/*!
 * \brief Dumps a pack of input data into a string of 8 bit ASCII characters.
 *
 * The composed string is placed as follows (in Intel notation): mm_output1[127:0], mm_output2[127:0], mm_output3[127:0], mm_output1[255:128], mm_output2[255:128], mm_output3[255:128].
 */
static BOOST_FORCEINLINE void dump_pack
(
    BOOST_LOG_AUX_MM_CONSTANT_ARGS_DECL
    __m256i mm_char_10_to_a, __m256i mm_input,
    __m256i& mm_output1, __m256i& mm_output2, __m256i& mm_output3
)
{
    // Split half-bytes
    __m256i mm_input_hi = _mm256_and_si256(_mm256_srli_epi16(mm_input, 4), mm_15);
    __m256i mm_input_lo = _mm256_and_si256(mm_input, mm_15);

    // Stringize each of the halves
    __m256i mm_addend_hi = _mm256_cmpgt_epi8(mm_input_hi, mm_9);
    __m256i mm_addend_lo = _mm256_cmpgt_epi8(mm_input_lo, mm_9);
    mm_addend_hi = _mm256_and_si256(mm_char_10_to_a, mm_addend_hi);
    mm_addend_lo = _mm256_and_si256(mm_char_10_to_a, mm_addend_lo);

    mm_input_hi = _mm256_add_epi8(mm_input_hi, mm_char_0);
    mm_input_lo = _mm256_add_epi8(mm_input_lo, mm_char_0);

    mm_input_hi = _mm256_add_epi8(mm_input_hi, mm_addend_hi);
    mm_input_lo = _mm256_add_epi8(mm_input_lo, mm_addend_lo);

    // Join them back together
    __m256i mm_1 = _mm256_unpacklo_epi8(mm_input_hi, mm_input_lo);
    __m256i mm_2 = _mm256_unpackhi_epi8(mm_input_hi, mm_input_lo);

    // Insert spaces between stringized bytes:
    // |0123456789abcdef|0123456789abcdef|
    // | 01 23 45 67 89 |ab cd ef 01 23 4|5 67 89 ab cd ef|
    __m256i mm_out1 = _mm256_shuffle_epi8(mm_1, mm_shuffle_pattern1.as_mm);
    __m256i mm_out3 = _mm256_shuffle_epi8(mm_2, mm_shuffle_pattern3.as_mm);
    __m256i mm_out2 = _mm256_shuffle_epi8(_mm256_alignr_epi8(mm_2, mm_1, 10), mm_shuffle_pattern2.as_mm);

    mm_output1 = _mm256_max_epu8(mm_out1, mm_char_space);
    mm_output2 = _mm256_max_epu8(mm_out2, mm_char_space);
    mm_output3 = _mm256_max_epu8(mm_out3, mm_char_space);
}

//! Dumps a pack of input data into a string of 8 bit ASCII characters
static BOOST_FORCEINLINE void dump_pack
(
    BOOST_LOG_AUX_MM_CONSTANT_ARGS_DECL
    __m256i mm_char_10_to_a, __m128i mm_input,
    __m128i& mm_output1, __m128i& mm_output2, __m128i& mm_output3
)
{
    // Split half-bytes
    __m128i mm_input_hi = _mm_srli_epi16(mm_input, 4);
    __m256i mm = _mm256_inserti128_si256(_mm256_castsi128_si256(_mm_unpacklo_epi8(mm_input_hi, mm_input)), _mm_unpackhi_epi8(mm_input_hi, mm_input), 1);
    mm = _mm256_and_si256(mm, mm_15);

    // Stringize the halves
    __m256i mm_addend = _mm256_cmpgt_epi8(mm, mm_9);
    mm_addend = _mm256_and_si256(mm_char_10_to_a, mm_addend);

    mm = _mm256_add_epi8(mm, mm_char_0);
    mm = _mm256_add_epi8(mm, mm_addend);

    // Insert spaces between stringized bytes:
    __m256i mm_out13 = _mm256_shuffle_epi8(mm, mm_shuffle_pattern13.as_mm);
    __m128i mm_out2 = _mm_shuffle_epi8(_mm_alignr_epi8(_mm256_extracti128_si256(mm, 1), _mm256_castsi256_si128(mm), 10), _mm256_castsi256_si128(mm_shuffle_pattern2.as_mm));

    mm_out13 = _mm256_max_epu8(mm_out13, mm_char_space);
    mm_output2 = _mm_max_epu8(mm_out2, _mm256_castsi256_si128(mm_char_space));
    mm_output1 = _mm256_castsi256_si128(mm_out13);
    mm_output3 = _mm256_extracti128_si256(mm_out13, 1);
}

template< typename CharT >
BOOST_FORCEINLINE void store_characters(__m128i mm_chars, CharT* buf)
{
    switch (sizeof(CharT))
    {
    case 1:
        _mm_store_si128(reinterpret_cast< __m128i* >(buf), mm_chars);
        break;

    case 2:
        _mm256_store_si256(reinterpret_cast< __m256i* >(buf), _mm256_cvtepu8_epi16(mm_chars));
        break;

    case 4:
        {
            __m128i mm = _mm_unpackhi_epi64(mm_chars, mm_chars);
            _mm256_store_si256(reinterpret_cast< __m256i* >(buf), _mm256_cvtepu8_epi32(mm_chars));
            _mm256_store_si256(reinterpret_cast< __m256i* >(buf) + 1, _mm256_cvtepu8_epi32(mm));
        }
        break;
    }
}

template< typename CharT >
BOOST_FORCEINLINE void store_characters_x3(__m256i mm_chars1, __m256i mm_chars2, __m256i mm_chars3, CharT* buf)
{
    store_characters(_mm256_castsi256_si128(mm_chars1), buf);
    store_characters(_mm256_castsi256_si128(mm_chars2), buf + 16);
    store_characters(_mm256_castsi256_si128(mm_chars3), buf + 32);
    store_characters(_mm256_extracti128_si256(mm_chars1, 1), buf + 48);
    store_characters(_mm256_extracti128_si256(mm_chars2, 1), buf + 64);
    store_characters(_mm256_extracti128_si256(mm_chars3, 1), buf + 80);
}

template< typename CharT >
BOOST_FORCEINLINE void dump_data_avx2(const void* data, std::size_t size, std::basic_ostream< CharT >& strm)
{
    typedef CharT char_type;

    char_type buf_storage[stride * 3u + 32u];
    // Align the temporary buffer at 32 bytes
    char_type* const buf = reinterpret_cast< char_type* >((uint8_t*)buf_storage + (32u - (((uintptr_t)(char_type*)buf_storage) & 31u)));
    char_type* buf_begin = buf + 1u; // skip the first space of the first chunk
    char_type* buf_end = buf + stride * 3u;

    __m256i mm_char_10_to_a;
    if (strm.flags() & std::ios_base::uppercase)
        mm_char_10_to_a = _mm256_set1_epi32(0x07070707); // '9' is 0x39 and 'A' is 0x41 in ASCII, so we have to add 0x07 to 0x3A to get uppercase letters
    else
        mm_char_10_to_a = _mm256_set1_epi32(0x27272727); // ...and 'a' is 0x61, which means we have to add 0x27 to 0x3A to get lowercase letters

    // First, check the input alignment. Also, if we can dump the whole data in one go, do it right away. It turns out to be faster than splitting
    // the work between prealign and tail part. It is also a fairly common case since on most platforms memory is not aligned to 32 bytes (i.e. prealign is often needed).
    const uint8_t* p = static_cast< const uint8_t* >(data);
    const std::size_t prealign_size = size == 32u ? static_cast< std::size_t >(32u) : static_cast< std::size_t >((32u - ((uintptr_t)p & 31u)) & 31u);
    if (prealign_size)
    {
        __m256i mm_input = _mm256_lddqu_si256(reinterpret_cast< const __m256i* >(p));
        BOOST_LOG_AUX_MM_CONSTANTS

        __m256i mm_output1, mm_output2, mm_output3;
        dump_pack(BOOST_LOG_AUX_MM_CONSTANT_ARGS mm_char_10_to_a, mm_input, mm_output1, mm_output2, mm_output3);

        store_characters_x3(mm_output1, mm_output2, mm_output3, buf);

        _mm256_zeroall(); // need to zero all ymm registers to avoid register spills/restores the compler generates around the function call
        strm.write(buf_begin, prealign_size * 3u - 1u);

        buf_begin = buf;
        size -= prealign_size;
        p += prealign_size;
    }

    const std::size_t stride_count = size / stride;
    std::size_t tail_size = size % stride;
    for (std::size_t i = 0; i < stride_count; ++i)
    {
        char_type* b = buf;
        BOOST_LOG_AUX_MM_CONSTANTS

        for (unsigned int j = 0; j < packs_per_stride; ++j, b += 3u * 32u, p += 32u)
        {
            __m256i mm_input = _mm256_load_si256(reinterpret_cast< const __m256i* >(p));
            __m256i mm_output1, mm_output2, mm_output3;
            dump_pack(BOOST_LOG_AUX_MM_CONSTANT_ARGS mm_char_10_to_a, mm_input, mm_output1, mm_output2, mm_output3);

            store_characters_x3(mm_output1, mm_output2, mm_output3, b);
        }

        _mm256_zeroall(); // need to zero all ymm registers to avoid register spills/restores the compler generates around the function call
        strm.write(buf_begin, buf_end - buf_begin);
        buf_begin = buf;
    }

    if (BOOST_UNLIKELY(tail_size > 0))
    {
        char_type* b = buf;
        while (tail_size >= 16u)
        {
            __m128i mm_input = _mm_load_si128(reinterpret_cast< const __m128i* >(p));
            BOOST_LOG_AUX_MM_CONSTANTS

            __m128i mm_output1, mm_output2, mm_output3;
            dump_pack(BOOST_LOG_AUX_MM_CONSTANT_ARGS mm_char_10_to_a, mm_input, mm_output1, mm_output2, mm_output3);

            store_characters(mm_output1, b);
            store_characters(mm_output2, b + 16u);
            store_characters(mm_output3, b + 32u);

            b += 3u * 16u;
            p += 16u;
            tail_size -= 16u;
        }

        _mm256_zeroall(); // need to zero all ymm registers to avoid register spills/restores the compler generates around the function call
        const char* const char_table = g_hex_char_table[(strm.flags() & std::ios_base::uppercase) != 0];
        for (unsigned int i = 0; i < tail_size; ++i, ++p, b += 3u)
        {
            uint32_t n = *p;
            b[0] = static_cast< char_type >(' ');
            b[1] = static_cast< char_type >(char_table[n >> 4]);
            b[2] = static_cast< char_type >(char_table[n & 0x0F]);
        }

        strm.write(buf_begin, b - buf_begin);
    }
}

} // namespace

void dump_data_char_avx2(const void* data, std::size_t size, std::basic_ostream< char >& strm)
{
    if (size >= 32)
    {
        dump_data_avx2(data, size, strm);
    }
    else
    {
        dump_data_generic(data, size, strm);
    }
}

void dump_data_wchar_avx2(const void* data, std::size_t size, std::basic_ostream< wchar_t >& strm)
{
    if (size >= 32)
    {
        dump_data_avx2(data, size, strm);
    }
    else
    {
        dump_data_generic(data, size, strm);
    }
}

#if !defined(BOOST_NO_CXX11_CHAR16_T)
void dump_data_char16_avx2(const void* data, std::size_t size, std::basic_ostream< char16_t >& strm)
{
    if (size >= 32)
    {
        dump_data_avx2(data, size, strm);
    }
    else
    {
        dump_data_generic(data, size, strm);
    }
}
#endif

#if !defined(BOOST_NO_CXX11_CHAR32_T)
void dump_data_char32_avx2(const void* data, std::size_t size, std::basic_ostream< char32_t >& strm)
{
    if (size >= 32)
    {
        dump_data_avx2(data, size, strm);
    }
    else
    {
        dump_data_generic(data, size, strm);
    }
}
#endif

} // namespace aux

BOOST_LOG_CLOSE_NAMESPACE // namespace log

} // namespace boost

#include <boost/log/detail/footer.hpp>
