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
#include <string>

#include "bound.h"
#include "src/storage/scoped_cursor.h"

namespace test_harness {
/*
 * bound_set is a basic class that can hold two bounds, an upper and a lower. It also provides a
 * convenient way of creating prefix bounds.
 */
class bound_set {
public:
    /* No default constructor is allowed. */
    bound_set() = delete;
    /* Construct a bound set given two bounds. */
    bound_set(bound lower, bound upper);
    /* Construct a prefix bound set given a key. */
    explicit bound_set(const std::string &key);

    /* Apply the bounds to a cursor. */
    void apply(scoped_cursor &cursor) const;

    /* Basic getters. */
    const bound &get_lower() const;
    const bound &get_upper() const;

private:
    bound _lower;
    bound _upper;
};
} // namespace test_harness
