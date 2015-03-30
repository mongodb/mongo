// status_with.h

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

#include <iosfwd>
#include <type_traits>
#include <utility>

#include "mongo/base/status.h"

namespace mongo {

    /**
     * StatusWith is used to return an error or a value.
     * This class is designed to make exception-free code cleaner by not needing as many out
     * parameters.
     *
     * Example:
     * StatusWith<int> fib( int n ) {
     *   if ( n < 0 ) 
     *       return StatusWith<int>( ErrorCodes::BadValue, "paramter to fib has to be >= 0" );
     *   if ( n <= 1 ) return StatusWith<int>( 1 );
     *   StatusWith<int> a = fib( n - 1 );
     *   StatusWith<int> b = fib( n - 2 );
     *   if ( !a.isOK() ) return a;
     *   if ( !b.isOK() ) return b;
     *   return StatusWith<int>( a.getValue() + b.getValue() );
     * }
     */
    template<typename T>
    class StatusWith {
        static_assert(!(std::is_same<T, mongo::Status>::value), "StatusWith<Status> is banned.");
    public:

        /**
         * for the error case
         */
        StatusWith( ErrorCodes::Error code, const std::string& reason, int location = 0 )
            : _status( Status( code, reason, location ) ) {
        }

        /**
         * for the error case
         */
        StatusWith( const Status& status )
            : _status( status ) {
        }

        /**
         * for the OK case
         */
        StatusWith(T t)
            : _status(Status::OK()), _t(std::move(t)) {
        }

#if defined(_MSC_VER) && _MSC_VER < 1900
        StatusWith(const StatusWith& s)
            : _status(s._status), _t(s._t) {
        }

        StatusWith(StatusWith&& s)
            : _status(std::move(s._status)), _t(std::move(s._t)) {
        }

        StatusWith& operator=(const StatusWith& other) {
            _status = other._status;
            _t = other._t;
            return *this;
        }

        StatusWith& operator=(StatusWith&& other) {
            _status = std::move(other._status);
            _t = std::move(other._t);
            return *this;
        }
#endif

        const T& getValue() const {
            // TODO dassert( isOK() );
            return _t;
        }

        T& getValue() {
            // TODO dassert( isOK() );
            return _t;
        }

        const Status& getStatus() const {
            return _status;
        }

        bool isOK() const {
            return _status.isOK();
        }

    private:
        Status _status;
        T _t;
    };

    template<typename T, typename... Args>
    StatusWith<T> makeStatusWith(Args&&... args) {
        return StatusWith<T>{T(std::forward<Args>(args)...)};
    }

    template<typename T>
    std::ostream& operator<<(std::ostream& stream, const StatusWith<T>& sw) {
        if (sw.isOK())
            return stream << sw.getValue();
        return stream << sw.getStatus();
    }

    //
    // EqualityComparable(StatusWith<T>, T). Intentionally not providing an ordering relation.
    //

    template<typename T>
    bool operator==(const StatusWith<T>& sw, const T& val) {
        return sw.isOK() && sw.getValue() == val;
    }

    template<typename T>
    bool operator==(const T& val, const StatusWith<T>& sw) {
        return sw.isOK() && val == sw.getValue();
    }

    template<typename T>
    bool operator!=(const StatusWith<T>& sw, const T& val) {
        return !(sw == val);
    }

    template<typename T>
    bool operator!=(const T& val, const StatusWith<T>& sw) {
        return !(val == sw);
    }

    //
    // EqualityComparable(StatusWith<T>, Status)
    //

    template<typename T>
    bool operator==(const StatusWith<T>& sw, const Status& status) {
        return sw.getStatus() == status;
    }

    template<typename T>
    bool operator==(const Status& status, const StatusWith<T>& sw) {
        return status == sw.getStatus();
    }

    template<typename T>
    bool operator!=(const StatusWith<T>& sw, const Status& status) {
        return !(sw == status);
    }

    template<typename T>
    bool operator!=(const Status& status, const StatusWith<T>& sw) {
        return !(status == sw);
    }

    //
    // EqualityComparable(StatusWith<T>, ErrorCode)
    //

    template<typename T>
    bool operator==(const StatusWith<T>& sw, const ErrorCodes::Error code) {
        return sw.getStatus() == code;
    }

    template<typename T>
    bool operator==(const ErrorCodes::Error code, const StatusWith<T>& sw) {
        return code == sw.getStatus();
    }

    template<typename T>
    bool operator!=(const StatusWith<T>& sw, const ErrorCodes::Error code) {
        return !(sw == code);
    }

    template<typename T>
    bool operator!=(const ErrorCodes::Error code, const StatusWith<T>& sw) {
        return !(code == sw);
    }

} // namespace mongo
