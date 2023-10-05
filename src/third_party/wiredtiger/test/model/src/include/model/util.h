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

#ifndef MODEL_UTIL_H
#define MODEL_UTIL_H

#include <stdexcept>
#include <string>
#include "wiredtiger.h"

namespace model {

/*
 * wiredtiger_exception --
 *     A WiredTiger exception.
 */
class wiredtiger_exception : std::runtime_error {

public:
    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_exception(WT_SESSION *session, const char *message, int error) noexcept
        : std::runtime_error(std::string(message) + session->strerror(session, error)),
          _error(error)
    {
    }

    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception.
     */
    inline wiredtiger_exception(WT_SESSION *session, int error) noexcept
        : std::runtime_error(session->strerror(session, error)), _error(error)
    {
    }

    /*
     * wiredtiger_exception::wiredtiger_exception --
     *     Create a new instance of the exception. This constructor is not thread-safe.
     */
    inline wiredtiger_exception(int error) noexcept
        : std::runtime_error(wiredtiger_strerror(error)), _error(error)
    {
    }

    /*
     * wiredtiger_exception::error --
     *     Get the error code.
     */
    inline int
    error() const noexcept
    {
        return _error;
    }

private:
    int _error;
};

/*
 * wiredtiger_cursor_guard --
 *     Automatically close the cursor on delete.
 */
class wiredtiger_cursor_guard {

public:
    /*
     * wiredtiger_cursor_guard::wiredtiger_cursor_guard --
     *     Create a new instance of the guard.
     */
    inline wiredtiger_cursor_guard(WT_CURSOR *cursor) noexcept : _cursor(cursor){};

    /*
     * wiredtiger_cursor_guard::~wiredtiger_cursor_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_cursor_guard()
    {
        if (_cursor != nullptr)
            (void)_cursor->close(_cursor);
    }

private:
    WT_CURSOR *_cursor;
};

/*
 * wiredtiger_session_guard --
 *     Automatically close the session on delete.
 */
class wiredtiger_session_guard {

public:
    /*
     * wiredtiger_session_guard::wiredtiger_session_guard --
     *     Create a new instance of the guard.
     */
    inline wiredtiger_session_guard(WT_SESSION *session) noexcept : _session(session){};

    /*
     * wiredtiger_session_guard::~wiredtiger_session_guard --
     *     Destroy the guard.
     */
    inline ~wiredtiger_session_guard()
    {
        if (_session != nullptr)
            (void)_session->close(_session, nullptr);
    }

private:
    WT_SESSION *_session;
};

} /* namespace model */
#endif
