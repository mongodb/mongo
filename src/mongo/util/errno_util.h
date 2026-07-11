// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#pragma once

#include "mongo/util/modules.h"

#include <cerrno>
#include <cstdlib>
#include <string>
#include <system_error>
#include <utility>


namespace [[MONGO_MOD_PUBLIC]] mongo {

#ifdef _WIN32
namespace errno_util_win32_detail {
int gle();
int wsaGle();
}  // namespace errno_util_win32_detail
#endif

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
    return systemError(errno_util_win32_detail::gle());
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
 *     `fmt::format("Unknown error {}", ec.value())`
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
    return systemError(errno_util_win32_detail::wsaGle());
#else
    return lastSystemError();
#endif
}

}  // namespace mongo
