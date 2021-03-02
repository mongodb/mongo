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

#ifndef RANDOM_GENERATOR_H
#define RANDOM_GENERATOR_H

#include <algorithm>
#include <cstddef>
#include <random>
#include <string>

namespace test_harness {

/* Helper class to generate random values. */
class random_generator {
    public:
    /* No copies of the singleton allowed. */
    random_generator(random_generator const &) = delete;
    void operator=(random_generator const &) = delete;

    static random_generator &
    get_instance()
    {
        static random_generator _instance;
        return _instance;
    }

    std::string
    generate_string(std::size_t length)
    {
        std::string random_string;

        for (std::size_t i = 0; i < length; ++i)
            random_string += _characters[_distribution(_generator)];

        return (random_string);
    }

    private:
    random_generator()
    {
        _generator = std::mt19937(std::random_device{}());
        _distribution = std::uniform_int_distribution<>(0, _characters.size() - 1);
    }

    std::mt19937 _generator;
    std::uniform_int_distribution<> _distribution;
    const std::string _characters =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
};
} // namespace test_harness

#endif
