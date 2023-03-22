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

#ifndef SCOPED_CURSOR_H
#define SCOPED_CURSOR_H

/* Following definitions are required in order to use printing format specifiers in C++. */
#ifndef __STDC_LIMIT_MACROS
#define __STDC_LIMIT_MACROS
#endif
#ifndef __STDC_FORMAT_MACROS
#define __STDC_FORMAT_MACROS
#endif

#include <string>

extern "C" {
#include "wiredtiger.h"
}

namespace test_harness {
class scoped_cursor {
public:
    scoped_cursor() = default;
    scoped_cursor(WT_SESSION *session, const std::string &uri, const std::string &cfg);

    /* Moving is ok but copying is not. */
    scoped_cursor(scoped_cursor &&other);

    ~scoped_cursor();

    scoped_cursor &operator=(scoped_cursor &&other);
    scoped_cursor(const scoped_cursor &) = delete;
    scoped_cursor &operator=(const scoped_cursor &) = delete;

    void reinit(WT_SESSION *session, const std::string &uri, const std::string &cfg);

    WT_CURSOR &operator*();
    WT_CURSOR *operator->();

    WT_CURSOR *get();

private:
    WT_CURSOR *_cursor = nullptr;
};
} // namespace test_harness
#endif
