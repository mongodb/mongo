/*
 * Distributed under the Boost Software License, Version 1.0.
 * (See accompanying file LICENSE_1_0.txt or copy at
 * http://www.boost.org/LICENSE_1_0.txt)
 *
 * Copyright (c) 2020 Andrey Semashev
 */
/*!
 * \file   find_address_sse2.cpp
 *
 * This file contains SSE2 implementation of the \c find_address algorithm
 */

#include <boost/predef/architecture/x86.h>
#include <boost/atomic/detail/int_sizes.hpp>

#if BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)

#include <cstddef>
#include <emmintrin.h>

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

#if BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8
namespace {

BOOST_FORCEINLINE __m128i mm_pand_si128(__m128i mm1, __m128i mm2)
{
    // As of 2020, gcc, clang and icc prefer to generate andps instead of pand if the surrounding
    // instructions pertain to FP domain, even if we use the _mm_and_si128 intrinsic. In our
    // algorithm implementation, the FP instruction happen to be shufps, which is not actually
    // restricted to FP domain (it is actually implemented in a separate MMX EU in Pentium 4 or
    // a shuffle EU in INT domain in Core 2; on AMD K8/K10 all SSE instructions are implemented in
    // FADD, FMUL and FMISC EUs regardless of INT/FP data types, and shufps is implemented in FADD/FMUL).
    // In other words, there should be no domain bypass penalty between shufps and pand.
    //
    // This would usually not pose a problem since andps and pand have the same latency and throughput
    // on most architectures of that age (before SSE4.1). However, it is possible that a newer architecture
    // runs the SSE2 code path (e.g. because some weird compiler doesn't support SSE4.1 or because
    // a hypervisor blocks SSE4.1 detection), and there pand may have a better throughput. For example,
    // Sandy Bridge can execute 3 pand instructions per cycle, but only one andps. For this reason
    // we prefer to generate pand and not andps.
#if defined(__GNUC__)
#if defined(__AVX__)
    // Generate VEX-coded variant if the code is compiled for AVX and later.
    __asm__("vpand %1, %0, %0\n\t" : "+x" (mm1) : "x" (mm2));
#else
    __asm__("pand %1, %0\n\t" : "+x" (mm1) : "x" (mm2));
#endif
#else
    mm1 = _mm_and_si128(mm1, mm2);
#endif
    return mm1;
}

} // namespace
#endif // BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8

//! SSE2 implementation of the \c find_address algorithm
std::size_t find_address_sse2(const volatile void* addr, const volatile void* const* addrs, std::size_t size)
{
#if BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8

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

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi32(mm2, mm_addr);
        mm3 = _mm_cmpeq_epi32(mm3, mm_addr);
        mm4 = _mm_cmpeq_epi32(mm4, mm_addr);
        mm5 = _mm_cmpeq_epi32(mm5, mm_addr);
        mm6 = _mm_cmpeq_epi32(mm6, mm_addr);
        mm7 = _mm_cmpeq_epi32(mm7, mm_addr);
        mm8 = _mm_cmpeq_epi32(mm8, mm_addr);

        __m128i mm_mask1_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask1_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i mm_mask2_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask2_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i mm_mask3_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm5), _mm_castsi128_ps(mm6), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask3_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm5), _mm_castsi128_ps(mm6), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i mm_mask4_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm7), _mm_castsi128_ps(mm8), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask4_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm7), _mm_castsi128_ps(mm8), _MM_SHUFFLE(3, 1, 3, 1)));

        mm_mask1_lo = mm_pand_si128(mm_mask1_lo, mm_mask1_hi);
        mm_mask2_lo = mm_pand_si128(mm_mask2_lo, mm_mask2_hi);
        mm_mask3_lo = mm_pand_si128(mm_mask3_lo, mm_mask3_hi);
        mm_mask4_lo = mm_pand_si128(mm_mask4_lo, mm_mask4_hi);

        mm_mask1_lo = _mm_packs_epi32(mm_mask1_lo, mm_mask2_lo);
        mm_mask3_lo = _mm_packs_epi32(mm_mask3_lo, mm_mask4_lo);

        mm_mask1_lo = _mm_packs_epi16(mm_mask1_lo, mm_mask3_lo);

        uint32_t mask = _mm_movemask_epi8(mm_mask1_lo);
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

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi32(mm2, mm_addr);
        mm3 = _mm_cmpeq_epi32(mm3, mm_addr);
        mm4 = _mm_cmpeq_epi32(mm4, mm_addr);

        __m128i mm_mask1_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask1_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(3, 1, 3, 1)));

        __m128i mm_mask2_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask2_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm3), _mm_castsi128_ps(mm4), _MM_SHUFFLE(3, 1, 3, 1)));

        mm_mask1_lo = mm_pand_si128(mm_mask1_lo, mm_mask1_hi);
        mm_mask2_lo = mm_pand_si128(mm_mask2_lo, mm_mask2_hi);

        mm_mask1_lo = _mm_packs_epi32(mm_mask1_lo, mm_mask2_lo);

        uint32_t mask = _mm_movemask_epi8(mm_mask1_lo);
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

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi32(mm2, mm_addr);

        __m128i mm_mask1_lo = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(2, 0, 2, 0)));
        __m128i mm_mask1_hi = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm1), _mm_castsi128_ps(mm2), _MM_SHUFFLE(3, 1, 3, 1)));

        mm_mask1_lo = mm_pand_si128(mm_mask1_lo, mm_mask1_hi);

        uint32_t mask = _mm_movemask_ps(_mm_castsi128_ps(mm_mask1_lo));
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

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        __m128i mm_mask = _mm_shuffle_epi32(mm1, _MM_SHUFFLE(2, 3, 0, 1));
        mm_mask = mm_pand_si128(mm_mask, mm1);

        uint32_t mask = _mm_movemask_pd(_mm_castsi128_pd(mm_mask));
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask);
            goto done;
        }

        pos += 2u;
    }

done:
    return pos;

#else // BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8

    if (size < 10u)
        return find_address_generic(addr, addrs, size);

    const __m128i mm_addr = _mm_set1_epi32((uintptr_t)addr);
    std::size_t pos = 0u;
    const std::size_t n = (size + 3u) & ~static_cast< std::size_t >(3u);
    for (std::size_t m = n & ~static_cast< std::size_t >(15u); pos < m; pos += 16u)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));
        __m128i mm2 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 4u));
        __m128i mm3 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 8u));
        __m128i mm4 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 12u));

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi32(mm2, mm_addr);
        mm3 = _mm_cmpeq_epi32(mm3, mm_addr);
        mm4 = _mm_cmpeq_epi32(mm4, mm_addr);

        mm1 = _mm_packs_epi32(mm1, mm2);
        mm3 = _mm_packs_epi32(mm3, mm4);

        mm1 = _mm_packs_epi16(mm1, mm3);

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
        __m128i mm2 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos + 4u));

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);
        mm2 = _mm_cmpeq_epi32(mm2, mm_addr);

        mm1 = _mm_packs_epi32(mm1, mm2);

        uint32_t mask = _mm_movemask_epi8(mm1);
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask) / 2u;
            goto done;
        }

        pos += 8u;
    }

    if (pos < n)
    {
        __m128i mm1 = _mm_load_si128(reinterpret_cast< const __m128i* >(addrs + pos));

        mm1 = _mm_cmpeq_epi32(mm1, mm_addr);

        uint32_t mask = _mm_movemask_ps(_mm_castsi128_ps(mm1));
        if (mask)
        {
            pos += atomics::detail::count_trailing_zeros(mask);
            goto done;
        }

        pos += 4u;
    }

done:
    return pos;

#endif // BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8
}

} // namespace detail
} // namespace atomics
} // namespace boost

#include <boost/atomic/detail/footer.hpp>

#endif // BOOST_ARCH_X86 && defined(BOOST_ATOMIC_DETAIL_SIZEOF_POINTER) && (BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 8 || BOOST_ATOMIC_DETAIL_SIZEOF_POINTER == 4)
