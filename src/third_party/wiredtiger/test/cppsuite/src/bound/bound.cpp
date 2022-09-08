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
#include "bound.h"

#include <string>

#include "src/common/constants.h"
#include "src/common/random_generator.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {
bound::bound()
{
    clear();
}

bound::bound(const std::string &key, bool lower_bound, bool inclusive)
    : _key(key), _lower_bound(lower_bound), _inclusive(inclusive)
{
}

bound::bound(uint64_t key_size_max, bool lower_bound) : _lower_bound(lower_bound)
{
    auto key_size =
      random_generator::instance().generate_integer(static_cast<uint64_t>(1), key_size_max);
    _key = random_generator::instance().generate_random_string(key_size, characters_type::ALPHABET);
    _inclusive = random_generator::instance().generate_integer(0, 1);
}

bound::bound(uint64_t key_size_max, bool lower_bound, char start) : bound(key_size_max, lower_bound)
{
    _key[0] = start;
}

std::string
bound::get_config() const
{
    return "bound=" + std::string(_lower_bound ? "lower" : "upper") +
      ",inclusive=" + std::string(_inclusive ? "true" : "false");
}

const std::string &
bound::get_key() const
{
    return _key;
}

bool
bound::get_inclusive() const
{
    return _inclusive;
}

void
bound::apply(scoped_cursor &cursor) const
{
    cursor->set_key(cursor.get(), _key.c_str());
    testutil_check(cursor->bound(cursor.get(), get_config().c_str()));
}

void
bound::clear()
{
    _key.clear();
    _inclusive = false;
    _lower_bound = false;
}
} // namespace test_harness
