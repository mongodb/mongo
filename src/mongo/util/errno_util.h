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
 */
inline std::error_code lastPosixError() {
    return std::error_code(errno, std::generic_category());
}

/**
 * On POSIX, returns `{errno, std::system_category()}`.
 * On Windows, returns `{GetLastError(), std::system_category()}`, but see `lastPosixError`.
 */
inline std::error_code lastSystemError() {
#ifdef _WIN32
    int e = GetLastError();
#else
    int e = errno;
#endif
    return std::error_code(e, std::system_category());
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

/** A system error code's error message. */
inline std::string errnoWithDescription(int e) {
    return errorMessage(std::error_code{e, std::system_category()});
}

/** The last system error code's error message. */
inline std::string errnoWithDescription() {
    return errorMessage(lastSystemError());
}

}  // namespace mongo
