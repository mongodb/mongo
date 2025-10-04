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

#include "random_generator.h"

#include <algorithm>

extern "C" {
#include "test_util.h"
}

namespace test_harness {
random_generator &
random_generator::instance()
{
    thread_local random_generator _instance;
    return (_instance);
}

std::string
random_generator::generate_random_string(std::size_t length, characters_type type)
{
    const std::string characters = get_characters(type);
    std::string str;

    while (str.size() < length)
        str += characters;

    std::shuffle(str.begin(), str.end(), _generator);
    return (str.substr(0, length));
}

std::string
random_generator::generate_pseudo_random_string(std::size_t length, characters_type type)
{
    std::string random_string;
    std::uniform_int_distribution<> &distribution = get_distribution(type);
    std::size_t start_location = distribution(_generator);
    const std::string &characters = get_characters(type);

    for (std::size_t i = 0; i < length; ++i) {
        random_string += characters[start_location];
        if (start_location == characters.size() - 1)
            start_location = 0;
        else
            start_location++;
    }
    return (random_string);
}

bool
random_generator::generate_bool()
{
    return generate_integer<int>(0, 1) == 1;
}

random_generator::random_generator()
{
    _generator = std::mt19937(std::random_device{}());
    _alphanum_distrib = std::uniform_int_distribution<>(0, _pseudo_alphanum.size() - 1);
    _alpha_distrib = std::uniform_int_distribution<>(0, _alphabet.size() - 1);
}

std::uniform_int_distribution<> &
random_generator::get_distribution(characters_type type)
{
    switch (type) {
    case characters_type::ALPHABET:
        return (_alpha_distrib);
    case characters_type::PSEUDO_ALPHANUMERIC:
        return (_alphanum_distrib);
    }
    testutil_die(EINVAL, "unexpected characters_type: %d", type);
}

const std::string &
random_generator::get_characters(characters_type type)
{
    switch (type) {
    case characters_type::ALPHABET:
        return (_alphabet);
    case characters_type::PSEUDO_ALPHANUMERIC:
        return (_pseudo_alphanum);
    }
    testutil_die(EINVAL, "unexpected characters_type: %d", static_cast<int>(type));
}

} // namespace test_harness
