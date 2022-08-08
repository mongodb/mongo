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
#include "bound_set.h"

#include <string>

#include "bound.h"
extern "C" {
#include "test_util.h"
}

namespace test_harness {
bound_set::bound_set(bound lower, bound upper) : _lower(lower), _upper(upper) {}

bound_set::bound_set(const std::string &key)
{
    _lower = bound(key, true, true);
    std::string key_copy = key;
    key_copy[key_copy.size() - 1]++;
    _upper = bound(key_copy, false, false);
}

void
bound_set::apply(scoped_cursor &cursor) const
{
    cursor->set_key(cursor.get(), _lower.get_key().c_str());
    testutil_check(cursor->bound(cursor.get(), _lower.get_config().c_str()));
    cursor->set_key(cursor.get(), _upper.get_key().c_str());
    testutil_check(cursor->bound(cursor.get(), _upper.get_config().c_str()));
}

const bound &
bound_set::get_lower() const
{
    return _lower;
}

const bound &
bound_set::get_upper() const
{
    return _upper;
}
} // namespace test_harness
