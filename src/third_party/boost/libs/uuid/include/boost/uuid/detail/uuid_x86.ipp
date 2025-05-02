/*
 *        Copyright Andrey Semashev 2013, 2022, 2024.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          https://www.boost.org/LICENSE_1_0.txt)
 */
/*!
 * \file   uuid/detail/uuid_x86.ipp
 *
 * \brief  This header contains optimized SSE implementation of \c boost::uuid operations.
 */

#ifndef BOOST_UUID_DETAIL_UUID_X86_IPP_INCLUDED_
#define BOOST_UUID_DETAIL_UUID_X86_IPP_INCLUDED_

#include <boost/uuid/detail/endian.hpp>
#include <cstdint>

#if defined(BOOST_UUID_REPORT_IMPLEMENTATION)
#include <boost/config/pragma_message.hpp>

#if defined(BOOST_UUID_USE_AVX10_1)
BOOST_PRAGMA_MESSAGE( "Using uuid_x86.ipp, AVX10.1" )

#elif defined(BOOST_UUID_USE_SSE41)
BOOST_PRAGMA_MESSAGE( "Using uuid_x86.ipp, SSE4.1" )

#elif defined(BOOST_UUID_USE_SSE3)
BOOST_PRAGMA_MESSAGE( "Using uuid_x86.ipp, SSE3" )

#else
BOOST_PRAGMA_MESSAGE( "Using uuid_x86.ipp, SSE2" )

#endif
#endif // #if defined(BOOST_UUID_REPORT_IMPLEMENTATION)

// MSVC does not always have immintrin.h (at least, not up to MSVC 10), so include the appropriate header for each instruction set
#if defined(BOOST_UUID_USE_AVX10_1)
#include <immintrin.h>
#elif defined(BOOST_UUID_USE_SSE41)
#include <smmintrin.h>
#elif defined(BOOST_UUID_USE_SSE3)
#include <pmmintrin.h>
#else
#include <emmintrin.h>
#endif

namespace boost {
namespace uuids {
namespace detail {

BOOST_FORCEINLINE __m128i load_unaligned_si128(const std::uint8_t* p) noexcept
{
    return _mm_loadu_si128(reinterpret_cast< const __m128i* >(p));
}

BOOST_FORCEINLINE void compare(uuid const& lhs, uuid const& rhs, std::uint32_t& cmp, std::uint32_t& rcmp) noexcept
{
    __m128i mm_left = uuids::detail::load_unaligned_si128(lhs.data);
    __m128i mm_right = uuids::detail::load_unaligned_si128(rhs.data);

    // To emulate lexicographical_compare behavior we have to perform two comparisons - the forward and reverse one.
    // Then we know which bytes are equivalent and which ones are different, and for those different the comparison results
    // will be opposite. Then we'll be able to find the first differing comparison result (for both forward and reverse ways),
    // and depending on which way it is for, this will be the result of the operation. There are a few notes to consider:
    //
    // 1. Due to little endian byte order the first bytes go into the lower part of the xmm registers,
    //    so the comparison results in the least significant bits will actually be the most signigicant for the final operation result.
    //    This means we have to determine which of the comparison results have the least significant bit on, and this is achieved with
    //    the "(x - 1) ^ x" trick. With BMI, this will produce a single blsmsk instruction.
    // 2. Because there is only signed byte comparison until AVX-512, we have to invert byte comparison results whenever signs of the
    //    corresponding bytes are different. I.e. in signed comparison it's -1 < 1, but in unsigned it is the opposite (255 > 1). To do
    //    that we XOR left and right, making the most significant bit of each byte 1 if the signs are different, and later apply this mask
    //    with another XOR to the comparison results.
    // 3. Until AVX-512, there is only pcmpgtb instruction that compares for "greater" relation, so we swap the arguments to get what we need.

#if defined(BOOST_UUID_USE_AVX10_1)

    __mmask16 k_cmp = _mm_cmplt_epu8_mask(mm_left, mm_right);
    __mmask16 k_rcmp = _mm_cmplt_epu8_mask(mm_right, mm_left);

    cmp = static_cast< std::uint32_t >(_cvtmask16_u32(k_cmp));
    rcmp = static_cast< std::uint32_t >(_cvtmask16_u32(k_rcmp));

#else // defined(BOOST_UUID_USE_AVX10_1)

    const __m128i mm_signs_mask = _mm_xor_si128(mm_left, mm_right);

    __m128i mm_cmp = _mm_cmpgt_epi8(mm_right, mm_left), mm_rcmp = _mm_cmpgt_epi8(mm_left, mm_right);

    mm_cmp = _mm_xor_si128(mm_signs_mask, mm_cmp);
    mm_rcmp = _mm_xor_si128(mm_signs_mask, mm_rcmp);

    cmp = static_cast< std::uint32_t >(_mm_movemask_epi8(mm_cmp));
    rcmp = static_cast< std::uint32_t >(_mm_movemask_epi8(mm_rcmp));

#endif // defined(BOOST_UUID_USE_AVX10_1)

    cmp = (cmp - 1u) ^ cmp;
    rcmp = (rcmp - 1u) ^ rcmp;
}

} // namespace detail

inline bool uuid::is_nil() const noexcept
{
    __m128i mm = uuids::detail::load_unaligned_si128(data);
#if defined(BOOST_UUID_USE_SSE41)
    return _mm_test_all_zeros(mm, mm) != 0;
#else
    mm = _mm_cmpeq_epi32(mm, _mm_setzero_si128());
    return _mm_movemask_epi8(mm) == 0xFFFF;
#endif
}

inline void uuid::swap(uuid& rhs) noexcept
{
    __m128i mm_this = uuids::detail::load_unaligned_si128(data);
    __m128i mm_rhs = uuids::detail::load_unaligned_si128(rhs.data);
    _mm_storeu_si128(reinterpret_cast< __m128i* >(rhs.data+0), mm_this);
    _mm_storeu_si128(reinterpret_cast< __m128i* >(data+0), mm_rhs);
}

inline bool operator== (uuid const& lhs, uuid const& rhs) noexcept
{
    __m128i mm_left = uuids::detail::load_unaligned_si128(lhs.data);
    __m128i mm_right = uuids::detail::load_unaligned_si128(rhs.data);

#if defined(BOOST_UUID_USE_SSE41)
    __m128i mm = _mm_xor_si128(mm_left, mm_right);
    return _mm_test_all_zeros(mm, mm) != 0;
#else
    __m128i mm_cmp = _mm_cmpeq_epi32(mm_left, mm_right);
    return _mm_movemask_epi8(mm_cmp) == 0xFFFF;
#endif
}

inline bool operator< (uuid const& lhs, uuid const& rhs) noexcept
{
    std::uint32_t cmp, rcmp;
    uuids::detail::compare(lhs, rhs, cmp, rcmp);
    return cmp < rcmp;
}

#if defined(BOOST_UUID_HAS_THREE_WAY_COMPARISON)

inline std::strong_ordering operator<=> (uuid const& lhs, uuid const& rhs) noexcept
{
    std::uint32_t cmp, rcmp;
    uuids::detail::compare(lhs, rhs, cmp, rcmp);
    return cmp <=> rcmp;
}

#endif

} // namespace uuids
} // namespace boost

#endif // BOOST_UUID_DETAIL_UUID_X86_IPP_INCLUDED_
