/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   find_address_sse41.cpp
 *
 * This file contains SSE4.1 implementation of the \c find_address algorithm
 */

#include <boost/predef/architecture/x86.h>
#include <boost/atomic/detail/int_sizes.hpp>

#if BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8)

#include <cstddef>
#include <smmintrin.h>

#include <boost/cstdint.hpp>
#include <boost/atomic/detail/config.hpp>
#include <boost/atomic/detail/intptr.hpp>
#include "find_address.hpp"
#include "x86_vector_tools.hpp"
#include "bit_operation_tools.hpp"

#include <boost/atomic/detail/header.hpp>

namespace boost {
namespace atomics {
namespace detail {

//! SSE4.1 implementation of the \c find_address algorithm
std::size_t find_address_sse41(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
    if (size < 12u)
        return find_address_generic(addr, addrs, size);

    const __m128i mm_addr = mm_set1_epiptr((uintptr_t)addr);
    std::size_t pos = 0u;
    const std::size_t n = (size + 1u) & ~static_cast< std::size_t >(1u);
    for (std::size_t m = n & ~static_cast< std::size_t >(15u); pos < m; pos += 16u)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));
        __m128i mm2 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 2u));
        __m128i mm3 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 4u));
        __m128i mm4 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 6u));
        __m128i mm5 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 8u));
        __m128i mm6 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 10u));
        __m128i mm7 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 12u));
        __m128i mm8 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 14u));

        mm1 = _mm_cmpeq_epi64(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi64(mm2, mm_addr);
        mm3 = _mm_cmpeq_epi64(mm3, mm_addr);
        mm4 = _mm_cmpeq_epi64(mm4, mm_addr);
        mm5 = _mm_cmpeq_epi64(mm5, mm_addr);
        mm6 = _mm_cmpeq_epi64(mm6, mm_addr);
        mm7 = _mm_cmpeq_epi64(mm7, mm_addr);
        mm8 = _mm_cmpeq_epi64(mm8, mm_addr);

        mm1 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));
        mm3 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(2, 0, 2, 0)));
        mm5 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm5), _mm_castsi128_ps(mm6), _MM_SHUFFLE(2, 0, 2, 0)));
        mm7 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm7), _mm_castsi128_ps(mm8), _MM_SHUFFLE(2, 0, 2, 0)));

        mm1 = _mm_packs_epi32(mm1, mm3);
        mm5 = _mm_packs_epi32(mm5, mm7);

        mm1 = _mm_packs_epi16(mm1, mm5);

        uint32_t mask = _mm_movemask_epi8(mm1);
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask);
            goto done;
        }
    }

    if ((n - pos) >= 8u)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));
        __m128i mm2 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 2u));
        __m128i mm3 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 4u));
        __m128i mm4 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 6u));

        mm1 = _mm_cmpeq_epi64(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi64(mm2, mm_addr);
        mm3 = _mm_cmpeq_epi64(mm3, mm_addr);
        mm4 = _mm_cmpeq_epi64(mm4, mm_addr);

        mm1 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));
        mm3 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(2, 0, 2, 0)));

        mm1 = _mm_packs_epi32(mm1, mm3);

        uint32_t mask = _mm_movemask_epi8(mm1);
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask) / 2u;
            goto done;
        }

        pos += 8u;
    }

    if ((n - pos) >= 4u)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));
        __m128i mm2 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 2u));

        mm1 = _mm_cmpeq_epi64(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi64(mm2, mm_addr);

        mm1 = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));

        uint32_t mask = _mm_movemask_ps(_mm_castsi128_ps(mm1));
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask);
            goto done;
        }

        pos += 4u;
    }

    if (pos < n)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));

        mm1 = _mm_cmpeq_epi64(mm1, mm_addr);
        uint32_t mask = _mm_movemask_pd(_mm_castsi128_pd(mm1));
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask);
            goto done;
        }

        pos += 2u;
    }

done:
    return pos;
}

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8)
