/*
 *          Copyright Andrey Semashev 2007 - 2015.
 * Distributed under the Boost Software License, Version 1.0.
 *    (See accompanying file LICENSE_1_0.txt or copy at
 *          http://www.boost.org/LICENSE_1_0.txt)
 */

#include <immintrin.h>
#include <stdio.h>

int main(int, char*[])
{
    __m256i mm = _mm256_setzero_si256();
    fread(&mm, 1u, sizeof(mm), stdin);
    mm = _mm256_shuffle_epi8(_mm256_alignr_epi8(mm, mm, 10), mm);
    fwrite(&mm, 1u, sizeof(mm), stdout);
    _mm256_zeroupper();
    return 0;
}
