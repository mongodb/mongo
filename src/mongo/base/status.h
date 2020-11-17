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
#include <type_traits>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>

#include "mongo/base/error_codes.h"
#include "mongo/base/error_extra_info.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/util/builder_fwd.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/static_immortal.h"

#define MONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "mongo/util/invariant.h"
#undef MONGO_INCLUDE_INVARIANT_H_WHITELISTED

namespace mongo {

/**
 * Status represents an error state or the absence thereof.
 *
 * A Status uses the standardized error codes from file "error_codes.yml" to
 * determine an error's cause. It further clarifies the error with a textual
 * description, and code-specific extra info (a subclass of ErrorExtraInfo).
 */
class MONGO_WARN_UNUSED_RESULT_CLASS Status {
public:
    /** This is the best way to construct an OK status. */
    static Status OK() {
        return {};
    }

    /**
     * Builds an error Status given the error code and a textual description of the error.
     *
     * In all Status constructors, the `reason` is natively a `std::string`, but
     * as a convenience it can be given as any type explicitly convertible to
     * `std::string`, such as `const char*`, `StringData`, or `str::stream`, or
     * `std::string_view`.
     *
     * If code is ErrorCodes::OK, the remaining arguments are ignored. Prefer
     * using Status::OK(), to make an OK Status.
     *
     * For adding context to the reason string, use withContext/addContext rather than making a new
     * Status manually, as these functions apply a formatting convention.
     *
     * If the Status comes from a command reply, use getStatusFromCommandResult() instead of manual
     * parsing. If the status is round-tripping through non-command BSON, use the constructor that
     * takes a BSONObj so that it can extract the extra info if the code is supposed to have any.
     */
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code, std::string reason)
        : Status{code, std::move(reason), nullptr} {}

    template <typename Reason,
              std::enable_if_t<std::is_constructible_v<std::string, Reason&&>, int> = 0>
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code, Reason&& reason)
        : Status{code, std::string{std::forward<Reason>(reason)}} {}

    /**
     * Same as above, but with an attached BSON object carrying errorExtraInfo to be parsed.
     * Parse is performed according to `code` and its associated ErrorExtraInfo subclass.
     */
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        std::string reason,
                                        const BSONObj& extraObj)
        : _error{_parseErrorInfo(code, std::move(reason), extraObj)} {}

    template <typename Reason,
              std::enable_if_t<std::is_constructible_v<std::string, Reason&&>, int> = 0>
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        Reason&& reason,
                                        const BSONObj& extraObj)
        : Status{code, std::string{std::forward<Reason>(reason)}, extraObj} {}

    /**
     * Builds a Status with a subclass of ErrorExtraInfo.
     * The Status code is inferred from the static type of the `extra` parameter.
     */
    template <typename Extra, std::enable_if_t<std::is_base_of_v<ErrorExtraInfo, Extra>, int> = 0>
    MONGO_COMPILER_COLD_FUNCTION Status(Extra&& extra, std::string reason)
        : Status{
              Extra::code,
              std::move(reason),
              std::make_shared<const std::remove_reference_t<Extra>>(std::forward<Extra>(extra))} {
        MONGO_STATIC_ASSERT(std::is_same_v<error_details::ErrorExtraInfoFor<Extra::code>, Extra>);
    }

    template <typename Extra,
              typename Reason,
              std::enable_if_t<std::is_base_of_v<ErrorExtraInfo, Extra> &&
                                   std::is_constructible_v<std::string, Reason&&>,
                               int> = 0>
    MONGO_COMPILER_COLD_FUNCTION Status(Extra&& extra, Reason&& reason)
        : Status{std::forward<Extra>(extra), std::string{std::forward<Reason>(reason)}} {}

    /**
     * Returns a new Status with the same data as this, but with the reason string replaced with
     * newReason.  The new reason is not visible to any other Statuses that share the same ErrorInfo
     * object.
     *
     * No-op when called on an OK status.
     */
    Status withReason(std::string newReason) const {
        return isOK() ? OK() : Status(code(), std::move(newReason), _error->extra);
    }

    template <typename Reason,
              std::enable_if_t<std::is_constructible_v<std::string, Reason&&>, int> = 0>
    Status withReason(Reason&& newReason) const {
        return withReason(std::string{std::forward<Reason>(newReason)});
    }

    /**
     * Returns a new Status with the same data as this, but with the reason string prefixed with
     * reasonPrefix and our standard " :: caused by :: " separator. The new reason is not visible to
     * any other Statuses that share the same ErrorInfo object.
     *
     * No-op when called on an OK status.
     */
    Status withContext(StringData reasonPrefix) const;

    void addContext(StringData reasonPrefix) {
        _error = withContext(reasonPrefix)._error;
    }

    bool isOK() const {
        return !_error;
    }

    ErrorCodes::Error code() const {
        return _error ? _error->code : ErrorCodes::OK;
    }

    std::string codeString() const {
        return ErrorCodes::errorString(code());
    }

    /** Returns the reason string or the empty string if isOK(). */
    const std::string& reason() const {
        if (_error)
            return _error->reason;
        static StaticImmortal<const std::string> empty;
        return *empty;
    }

    /** Returns the generic ErrorExtraInfo if present. */
    std::shared_ptr<const ErrorExtraInfo> extraInfo() const {
        return isOK() ? nullptr : _error->extra;
    }

    /** Returns a specific subclass of ErrorExtraInfo if the error code matches that type. */
    template <typename T>
    std::shared_ptr<const T> extraInfo() const {
        MONGO_STATIC_ASSERT(std::is_base_of_v<ErrorExtraInfo, T>);
        MONGO_STATIC_ASSERT(std::is_same_v<error_details::ErrorExtraInfoFor<T::code>, T>);

        if (isOK())
            return nullptr;
        if (code() != T::code)
            return nullptr;

        if (!_error->extra) {
            invariant(!ErrorCodes::mustHaveExtraInfo(_error->code));
            return nullptr;
        }

        // Can't use checked_cast due to include cycle.
        dassert(dynamic_cast<const T*>(_error->extra.get()));
        return std::static_pointer_cast<const T>(_error->extra);
    }

    std::string toString() const;

    /**
     * Serializes the "code", "codeName", "errmsg" (aka `Status::reason()`) in
     * the canonical format used in the server command responses. If present,
     * the `extraInfo()` object is also serialized to the builder.
     */
    void serialize(BSONObjBuilder* builder) const;

    /** Same as `serialize`, but may only be called on non-OK Status. */
    void serializeErrorToBSON(BSONObjBuilder* builder) const {
        invariant(!isOK());
        serialize(builder);
    }

    /** Returns true if this Status's code is a member of the given category. */
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

    /** Only compares codes. Ignores reason strings. */
    friend bool operator==(const Status& a, const Status& b) {
        return a.code() == b.code();
    }
    friend bool operator!=(const Status& a, const Status& b) {
        return !(a == b);
    }

    /** Status and ErrorCodes::Error are symmetrically EqualityComparable. */
    friend bool operator==(const Status& a, ErrorCodes::Error b) {
        return a.code() == b;
    }
    friend bool operator!=(const Status& a, ErrorCodes::Error b) {
        return !(a == b);
    }
    friend bool operator==(ErrorCodes::Error a, const Status& b) {
        return b == a;
    }
    friend bool operator!=(ErrorCodes::Error a, const Status& b) {
        return b != a;
    }

    /**
     * A `std::ostream` receives a minimal Status representation:
     *     os << codeString() << " " << reason();
     * This leaves a trailing space if reason is empty (e.g. the OK Status).
     */
    friend std::ostream& operator<<(std::ostream& os, const Status& status) {
        return status._streamTo(os);
    }

    /**
     * A `StringBuilder` receives the serialized errorInfo:
     *
     * For an isOK() Status:
     *     os << codeString();
     * Otherwise:
     *     os << codeString()
     *        << serializedErrorInfo // if present
     *        << ": " << reason()
     */
    friend StringBuilder& operator<<(StringBuilder& os, const Status& status) {
        return status._streamTo(os);
    }

private:
    struct ErrorInfo : boost::intrusive_ref_counter<ErrorInfo> {
        ErrorInfo(ErrorCodes::Error code,
                  std::string reason,
                  std::shared_ptr<const ErrorExtraInfo> extra)
            : code{code}, reason{std::move(reason)}, extra{std::move(extra)} {}

        ErrorCodes::Error code;
        std::string reason;
        std::shared_ptr<const ErrorExtraInfo> extra;
    };

    static boost::intrusive_ptr<const ErrorInfo> _createErrorInfo(
        ErrorCodes::Error code, std::string reason, std::shared_ptr<const ErrorExtraInfo> extra);

    static boost::intrusive_ptr<const ErrorInfo> _parseErrorInfo(ErrorCodes::Error code,
                                                                 std::string reason,
                                                                 const BSONObj& extraObj);

    Status() = default;

    // Private since it could result in a type mismatch between code and extraInfo.
    MONGO_COMPILER_COLD_FUNCTION Status(ErrorCodes::Error code,
                                        std::string reason,
                                        std::shared_ptr<const ErrorExtraInfo> extra)
        : _error{_createErrorInfo(code, std::move(reason), std::move(extra))} {}

    std::ostream& _streamTo(std::ostream& os) const;
    StringBuilder& _streamTo(StringBuilder& os) const;

    boost::intrusive_ptr<const ErrorInfo> _error;
};

}  // namespace mongo
