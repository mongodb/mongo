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

#include <iosfwd>
#include <string>

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"

namespace mongoutils {
namespace str {
class stream;
}  // namespace str
}  // namespace mongoutils

namespace mongo {

// Including builder.h here would cause a cycle.
template <typename Allocator>
class StringBuilderImpl;

/**
 * Status represents an error state or the absence thereof.
 *
 * A Status uses the standardized error codes -- from file 'error_codes.err' -- to
 * determine an error's cause. It further clarifies the error with a textual
 * description, and code-specific extra info (a subclass of ErrorExtraInfo).
 */
class MONGO_WARN_UNUSED_RESULT_CLASS Status {
public:
    /**
     * This is the best way to construct an OK status.
     */
    static inline Status OK();

    /**
     * Builds an error status given the error code and a textual description of what
     * caused the error.
     *
     * For OK Statuses prefer using Status::OK(). If code is OK, the remaining arguments are
     * ignored.
     *
     * For adding context to the reason string, use withContext/addContext rather than making a new
     * Status manually.
     *
     * If the status comes from a command reply, use getStatusFromCommandResult() instead of manual
     * parsing. If the status is round-tripping through non-command BSON, use the constructor that
     * takes a BSONObj so that it can extract the extra info if the code is supposed to have any.
     */
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code, const std::string& reason);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code, const char* reason);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code, StringData reason);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        const mongoutils::str::stream& reason);
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        StringData message,
                                        const BSONObj& extraInfoHolder);

    /**
     * Constructs a Status with a subclass of ErrorExtraInfo.
     */
    template <typename T, typename = stdx::enable_if_t<std::is_base_of<ErrorExtraInfo, T>::value>>
    MONGO_COMPILER_COLD_FUNCTION Status(T&& detail, StringData message)
        : Status(T::code,
                 message,
                 std::make_shared<const std::remove_reference_t<T>>(std::forward<T>(detail))) {
        MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorExtraInfoFor<T::code>, T>());
    }

    inline Status(const Status& other);
    inline Status& operator=(const Status& other);

    inline Status(Status&& other) noexcept;
    inline Status& operator=(Status&& other) noexcept;

    inline ~Status();

    /**
     * Returns a new Status with the same data as this, but with the reason string replaced with
     * newReason.  The new reason is not visible to any other Statuses that share the same ErrorInfo
     * object.
     *
     * No-op when called on an OK status.
     */
    Status withReason(StringData newReason) const;

    /**
     * Returns a new Status with the same data as this, but with the reason string prefixed with
     * reasonPrefix and our standard " :: caused by :: " separator. The new reason is not visible to
     * any other Statuses that share the same ErrorInfo object.
     *
     * No-op when called on an OK status.
     */
    Status withContext(StringData reasonPrefix) const;
    void addContext(StringData reasonPrefix) {
        *this = this->withContext(reasonPrefix);
    }

    /**
     * Only compares codes. Ignores reason strings.
     */
    bool operator==(const Status& other) const {
        return code() == other.code();
    }
    bool operator!=(const Status& other) const {
        return !(*this == other);
    }

    /**
     * Compares this Status's code with an error code.
     */
    bool operator==(const ErrorCodes::Error other) const {
        return code() == other;
    }
    bool operator!=(const ErrorCodes::Error other) const {
        return !(*this == other);
    }

    //
    // accessors
    //

    inline bool isOK() const;

    inline ErrorCodes::Error code() const;

    inline std::string codeString() const;


    /**
     * Returns the reason string or the empty string if isOK().
     */
    const std::string& reason() const {
        if (_error)
            return _error->reason;

        static const std::string empty;
        return empty;
    }

    /**
     * Returns the generic ErrorExtraInfo if present.
     */
    const ErrorExtraInfo* extraInfo() const {
        return isOK() ? nullptr : _error->extra.get();
    }

    /**
     * Returns a specific subclass of ErrorExtraInfo if the error code matches that type.
     */
    template <typename T>
    const T* extraInfo() const {
        MONGO_STATIC_ASSERT(std::is_base_of<ErrorExtraInfo, T>());
        MONGO_STATIC_ASSERT(std::is_same<error_details::ErrorExtraInfoFor<T::code>, T>());

        if (isOK())
            return nullptr;
        if (code() != T::code)
            return nullptr;

        // Can't use checked_cast due to include cycle.
        invariant(_error->extra);
        dassert(dynamic_cast<const T*>(_error->extra.get()));
        return static_cast<const T*>(_error->extra.get());
    }

    std::string toString() const;

    /**
     * Returns true if this Status's code is a member of the given category.
     */
    template <ErrorCategory category>
    bool isA() const {
        return ErrorCodes::isA<category>(code());
    }

    /**
     * Call this method to indicate that it is your intention to ignore a returned status.
     */
    void ignore() const noexcept {}

    /**
     * This method is a transitional tool, to facilitate transition to compile-time enforced status
     * checking.
     *
     * NOTE: DO NOT ADD NEW CALLS TO THIS METHOD. This method serves the same purpose as
     * `.ignore()`; however, it indicates a situation where the code that presently ignores a status
     * code has not been audited for correctness. This method will be removed at some point. If you
     * encounter a compiler error from ignoring the result of a status-returning function be sure to
     * check the return value, or deliberately ignore the return value.
     */
    void transitional_ignore() && noexcept {};
    void transitional_ignore() const& noexcept = delete;

    //
    // Below interface used for testing code only.
    //

    inline unsigned refCount() const;

private:
    // Private since it could result in a type mismatch between code and extraInfo.
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        StringData reason,
                                        std::shared_ptr<const ErrorExtraInfo>);
    inline Status();

    struct ErrorInfo {
        AtomicWord<unsigned> refs;     // reference counter
        const ErrorCodes::Error code;  // error code
        const std::string reason;      // description of error cause
        const std::shared_ptr<const ErrorExtraInfo> extra;

        static ErrorInfo* create(ErrorCodes::Error code,
                                 StringData reason,
                                 std::shared_ptr<const ErrorExtraInfo> extra);

        ErrorInfo(ErrorCodes::Error code, StringData reason, std::shared_ptr<const ErrorExtraInfo>);
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

std::ostream& operator<<(std::ostream& os, const Status& status);

// This is only implemented for StringBuilder, not StackStringBuilder.
template <typename Allocator>
StringBuilderImpl<Allocator>& operator<<(StringBuilderImpl<Allocator>& os, const Status& status);

}  // namespace mongo

#include "mongo/base/status-inl.h"
