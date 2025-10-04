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

#include "scoped_session.h"

extern "C" {
#include "test_util.h"
}

namespace test_harness {
/* scoped_session implementation */
scoped_session::scoped_session(WT_CONNECTION *conn)
{
    reinit(conn);
}

scoped_session::~scoped_session()
{
    if (_session != nullptr) {
        testutil_check(_session->close(_session, nullptr));
        _session = nullptr;
    }
}

scoped_session::scoped_session(scoped_session &&other)
{
    std::swap(_session, other._session);
}

void
scoped_session::close_session()
{
    testutil_check(_session->close(_session, nullptr));
    _session = nullptr;
}

/*
 * Implement move assignment by move constructing a temporary and swapping its internals with the
 * current session. This means that the currently held WT_SESSION will get destroyed as the
 * temporary falls out of the scope and we will steal the one that we're move assigning from.
 */
scoped_session &
scoped_session::operator=(scoped_session &&other)
{
    scoped_session tmp(std::move(other));
    std::swap(_session, tmp._session);
    return (*this);
}

void
scoped_session::reinit(WT_CONNECTION *conn)
{
    if (_session != nullptr) {
        testutil_check(_session->close(_session, nullptr));
        _session = nullptr;
    }
    if (conn != nullptr)
        testutil_check(conn->open_session(conn, nullptr, nullptr, &_session));
}

WT_SESSION &
scoped_session::operator*()
{
    return (*_session);
}

WT_SESSION *
scoped_session::operator->()
{
    return (_session);
}

WT_SESSION *
scoped_session::get()
{
    return (_session);
}

scoped_cursor
scoped_session::open_scoped_cursor(const std::string &uri, const std::string &cfg)
{
    return (scoped_cursor(_session, uri, cfg));
}
} // namespace test_harness
