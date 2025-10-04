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

#include "mongo/base/error_codes.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/compiler.h"
#include "mongo/unittest/stringify.h"
#include "mongo/util/assert_util_core.h"

#include <iosfwd>
#include <string>
#include <type_traits>
#include <utility>

#include <boost/optional.hpp>
#include <boost/optional/optional.hpp>

namespace mongo {

template <typename T>
class StatusWith;

template <typename T>
inline constexpr bool isStatusWith = false;
template <typename T>
inline constexpr bool isStatusWith<StatusWith<T>> = true;

template <typename T>
inline constexpr bool isStatusOrStatusWith = std::is_same_v<T, Status> || isStatusWith<T>;

template <typename T>
using StatusOrStatusWith = std::conditional_t<std::is_void_v<T>, Status, StatusWith<T>>;

/**
 * StatusWith is used to return an error or a value.
 * This class is designed to make exception-free code cleaner by not needing as many out
 * parameters.
 *
 * Example:
 * StatusWith<int> fib( int n ) {
 *   if ( n < 0 )
 *       return StatusWith<int>( ErrorCodes::BadValue, "parameter to fib has to be >= 0" );
 *   if ( n <= 1 ) return StatusWith<int>( 1 );
 *   StatusWith<int> a = fib( n - 1 );
 *   StatusWith<int> b = fib( n - 2 );
 *   if ( !a.isOK() ) return a;
 *   if ( !b.isOK() ) return b;
 *   return StatusWith<int>( a.getValue() + b.getValue() );
 * }
 */
template <typename T>
class [[nodiscard]] StatusWith {
private:
    MONGO_STATIC_ASSERT_MSG(!isStatusOrStatusWith<T>,
                            "StatusWith<Status> and StatusWith<StatusWith<T>> are banned.");

public:
    using value_type = T;

    /**
     * For the error case.
     * As with the `Status` constructors, `reason` can be `std::string` or
     * anything that can construct one (e.g. `StringData`, `str::stream`).
     */
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code, std::string reason)
        : _status(code, std::move(reason)) {}
    template <typename Reason,
              std::enable_if_t<std::is_constructible_v<std::string, Reason&&>, int> = 0>
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code, Reason&& reason)
        : StatusWith(code, std::string{std::forward<Reason>(reason)}) {}

    /**
     * for the error case
     */
    MONGO_COMPILER_COLD_FUNCTION StatusWith(Status status) : _status(std::move(status)) {
        dassert(!isOK());
    }

    /**
     * for the OK case
     */
    constexpr StatusWith(T t) : _status(Status::OK()), _t(std::move(t)) {}

    template <std::convertible_to<T> U>
    requires(!std::is_same_v<U, T>)
    constexpr StatusWith(U&& other) : StatusWith(static_cast<T>(std::forward<U>(other))) {}

    template <std::convertible_to<T> U>
    requires(!std::is_same_v<U, T>)
    constexpr StatusWith(StatusWith<U> other) : _status(std::move(other.getStatus())) {
        if (other.isOK())
            this->_t = std::move(other.getValue());
    }

    constexpr const T& getValue() const {
        dassert(isOK());
        return *_t;
    }

    constexpr T& getValue() {
        dassert(isOK());
        return *_t;
    }

    constexpr const Status& getStatus() const {
        return _status;
    }

    constexpr bool isOK() const {
        return _status.isOK();
    }

    /**
     * This method is a transitional tool, to facilitate transition to compile-time enforced status
     * checking.
     *
     * NOTE: DO NOT ADD NEW CALLS TO THIS METHOD. This method serves the same purpose as
     * `.getStatus().ignore()`; however, it indicates a situation where the code that presently
     * ignores a status code has not been audited for correctness. This method will be removed at
     * some point. If you encounter a compiler error from ignoring the result of a `StatusWith`
     * returning function be sure to check the return value, or deliberately ignore the return
     * value. The function is named to be auditable independently from unaudited `Status` ignore
     * cases.
     */
    void status_with_transitional_ignore() && noexcept {};
    void status_with_transitional_ignore() const& noexcept = delete;

    constexpr bool operator==(const T& val) const {
        return isOK() && getValue() == val;
    }

    constexpr bool operator==(const Status& status) const {
        return getStatus() == status;
    }

    constexpr bool operator==(ErrorCodes::Error code) const {
        return getStatus() == code;
    }

private:
    Status _status;
    // Using std::optional because boost::optional isn't constexpr-friendly.
    std::optional<T> _t;  // NOLINT not used in public API, so no interop issues.
};

template <typename T>
std::string stringify_forTest(const StatusWith<T>& sw) {
    if (sw.isOK()) {
        return unittest::stringify::invoke(sw.getValue());
    } else {
        return unittest::stringify::invoke(sw.getStatus());
    }
}

template <typename T>
auto operator<<(std::ostream& stream, const StatusWith<T>& sw)
    -> decltype(stream << sw.getValue())  // SFINAE on T streamability.
{
    if (sw.isOK())
        return stream << sw.getValue();
    return stream << sw.getStatus();
}

template <typename Allocator, typename T>
auto operator<<(StringBuilderImpl<Allocator>& stream, const StatusWith<T>& sw)
    -> decltype(stream << sw.getValue())  // SFINAE on T streamability.
{
    if (sw.isOK())
        return stream << sw.getValue();
    return stream << sw.getStatus();
}


}  // namespace mongo
