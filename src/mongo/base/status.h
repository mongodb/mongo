/*    Copyright 2012 10gen Inc.
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

#include <boost/config.hpp>
#include <iosfwd>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/platform/atomic_word.h"

namespace mongoutils {
namespace str {
class stream;
}
}

namespace mongo {

/**
 * Status represents an error state or the absence thereof.
 *
 * A Status uses the standardized error codes -- from file 'error_codes.h' -- to
 * determine an error's cause. It further clarifies the error with a textual
 * description. Optionally, a Status may also have an error location number, which
 * should be a unique, grep-able point in the code base (including assert numbers).
 *
 * Example usage:
 *
 *    Status sumAB(int a, int b, int* c) {
 *       if (overflowIfSum(a,b)) {
 *           return Status(ErrorCodes::ERROR_OVERFLOW, "overflow in sumAB", 16494);
 *       }
 *
 *       *c = a+b;
 *       return Status::OK();
 *   }
 *
 * TODO: expand base/error_codes.h to capture common errors in current code
 * TODO: generate base/error_codes.h out of a description file
 * TODO: check 'location' duplicates against assert numbers
 */
class Status {
public:
    // Short-hand for returning an OK status.
    static inline Status OK();

    /**
     * Builds an error status given the error code, a textual description of what
     * caused the error, and a unique position in the where the error occurred
     * (similar to an assert number).
     *
     * For OK Statuses prefer using Status::OK(). If code is OK, the remaining arguments are
     * ignored.
     */
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        std::string reason,
                                        int location = 0);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        const char* reason,
                                        int location = 0);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        const mongoutils::str::stream& reason,
                                        int location = 0);

    inline Status(const Status& other);
    inline Status& operator=(const Status& other);

    inline Status(Status&& other) BOOST_NOEXCEPT;
    inline Status& operator=(Status&& other) BOOST_NOEXCEPT;

    inline ~Status();

    /**
     * Returns true if 'other's error code and location are equal/different to this
     * instance's. Otherwise returns false.
     */
    bool compare(const Status& other) const;
    bool operator==(const Status& other) const;
    bool operator!=(const Status& other) const;

    /**
     * Returns true if 'other's error code is equal/different to this instance's.
     * Otherwise returns false.
     */
    bool compareCode(const ErrorCodes::Error other) const;
    bool operator==(const ErrorCodes::Error other) const;
    bool operator!=(const ErrorCodes::Error other) const;

    //
    // accessors
    //

    inline bool isOK() const;

    inline ErrorCodes::Error code() const;

    inline std::string codeString() const;

    inline std::string reason() const;

    inline int location() const;

    std::string toString() const;

    //
    // Below interface used for testing code only.
    //

    inline AtomicUInt32::WordType refCount() const;

private:
    inline Status();

    struct ErrorInfo {
        AtomicUInt32 refs;             // reference counter
        const ErrorCodes::Error code;  // error code
        const std::string reason;      // description of error cause
        const int location;            // unique location of the triggering line in the code

        static ErrorInfo* create(ErrorCodes::Error code, std::string reason, int location);

        ErrorInfo(ErrorCodes::Error code, std::string reason, int location);
    };

    ErrorInfo* _error;

    /**
     * Increment/Decrement the reference counter inside an ErrorInfo
     *
     * @param error  ErrorInfo to be incremented
     */
    static inline void ref(ErrorInfo* error);
    static inline void unref(ErrorInfo* error);
};

inline bool operator==(const ErrorCodes::Error lhs, const Status& rhs);

inline bool operator!=(const ErrorCodes::Error lhs, const Status& rhs);

//
// Convenience method for unittest code. Please use accessors otherwise.
//

std::ostream& operator<<(std::ostream& os, const Status& status);
std::ostream& operator<<(std::ostream& os, ErrorCodes::Error);

}  // namespace mongo

#include "mongo/base/status-inl.h"
