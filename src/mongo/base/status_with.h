/*    Copyright 2013 10gen Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects
 *    for all of the code used other than as permitted herein. If you modify
 *    file(s) with this exception, you may extend this exception to your
 *    version of the file(s), but you are not obligated to do so. If you do not
 *    wish to do so, delete this exception statement from your version. If you
 *    delete this exception statement from all source files in the program,
 *    then also delete it in the license file.
 */

#pragma once

#include <boost/optional.hpp>
#include <iosfwd>
#include <type_traits>
#include <utility>

#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/platform/compiler.h"

#define MONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "mongo/util/invariant.h"
#undef MONGO_INCLUDE_INVARIANT_H_WHITELISTED


namespace mongo {

template <typename T>
class StatusWith;

template <typename T>
constexpr bool isStatusWith = false;
template <typename T>
constexpr bool isStatusWith<StatusWith<T>> = true;

template <typename T>
constexpr bool isStatusOrStatusWith = std::is_same<T, mongo::Status>::value || isStatusWith<T>;

template <typename T>
using StatusOrStatusWith = std::conditional_t<std::is_void<T>::value, Status, StatusWith<T>>;

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
class MONGO_WARN_UNUSED_RESULT_CLASS StatusWith {
    MONGO_STATIC_ASSERT_MSG(!isStatusOrStatusWith<T>,
                            "StatusWith<Status> and StatusWith<StatusWith<T>> are banned.");

public:
    using value_type = T;

    /**
     * for the error case
     */
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code, StringData reason)
        : _status(code, reason) {}
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code, std::string reason)
        : _status(code, std::move(reason)) {}
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code, const char* reason)
        : _status(code, reason) {}
    MONGO_COMPILER_COLD_FUNCTION StatusWith(ErrorCodes::Error code,
                                            const mongoutils::str::stream& reason)
        : _status(code, reason) {}

    /**
     * for the error case
     */
    MONGO_COMPILER_COLD_FUNCTION StatusWith(Status status) : _status(std::move(status)) {
        dassert(!isOK());
    }

    /**
     * for the OK case
     */
    StatusWith(T t) : _status(Status::OK()), _t(std::move(t)) {}

    const T& getValue() const {
        dassert(isOK());
        return *_t;
    }

    T& getValue() {
        dassert(isOK());
        return *_t;
    }

    const Status& getStatus() const {
        return _status;
    }

    bool isOK() const {
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

private:
    Status _status;
    boost::optional<T> _t;
};

template <typename T, typename... Args>
StatusWith<T> makeStatusWith(Args&&... args) {
    return StatusWith<T>{T(std::forward<Args>(args)...)};
}

template <typename T>
std::ostream& operator<<(std::ostream& stream, const StatusWith<T>& sw) {
    if (sw.isOK())
        return stream << sw.getValue();
    return stream << sw.getStatus();
}

//
// EqualityComparable(StatusWith<T>, T). Intentionally not providing an ordering relation.
//

template <typename T>
bool operator==(const StatusWith<T>& sw, const T& val) {
    return sw.isOK() && sw.getValue() == val;
}

template <typename T>
bool operator==(const T& val, const StatusWith<T>& sw) {
    return sw.isOK() && val == sw.getValue();
}

template <typename T>
bool operator!=(const StatusWith<T>& sw, const T& val) {
    return !(sw == val);
}

template <typename T>
bool operator!=(const T& val, const StatusWith<T>& sw) {
    return !(val == sw);
}

//
// EqualityComparable(StatusWith<T>, Status)
//

template <typename T>
bool operator==(const StatusWith<T>& sw, const Status& status) {
    return sw.getStatus() == status;
}

template <typename T>
bool operator==(const Status& status, const StatusWith<T>& sw) {
    return status == sw.getStatus();
}

template <typename T>
bool operator!=(const StatusWith<T>& sw, const Status& status) {
    return !(sw == status);
}

template <typename T>
bool operator!=(const Status& status, const StatusWith<T>& sw) {
    return !(status == sw);
}

//
// EqualityComparable(StatusWith<T>, ErrorCode)
//

template <typename T>
bool operator==(const StatusWith<T>& sw, const ErrorCodes::Error code) {
    return sw.getStatus() == code;
}

template <typename T>
bool operator==(const ErrorCodes::Error code, const StatusWith<T>& sw) {
    return code == sw.getStatus();
}

template <typename T>
bool operator!=(const StatusWith<T>& sw, const ErrorCodes::Error code) {
    return !(sw == code);
}

template <typename T>
bool operator!=(const ErrorCodes::Error code, const StatusWith<T>& sw) {
    return !(code == sw);
}

}  // namespace mongo
