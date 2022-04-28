/**
 *    Copyright (C) 2018-present MongoDB, Inc.
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the Server Side Public License, version 1,
 *    as published by MongoDB, Inc.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    Server Side Public License for more details.
 *
 *    You should have received a copy of the Server Side Public License
 *    along with this program. If not, see
 *    <http://www.mongodb.com/licensing/server-side-public-license>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the Server Side Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#pragma once

#include <system_error>
#include <utility>

#include "mongo/base/string_data.h"

namespace mongo {

/**
 * Returns category to use for POSIX errno error codes.
 * On POSIX, `errno` codes are the `std::system_category`.
 * On Windows, the `errno` codes are the `std::generic_category`.
 */
inline const std::error_category& posixCategory() {
#ifdef _WIN32
    return std::generic_category();
#else
    return std::system_category();
#endif
}

/** Wraps POSIX `errno` value in an appropriate `std::error_code`. */
inline std::error_code posixError(int e) {
    return std::error_code(e, posixCategory());
}

/** Wraps `e` in a `std::error_code` with `std::system_category`. */
inline std::error_code systemError(int e) {
    return std::error_code(e, std::system_category());
}

/**
 * Returns `{errno, std::generic_category()}`.
 * Windows has both Windows errors and POSIX errors. That is, there's a
 * `GetLastError` and an `errno`. They are tracked separately, and unrelated to
 * each other.
 *
 * In practice, this function is useful only to handle the `errno`-setting POSIX
 * compatibility functions on Windows.
 *
 * On POSIX systems, `std::system_category` is potentially a superset of
 * `std::generic_category`, so `lastSystemError` should be preferred for
 * handling system errors.
 *
 * Guaranteed to not modify `errno`.
 */
inline std::error_code lastPosixError() {
    return posixError(errno);
}

/**
 * On POSIX, returns `{errno, std::system_category()}`.
 * On Windows, returns `{GetLastError(), std::system_category()}`, but see `lastPosixError`.
 *
 * Guaranteed to not modify the system error code variable.
 */
inline std::error_code lastSystemError() {
#ifdef _WIN32
    return systemError(GetLastError());
#else
    return systemError(errno);
#endif
}

/**
 * Returns `ec.message()`, possibly augmented to disambiguate unknowns.
 *
 * In libstdc++, the unknown error messages include the number. Windows and
 * Libc++ do not include it. So if the code is an unknown, it is replaced with the
 * message that libstdc++ would have given, which is the expanded format
 * expression:
 *     `"Unknown error {}"_format(ec.value())`
 */
std::string errorMessage(std::error_code ec);

/**
 * A category for `getaddrinfo` or `getnameinfo` (i.e. the netdb.h library)
 * results. Uses `gai_error` on Unix systems. On Windows, these errors are
 * compatible with the system error space.
 */
const std::error_category& addrInfoCategory();

/** Wrap `e` in a `std::error_code` with `addrInfoCategory`. */
inline std::error_code addrInfoError(int e) {
    return std::error_code(e, addrInfoCategory());
}

/**
 * Portable wrapper for socket API calls. On POSIX platforms this is just
 * `lastSystemError`. On Windows, Winsock API callers must query last error with
 * `WSAGetLastError` instead of `GetLastError`. The Winsock errors can use the
 * same error code category as other Windows API calls.
 */
inline std::error_code lastSocketError() {
#ifdef _WIN32
    return systemError(WSAGetLastError());
#else
    return lastSystemError();
#endif
}

}  // namespace mongo
