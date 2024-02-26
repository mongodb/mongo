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

#pragma once

#include <cstddef>
#include "model/core.h"

extern "C" {
#include "wt_internal.h"
}

namespace model {

/*
 * random --
 *     A random number generator.
 */
class random {

public:
    /*
     * random::random --
     *     Create a new instance of the random number generator.
     */
    random(uint64_t seed = 0) noexcept;

    /*
     * random::next_double --
     *     Get the next double between 0 and 1.
     */
    double next_double() noexcept;

    /*
     * random::next_float --
     *     Get the next float between 0 and 1.
     */
    float next_float() noexcept;

    /*
     * random::next_index --
     *     Get the next index from the list of the given length.
     */
    size_t next_index(size_t length);

    /*
     * random::next_uint64 --
     *     Get the next integer.
     */
    inline uint64_t
    next_uint64(uint64_t max)
    {
        return (uint64_t)(next_double() * max);
    }

    /*
     * random::next_uint64 --
     *     Get the next integer.
     */
    inline uint64_t
    next_uint64(uint64_t min, uint64_t max)
    {
        return min + next_uint64(max - min);
    }

private:
    WT_RAND_STATE _random_state;
};

/*
 * probability_switch --
 *     A convenience macro for situations where we need to make different decisions with different
 *     probabilities.
 */
#define probability_switch(r) for (auto __r = (r); __r >= 0; __r = -1)

/*
 * probability_case --
 *     A case within a probability switch.
 */
#define probability_case(p) if (__r >= 0 && (__r -= (p)) < 0)

/*
 * probability_default --
 *     The default case within a probability switch.
 */
#define probability_default if (__r >= 0)

} /* namespace model */
