/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <tmmintrin.h>

void pretend_used(__m128i*);

int main(int, char*[])
{
    __m128i mm = _mm_setzero_si128();
    pretend_used(&mm);
    mm = _mm_shuffle_epi8(_mm_alignr_epi8(mm, mm, 10), mm);
    pretend_used(&mm);
    return 0;
}
