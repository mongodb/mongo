/*-
 * Public Domain 2014-present MongoDB, Inc.
 * Public Domain 2008-2014 WiredTiger, Inc.
 *
 * This is free and unencumbered software released into the public domain.
 *
 * Anyone is free to copy, modify, publish, use, compile, sell, or
 * distribute this software, either in source code form or as a compiled
 * binary, for any purpose, commercial or non-commercial, and by any
 * means.
 *
 * In jurisdictions that recognize copyright laws, the author or authors
 * of this software dedicate any and all copyright interest in the
 * software to the public domain. We make this dedication for the benefit
 * of the public at large and to the detriment of our heirs and
 * successors. We intend this dedication to be an overt act of
 * relinquishment in perpetuity of all present and future rights to this
 * software under copyright law.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */
#include "test_util.h"

/*
 * testutil_random_from_random --
 *     Seed a destination random number generator from a source random number generator. The source
 *     generator's state is advanced.
 */
void
testutil_random_from_random(WT_RAND_STATE *dest, WT_RAND_STATE *src)
{
    testutil_random_from_seed(dest, __wt_random(src));
}

/*
 * testutil_random_from_seed --
 *     Seed a random number generator from a single seed value.
 */
void
testutil_random_from_seed(WT_RAND_STATE *rnd, uint64_t seed)
{
    uint32_t lower, upper;

    /*
     * Our random number generator has two parts that operate independently. We need to seed both
     * with a non-zero value to get the maximum variation. We may be called with a seed that is less
     * than 2^32, so we need to work with zeroes in one half of our 64 bit seed.
     */
    lower = seed & 0xffffffff;
    upper = seed >> 32;

    rnd->x.w = (lower == 0 ? upper : lower);
    rnd->x.z = (upper == 0 ? lower : upper);
}

/*
 * testutil_random_init --
 *     Initialize the Nth random number generator from the seed. If the seed is not yet set, get a
 *     random seed. The random seed is always returned.
 */
void
testutil_random_init(WT_RAND_STATE *rnd, uint64_t *seedp, uint32_t n)
{
    uint32_t shift;

    if (*seedp == 0) {
        /*
         * We'd like to seed our random generator with a 3 byte value. This gives us plenty of
         * variation for testing, but yet makes the seed more convenient for human use. We generate
         * an initial "random" seed that we can then manipulate.
         *
         * However, the initial "random" seed is not random with respect to time, because it is
         * based on the system clock. Successive calls to this function may yield the same clock
         * time on some systems, and that leaves us with the same random seed. So we factor in a "n"
         * value from the caller to get up to 4 different random seeds.
         */
        __wt_random_init_seed(NULL, rnd);
        shift = 8 * (n % 4);
        *seedp = ((rnd->v >> shift) & 0xffffff);
    }
    testutil_random_from_seed(rnd, *seedp);
}
