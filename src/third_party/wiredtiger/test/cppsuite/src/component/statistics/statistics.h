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

#ifndef STATISTICS_H
#define STATISTICS_H

#include <string>

#include "src/main/configuration.h"
#include "src/storage/scoped_cursor.h"

namespace test_harness {

class statistics {
public:
    statistics() = default;
    statistics(configuration &config, const std::string &stat_name, int stat_field);
    virtual ~statistics() = default;

    /* Check that the statistics are within bounds. */
    virtual void check(scoped_cursor &cursor);

    /* Retrieve the value associated to the stat in a string format. */
    virtual std::string get_value_str(scoped_cursor &cursor);

    /* Getters. */
    int get_field() const;
    int64_t get_max() const;
    int64_t get_min() const;
    const std::string &get_name() const;
    bool get_postrun() const;
    bool get_runtime() const;
    bool get_save() const;

protected:
    int field;
    int64_t max;
    int64_t min;
    std::string name;
    bool postrun;
    bool runtime;
    bool save;
};
} // namespace test_harness

#endif
