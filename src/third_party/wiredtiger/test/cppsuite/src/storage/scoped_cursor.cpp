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

#include "scoped_cursor.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {

/* scoped_cursor implementation */
scoped_cursor::scoped_cursor(WT_SESSION *session, const std::string &uri, const std::string &cfg)
{
    reinit(session, uri, cfg);
}

scoped_cursor::scoped_cursor(scoped_cursor &&other)
{
    std::swap(_cursor, other._cursor);
}

scoped_cursor::~scoped_cursor()
{
    if (_cursor != nullptr) {
        testutil_check(_cursor->close(_cursor));
        _cursor = nullptr;
    }
}

/*
 * Implement move assignment by move constructing a temporary and swapping its internals with the
 * current cursor. This means that the currently held WT_CURSOR will get destroyed as the temporary
 * falls out of the scope and we will steal the one that we're move assigning from.
 */
scoped_cursor &
scoped_cursor::operator=(scoped_cursor &&other)
{
    scoped_cursor tmp(std::move(other));
    std::swap(_cursor, tmp._cursor);
    return (*this);
}

void
scoped_cursor::reinit(WT_SESSION *session, const std::string &uri, const std::string &cfg)
{
    testutil_assert(!uri.empty());
    if (_cursor != nullptr) {
        testutil_check(_cursor->close(_cursor));
        _cursor = nullptr;
    }
    if (session != nullptr)
        testutil_check(session->open_cursor(
          session, uri.c_str(), nullptr, cfg.empty() ? nullptr : cfg.c_str(), &_cursor));
}

/*
 * Override the dereference operators. The idea is that we should able to use this class as if it is
 * a pointer to a WT_CURSOR.
 */
WT_CURSOR &
scoped_cursor::operator*()
{
    return (*_cursor);
}

WT_CURSOR *
scoped_cursor::operator->()
{
    return (_cursor);
}

WT_CURSOR *
scoped_cursor::get()
{
    return (_cursor);
}
} // namespace test_harness
