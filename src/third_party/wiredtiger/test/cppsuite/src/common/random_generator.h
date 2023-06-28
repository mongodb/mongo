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

/* Following definitions are required in order to use printing format specifiers in C++. */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <random>
#include <string>

namespace test_harness {
/* Helper class to generate random values using uniform distributions. */

enum class characters_type { PSEUDO_ALPHANUMERIC, ALPHABET };

class random_generator {
public:
    static random_generator &instance();

    /* No copies of the singleton allowed. */
    random_generator(random_generator const &) = delete;
    random_generator &operator=(random_generator const &) = delete;

    /* Generate a random string of a given length. */
    std::string generate_random_string(
      std::size_t length, characters_type type = characters_type::PSEUDO_ALPHANUMERIC);

    /*
     * Generate a pseudo random string which compresses better. It should not be used to generate
     * keys due to the limited randomness.
     */
    std::string generate_pseudo_random_string(
      std::size_t length, characters_type type = characters_type::PSEUDO_ALPHANUMERIC);

    /*
     * Generate a boolean with 50/50 probability.
     */
    bool generate_bool();

    /* Generate a random integer between min and max. */
    template <typename T>
    T
    generate_integer(T min, T max)
    {
        std::uniform_int_distribution<T> dis(min, max);
        return dis(_generator);
    }

private:
    random_generator();
    std::uniform_int_distribution<> &get_distribution(characters_type type);
    const std::string &get_characters(characters_type type);

    std::mt19937 _generator;
    std::uniform_int_distribution<> _alphanum_distrib, _alpha_distrib;
    const std::string _alphabet = "abcdefghijklmnopqrstuvwxyz";
    const std::string _pseudo_alphanum =
      "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
};
} // namespace test_harness

#endif
