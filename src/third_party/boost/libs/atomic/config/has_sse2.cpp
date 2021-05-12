/*
 *             Copyright Andrey Semashev 2020.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <emmintrin.h>

int main(int, char*[])
{
    __m128i mm = _mm_setzero_si128();
    mm = _mm_cmpeq_epi32(mm, mm);
    mm = _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(mm), _mm_castsi128_ps(mm), _MM_SHUFFLE(2, 0, 2, 0)));
    mm = _mm_packs_epi32(mm, mm);
    return _mm_movemask_epi8(mm);
}
