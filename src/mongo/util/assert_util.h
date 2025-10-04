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
#include "mongo/base/error_extra_info.h"
#include "mongo/base/static_assert.h"
#include "mongo/base/status.h"
#include "mongo/base/status_with.h"
#include "mongo/base/string_data.h"
#include "mongo/platform/atomic_word.h"
#include "mongo/platform/compiler.h"
#include "mongo/platform/source_location.h"
#include "mongo/util/assert_util_core.h"  // IWYU pragma: export
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"
#include "mongo/util/exit_code.h"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>
#include <typeinfo>
#include <utility>
#include <vector>

#include <fmt/format.h>

namespace mongo {

/**
 * Sets the appropriate state to enable/disable diagnostic logging based on `newVal`.
 */
void setDiagnosticLoggingInAssertUtil(bool newVal);

class AssertionCount {
public:
    AssertionCount();
    void rollover();
    void condrollover(int newValue);

    AtomicWord<int> regular;
    AtomicWord<int> warning;
    AtomicWord<int> msg;
    AtomicWord<int> user;
    AtomicWord<int> tripwire;
    AtomicWord<int> rollovers;
};

extern AssertionCount assertionCount;
class DBException;

/** Most mongo exceptions inherit from this; this is commonly caught in most threads */
class DBException : public std::exception {
public:
    const char* what() const noexcept final {
        return reason().c_str();
    }

    virtual void addContext(StringData context) {
        _status.addContext(context);
    }

    Status toStatus(StringData context) const {
        return _status.withContext(context);
    }
    const Status& toStatus() const {
        return _status;
    }

    virtual std::string toString() const {
        return _status.toString();
    }

    virtual void serialize(BSONObjBuilder* builder) const {
        _status.serialize(builder);
    }

    const std::string& reason() const {
        return _status.reason();
    }

    ErrorCodes::Error code() const {
        return _status.code();
    }

    std::string codeString() const {
        return _status.codeString();
    }

    /**
     * Returns true if this DBException's code is a member of the given category.
     */
    template <ErrorCategory category>
    bool isA() const {
        return ErrorCodes::isA<category>(*this);
    }

    /**
     * Returns the generic ErrorExtraInfo if present.
     */
    std::shared_ptr<const ErrorExtraInfo> extraInfo() const {
        return _status.extraInfo();
    }

    /**
     * Returns a specific subclass of ErrorExtraInfo if the error code matches that type.
     */
    template <typename ErrorDetail>
    std::shared_ptr<const ErrorDetail> extraInfo() const {
        return _status.extraInfo<ErrorDetail>();
    }

    static inline AtomicWord<bool> traceExceptions{false};

    /**
     * Allows handling `ErrorCodes::WriteConflict` as a special case and if true, will call
     * `printStackTrace` on every `WriteConflict` error. Can be set via the
     * `traceWriteConflictExceptions` server parameter.
     */
    static inline AtomicWord<bool> traceWriteConflictExceptions{false};

protected:
    DBException(const Status& status) : _status(status) {
        invariant(!status.isOK());
        traceIfNeeded(*this);
    }

private:
    static void traceIfNeeded(const DBException& e);

    /**
     * This method exists only to make all non-final types in this hierarchy abstract to prevent
     * accidental slicing.
     */
    virtual void defineOnlyInFinalSubclassToPreventSlicing() = 0;

    Status _status;
};

class AssertionException : public DBException {
public:
    AssertionException(const Status& status) : DBException(status) {}
};

/**
 * Encompasses a class of exceptions due to lack of resources or conflicting resources. Can be used
 * to conveniently catch all derived exceptions instead of enumerating each of them individually.
 */
class StorageUnavailableException : public DBException {
public:
    using DBException::DBException;
};

/**
 * Use `throwWriteConflictException()` instead of throwing `WriteConflictException` directly.
 */
class WriteConflictException final : public StorageUnavailableException {
public:
    WriteConflictException(const Status& status) : StorageUnavailableException(status) {}

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

/**
 * Use `throwTemporarilyUnavailableException()` instead of throwing
 * `TemporarilyUnavailableException` directly.
 */
class TemporarilyUnavailableException final : public StorageUnavailableException {
public:
    TemporarilyUnavailableException(const Status& status) : StorageUnavailableException(status) {}

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

/**
 * Use `throwTransactionTooLargeForCache()` instead of throwing
 * `TransactionTooLargeForCache` directly.
 */
class TransactionTooLargeForCacheException final : public StorageUnavailableException {
public:
    TransactionTooLargeForCacheException(const Status& status)
        : StorageUnavailableException(status) {}

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};


/**
 * This namespace contains implementation details for our error handling code and should not be used
 * directly in general code.
 */
namespace error_details {

/**
 * The base class of all DBExceptions for codes of the given ErrorCategory to allow catching by
 * category.
 */
template <ErrorCategory kCategory>
class ExceptionForCat : public virtual AssertionException {
protected:
    // This will only be called by subclasses, and they are required to instantiate
    // AssertionException themselves since it is a virtual base. Therefore, the AssertionException
    // construction here should never actually execute, but it is required to be present to allow
    // subclasses to construct us.
    ExceptionForCat() : AssertionException((std::abort(), Status::OK())) {
        invariant(isA<kCategory>());
    }
};

template <ErrorCodes::Error kCode, typename... Bases>
class ExceptionForCode final : public Bases... {
public:
    MONGO_STATIC_ASSERT(isNamedCode<kCode>);

    ExceptionForCode(const Status& status) : AssertionException(status) {
        invariant(status.code() == kCode);
    }

    // This is only a template to enable SFINAE. It will only be instantiated with the default
    // value.
    template <ErrorCodes::Error code_copy = kCode>
    std::shared_ptr<const ErrorExtraInfoFor<code_copy>> operator->() const {
        MONGO_STATIC_ASSERT(code_copy == kCode);
        return this->template extraInfo<ErrorExtraInfoFor<kCode>>();
    }

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

template <ErrorCodes::Error code, typename categories = ErrorCategoriesFor<code>>
struct ExceptionForCodeDispatcher;

template <ErrorCodes::Error code, ErrorCategory... categories>
struct ExceptionForCodeDispatcher<code, CategoryList<categories...>> {
    using type = std::conditional_t<sizeof...(categories) == 0,
                                    ExceptionForCode<code, AssertionException>,
                                    ExceptionForCode<code, ExceptionForCat<categories>...>>;
};

// Note: second template parameter shouldn't be necessary but is on MSVC.
// It seems to be unable to disambiguate the partial specializations of an auto NTTP
// with concrete types and needs a dummy type template parameter to do so.
template <auto codeOrCat, typename = decltype(codeOrCat)>
struct ExceptionForDispatcher;

template <ErrorCodes::Error code>
struct ExceptionForDispatcher<code, ErrorCodes::Error> : ExceptionForCodeDispatcher<code> {};

template <>
struct ExceptionForDispatcher<ErrorCodes::WriteConflict, ErrorCodes::Error> {
    using type = WriteConflictException;
};

template <>
struct ExceptionForDispatcher<ErrorCodes::TemporarilyUnavailable, ErrorCodes::Error> {
    using type = TemporarilyUnavailableException;
};

template <>
struct ExceptionForDispatcher<ErrorCodes::TransactionTooLargeForCache, ErrorCodes::Error> {
    using type = TransactionTooLargeForCacheException;
};

template <ErrorCategory category>
struct ExceptionForDispatcher<category, ErrorCategory> {
    using type = ExceptionForCat<category>;
};

}  // namespace error_details


/**
 * Resolves to the concrete exception type for the given ErrorCode or ErrorCategory.
 *
 * It will be a subclass of AssertionException, and when passed an error code, it will also be a
 * subclass of ExceptionFor<ErrorCategory> of every category that the code belongs to.
 */
template <auto codeOrCatagory>
requires std::is_same_v<decltype(codeOrCatagory), ErrorCodes::Error> ||
    std::is_same_v<decltype(codeOrCatagory), ErrorCategory>
using ExceptionFor = typename error_details::ExceptionForDispatcher<codeOrCatagory>::type;


MONGO_COMPILER_NORETURN void verifyFailed(const char* expr,
                                          SourceLocation loc = MONGO_SOURCE_LOCATION());
MONGO_COMPILER_NORETURN void invariantOKFailed(
    const char* expr, const Status& status, SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;
MONGO_COMPILER_NORETURN void invariantOKFailedWithMsg(
    const char* expr,
    const Status& status,
    const std::string& msg,
    SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;

namespace fassert_detail {

/** Convertible from exactly `int`, but not from bool or other types that convert to int. */
struct MsgId {
    /** Allow exactly int */
    constexpr explicit(false) MsgId(int id) : id{id} {}

    /** Allow copy */
    constexpr MsgId(const MsgId&) = default;
    constexpr MsgId& operator=(const MsgId&) = default;

    /** Reject everything else. */
    template <typename T, std::enable_if_t<!std::is_same_v<std::decay_t<T>, MsgId>, int> = 0>
    explicit(false) MsgId(T&&) = delete;

    int id;
};

MONGO_COMPILER_NORETURN void failed(MsgId msgid,
                                    SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;
MONGO_COMPILER_NORETURN void failed(MsgId msgid,
                                    const Status& status,
                                    SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;
MONGO_COMPILER_NORETURN void failedNoTrace(MsgId msgid,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;
MONGO_COMPILER_NORETURN void failedNoTrace(MsgId msgid,
                                           const Status& status,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;

/** Aborts if `cond` is false. */
constexpr void check(MsgId msgid, bool cond, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!cond)) {
        failed(msgid, loc);
    }
}

constexpr void check(MsgId msgid,
                     const Status& status,
                     SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        failed(msgid, status, loc);
    }
}

template <typename T>
constexpr T check(MsgId msgid, StatusWith<T> sw, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!sw.isOK())) {
        failed(msgid, sw.getStatus(), loc);
    }
    return std::move(sw.getValue());
}

/** Reject anything stringlike from being used as a bool cond by mistake. */
template <typename T, std::enable_if_t<std::is_convertible_v<T, StringData>, int> = 0>
void check(MsgId msgid, T&& cond, SourceLocation loc = MONGO_SOURCE_LOCATION()) = delete;

constexpr void checkNoTrace(MsgId msgid, bool cond, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!cond)) {
        failedNoTrace(msgid, loc);
    }
}

/** Reject anything stringlike from being used as a bool cond by mistake. */
template <typename T, std::enable_if_t<std::is_convertible_v<T, StringData>, int> = 0>
void checkNoTrace(MsgId msgid, T&& cond, SourceLocation loc = MONGO_SOURCE_LOCATION()) = delete;

constexpr void checkNoTrace(MsgId msgid,
                            const Status& status,
                            SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        failedNoTrace(msgid, status, loc);
    }
}

template <typename T>
constexpr T checkNoTrace(MsgId msgid,
                         StatusWith<T> sw,
                         SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!sw.isOK())) {
        failedNoTrace(msgid, sw.getStatus(), loc);
    }
    return std::move(sw.getValue());
}
}  // namespace fassert_detail

#define MONGO_fasserted(...) mongo::fassert_detail::failed(__VA_ARGS__, MONGO_SOURCE_LOCATION())
#define MONGO_fassertedNoTrace(...) \
    mongo::fassert_detail::failedNoTrace(__VA_ARGS__, MONGO_SOURCE_LOCATION())
#define MONGO_fassert(...) mongo::fassert_detail::check(__VA_ARGS__, MONGO_SOURCE_LOCATION())
#define MONGO_fassertNoTrace(...) \
    mongo::fassert_detail::checkNoTrace(__VA_ARGS__, MONGO_SOURCE_LOCATION())

/**
 * `fassert` failures will terminate the entire process; this is used for
 * low-level checks where continuing might lead to corrupt data or loss of data
 * on disk. Additionally, `fassert` will log the assertion message with fatal
 * severity and add a breakpoint before terminating.
 *
 * Each `fassert` call site must have a unique `msgid` as the first argument.
 * These are chosen using the same convention as logv2 log IDs.
 *
 * To log a custom assert message with process-fatal semantics, use LOGV2_FATAL.
 * Consider using this variant if there is only one way to reach the fatal point in code.
 *
 * The second argument is a condition. `fassert` invocations are forwarded to an
 * overload set of handlers such that it can accept a `bool`, `const Status&`,
 * or `StatusWith` as a condition.
 *
 *     void fassert(int msgid, bool cond);
 *     void fassert(int msgid, const Status& status);
 *     T fassert(int msgid, StatusWith<T> statusWith);
 *
 * `fassert` also supports a StatusWith<T> argument.
 * In that case, `fassert` returns the extracted `sw.getValue()`.
 * Because the argument is passed by value, this is best used
 * to wrap a function call directly to avoid copies:
 *
 *     T value = fassert(9079702, someStatusWithFunc());
 *
 * See the `docs/exception_architecture.md` guide for more,
 * including guidance on choosing a `msgid` number.
 */
#define fassert MONGO_fassert

/**
 * Same usage and arguments as `fassert`, but performs
 * a quickExit instead of a stacktrace-dumping abort.
 * Use LOGV2_FATAL_NO_TRACE to supply a custom error message.
 * Consider using this variant if there is only one way to reach the fatal point in code.
 */
#define fassertNoTrace MONGO_fassertNoTrace

/**
 * Like `fassert`, without a condition argument.
 * Use when failure was already determined, and needs to be reported.
 *
 * Though `fasserted` doesn't use a condition to determine whether
 * to fire, it can still accept an optional diagnostic `Status`
 * argument which will be emitted in the log.
 *
 *     fasserted(9079703);
 *     fasserted(9079704, status);
 *     fasserted(9079705, {ErrorCodes::Overflow, "too much foo"});
 *
 * `fasserted` can also be spelled `fassertFailed`.
 */
#define fasserted MONGO_fasserted

/**
 * Like `fasserted`, but performs a quickExit instead of a
 * stacktrace-dumping abort.
 */
#define fassertedNoTrace MONGO_fassertedNoTrace

/**
 * To match other assert macros, we'll prefer `fasserted` over the older
 * `fassertFailed` spelling, but they're the same thing.
 */
#define fassertFailed fasserted
#define fassertFailedWithStatus fasserted
#define fassertFailedNoTrace fassertedNoTrace
#define fassertFailedWithStatusNoTrace fassertedNoTrace

namespace error_details {

inline const Status& makeStatus(const Status& s) {
    return s;
}

template <typename T>
const Status& makeStatus(const StatusWith<T>& sw) {
    return sw.getStatus();
}

// This function exists so that uassert/massert can take plain int literals rather than requiring
// ErrorCodes::Error wrapping.
template <typename StringLike>
Status makeStatus(int code, StringLike&& message) {
    return Status(ErrorCodes::Error(code), std::forward<StringLike>(message));
}

template <typename ErrorDetail,
          typename StringLike,
          typename = stdx::enable_if_t<
              std::is_base_of<ErrorExtraInfo, std::remove_reference_t<ErrorDetail>>::value>>
Status makeStatus(ErrorDetail&& detail, StringLike&& message) {
    return Status(std::forward<ErrorDetail>(detail), std::forward<StringLike>(message));
}

}  // namespace error_details

/**
 * Common implementation for assert and assertFailed macros. Not for direct use.
 *
 * Using an immediately invoked lambda to give the compiler an easy way to inline the check (expr)
 * and out-of-line the error path. This is most helpful when the error path involves building a
 * complex error message in the expansion of msg. The call to the lambda is followed by
 * MONGO_COMPILER_UNREACHABLE as it is impossible to mark a lambda noreturn.
 * The source location is captured outside of the lambda, so that it represents
 * the function name of the macro invocation site.
 */
#define MONGO_BASE_ASSERT_FAILED(fail_func, ...)                             \
    do {                                                                     \
        auto loc = MONGO_SOURCE_LOCATION();                                  \
        [&]() MONGO_COMPILER_COLD_FUNCTION {                                 \
            fail_func(::mongo::error_details::makeStatus(__VA_ARGS__), loc); \
        }();                                                                 \
        MONGO_COMPILER_UNREACHABLE;                                          \
    } while (false)

#define MONGO_BASE_ASSERT(fail_func, code, msg, cond)       \
    do {                                                    \
        if (MONGO_unlikely(!(cond))) {                      \
            MONGO_BASE_ASSERT_FAILED(fail_func, code, msg); \
        }                                                   \
    } while (false)

/**
 * "user assert".  if asserts, user did something wrong, not our code.
 * On failure, throws an exception.
 */
#define uasserted(msgid, msg) MONGO_BASE_ASSERT_FAILED(::mongo::uassertedWithLocation, msgid, msg)
#define uassert(msgid, msg, expr) \
    MONGO_BASE_ASSERT(::mongo::uassertedWithLocation, msgid, msg, expr)

MONGO_COMPILER_NORETURN void uassertedWithLocation(const Status& status,
                                                   SourceLocation loc = MONGO_SOURCE_LOCATION());

#define uassertStatusOK(...) \
    ::mongo::uassertStatusOKWithLocation(__VA_ARGS__, MONGO_SOURCE_LOCATION())

constexpr void uassertStatusOKWithLocation(const Status& status,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        uassertedWithLocation(status, loc);
    }
}

template <typename T>
constexpr T uassertStatusOKWithLocation(StatusWith<T> sw,
                                        SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    uassertStatusOKWithLocation(sw.getStatus(), loc);
    return std::move(sw.getValue());
}

/**
 * Like uassertStatusOK(status), but also takes an expression that evaluates to  something
 * convertible to std::string to add more context to error messages. This contextExpr is only
 * evaluated if the status is not OK.
 */
#define uassertStatusOKWithContext(status, contextExpr) \
    ::mongo::uassertStatusOKWithContextAndLocation(     \
        status, [&]() -> std::string { return (contextExpr); }, MONGO_SOURCE_LOCATION())

template <typename ContextExpr>
constexpr void uassertStatusOKWithContextAndLocation(const Status& status,
                                                     ContextExpr&& contextExpr,
                                                     SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        uassertedWithLocation(status.withContext(std::forward<ContextExpr>(contextExpr)()), loc);
    }
}

template <typename T, typename ContextExpr>
constexpr T uassertStatusOKWithContextAndLocation(StatusWith<T> sw,
                                                  ContextExpr&& contextExpr,
                                                  SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    uassertStatusOKWithContextAndLocation(
        sw.getStatus(), std::forward<ContextExpr>(contextExpr), loc);
    return std::move(sw.getValue());
}

/**
 * massert is like uassert but it logs the message before throwing.
 */
#define massert(msgid, msg, expr) \
    MONGO_BASE_ASSERT(::mongo::msgassertedWithLocation, msgid, msg, expr)

#define msgasserted(msgid, msg) \
    MONGO_BASE_ASSERT_FAILED(::mongo::msgassertedWithLocation, msgid, msg)
MONGO_COMPILER_NORETURN void msgassertedWithLocation(const Status& status,
                                                     SourceLocation loc = MONGO_SOURCE_LOCATION());

#define massertStatusOK(...) \
    ::mongo::massertStatusOKWithLocation(__VA_ARGS__, MONGO_SOURCE_LOCATION())

constexpr void massertStatusOKWithLocation(const Status& status,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        msgassertedWithLocation(status, loc);
    }
}

template <typename T>
constexpr T massertStatusOKWithLocation(StatusWith<T> sw,
                                        SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    massertStatusOKWithLocation(sw.getStatus(), loc);
    return std::move(sw.getValue());
}

#define MONGO_BASE_ASSERT_VA_4(fail_func, code, msg, cond)      \
    do {                                                        \
        if (MONGO_unlikely(!(cond)))                            \
            MONGO_BASE_ASSERT_FAILED(fail_func, (code), (msg)); \
    } while (false)

#define MONGO_BASE_ASSERT_VA_2(fail_func, statusExpr)                              \
    do {                                                                           \
        if (const auto& stLocal_ = (statusExpr); MONGO_unlikely(!stLocal_.isOK())) \
            MONGO_BASE_ASSERT_FAILED(fail_func, stLocal_);                         \
    } while (false)

#define MONGO_BASE_ASSERT_VA_EXPAND(x) x /**< MSVC workaround */
#define MONGO_BASE_ASSERT_VA_PICK(_1, _2, _3, _4, x, ...) x
#define MONGO_BASE_ASSERT_VA_DISPATCH(...)                                        \
    MONGO_BASE_ASSERT_VA_EXPAND(MONGO_BASE_ASSERT_VA_PICK(__VA_ARGS__,            \
                                                          MONGO_BASE_ASSERT_VA_4, \
                                                          MONGO_BASE_ASSERT_VA_3, \
                                                          MONGO_BASE_ASSERT_VA_2, \
                                                          MONGO_BASE_ASSERT_VA_1)(__VA_ARGS__))

/**
 * `iassert` is provided as an alternative for `uassert` variants (e.g., `uassertStatusOK`)
 * to support cases where we expect a failure, the failure is recoverable, or accounting for the
 * failure, updating assertion counters, isn't desired. `iassert` logs at D3 instead of D1,
 * which helps with reducing the noise of assertions in production. The goal is to keep one
 * interface (i.e., `iassert(...)`) for all possible assertion variants, and use function
 * overloading to expand type support as needed.
 */
#define iassert(...) MONGO_BASE_ASSERT_VA_DISPATCH(::mongo::iassertFailed, __VA_ARGS__)
#define iasserted(...) MONGO_BASE_ASSERT_FAILED(::mongo::iassertFailed, __VA_ARGS__)
MONGO_COMPILER_NORETURN void iassertFailed(const Status& status,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION());

/**
 * "tripwire/test assert". Like uassert, but with a deferred-fatality tripwire that gets
 * checked prior to normal shutdown. Used to ensure that this assertion will both fail the
 * operation and also cause a test suite failure.
 */
#define tassert(...) MONGO_BASE_ASSERT_VA_DISPATCH(::mongo::tassertFailed, __VA_ARGS__)
#define tasserted(...) MONGO_BASE_ASSERT_FAILED(::mongo::tassertFailed, __VA_ARGS__)
MONGO_COMPILER_NORETURN void tassertFailed(const Status& status,
                                           SourceLocation loc = MONGO_SOURCE_LOCATION());

/**
 * Return true if tripwire conditions have occurred.
 */
bool haveTripwireAssertionsOccurred();

/**
 * If tripwire conditions have occurred, warn via the log.
 */
void warnIfTripwireAssertionsOccurred();

/**
 * MONGO_verify is deprecated. It is like invariant() in debug builds and massert() in release
 * builds.
 */
#define MONGO_verify(expression_)                                         \
    do {                                                                  \
        if (MONGO_unlikely(!(expression_))) {                             \
            ::mongo::verifyFailed(#expression_, MONGO_SOURCE_LOCATION()); \
        }                                                                 \
    } while (false)

constexpr void invariantWithLocation(const Status& status,
                                     const char* expr,
                                     SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        ::mongo::invariantOKFailed(expr, status, loc);
    }
}

template <typename T>
constexpr T invariantWithLocation(StatusWith<T> sw,
                                  const char* expr,
                                  SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!sw.isOK())) {
        ::mongo::invariantOKFailed(expr, sw.getStatus(), loc);
    }
    return std::move(sw.getValue());
}

template <typename ContextExpr>
constexpr void invariantWithContextAndLocation(const Status& status,
                                               const char* expr,
                                               ContextExpr&& contextExpr,
                                               SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        ::mongo::invariantOKFailedWithMsg(
            expr, status, std::forward<ContextExpr>(contextExpr)(), loc);
    }
}

template <typename T, typename ContextExpr>
constexpr T invariantWithContextAndLocation(StatusWith<T> sw,
                                            const char* expr,
                                            ContextExpr&& contextExpr,
                                            SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!sw.isOK())) {
        ::mongo::invariantOKFailedWithMsg(expr, sw.getStatus(), contextExpr(), loc);
    }
    return std::move(sw.getValue());
}

MONGO_COMPILER_NORETURN void invariantStatusOKFailed(
    const Status& status, SourceLocation loc = MONGO_SOURCE_LOCATION()) noexcept;

/**
 * Like uassertStatusOK(status), but for checking if an invariant holds on a status.
 */
#define invariantStatusOK(...) \
    ::mongo::invariantStatusOKWithLocation(__VA_ARGS__, MONGO_SOURCE_LOCATION())
constexpr void invariantStatusOKWithLocation(const Status& status,
                                             SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        invariantStatusOKFailed(status, loc);
    }
}

template <typename T>
constexpr T invariantStatusOKWithLocation(StatusWith<T> sw,
                                          SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    invariantStatusOKWithLocation(sw.getStatus(), loc);
    return std::move(sw.getValue());
}

/**
 * Similar to invariantStatusOK(status), but also takes an expression that evaluates to something
 * convertible to std::string to add more context to error messages. This contextExpr is only
 * evaluated if the status is not OK.
 */
#define invariantStatusOKWithContext(status, contextExpr) \
    ::mongo::invariantStatusOKWithContextAndLocation(     \
        status, [&]() -> std::string { return (contextExpr); }, MONGO_SOURCE_LOCATION())
template <typename ContextExpr>
constexpr void invariantStatusOKWithContextAndLocation(
    const Status& status, ContextExpr&& contextExpr, SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    if (MONGO_unlikely(!status.isOK())) {
        invariantStatusOKFailed(status.withContext(std::forward<ContextExpr>(contextExpr)()), loc);
    }
}

template <typename T, typename ContextExpr>
constexpr T invariantStatusOKWithContextAndLocation(StatusWith<T> sw,
                                                    ContextExpr&& contextExpr,
                                                    SourceLocation loc = MONGO_SOURCE_LOCATION()) {
    invariantStatusOKWithContextAndLocation(
        sw.getStatus(), std::forward<ContextExpr>(contextExpr), loc);
    return std::move(sw.getValue());
}

// some special ids that we want to duplicate

// > 10000 asserts
// < 10000 AssertionException

enum { ASSERT_ID_DUPKEY = 11000 };

std::string demangleName(const std::type_info& typeinfo);

/**
 * A utility function that converts an exception to a Status.
 * Only call this function when there is an active exception
 * (e.g. in a catch block).
 *
 * Note: this technique was created by Lisa Lippincott.
 *
 * Example usage:
 *
 *   Status myFunc() {
 *       try {
 *           funcThatThrows();
 *           return Status::OK();
 *       } catch (...) {
 *           return exceptionToStatus();
 *       }
 *   }
 */
Status exceptionToStatus();

/**
 * Produces an invariant failure if executed. Use when reaching this statement indicates a
 * programming error. Example:
 *     // code above checks that expr can only be FOO or BAR
 *     switch (expr) {
 *     case FOO: { ... }
 *     case BAR: { ... }
 *     default:
 *         MONGO_UNREACHABLE;
 */
#define MONGO_UNREACHABLE \
    ::mongo::invariantFailed("Hit a MONGO_UNREACHABLE!", MONGO_SOURCE_LOCATION());

/**
 * Like `MONGO_UNREACHABLE`, but triggers a `tassert` instead of an `invariant`
 */
#define MONGO_UNREACHABLE_TASSERT(msgid) tasserted(msgid, "Hit a MONGO_UNREACHABLE_TASSERT!")

/**
 * Produces an invariant failure if executed. Subset of MONGO_UNREACHABLE, but specifically
 * to indicate that the program has reached a function that is unimplemented and should be
 * unreachable from production.
 * Example:
 *
 *   void myFuncToDo() {
 *       MONGO_UNIMPLEMENTED;
 *   }
 */
#define MONGO_UNIMPLEMENTED \
    ::mongo::invariantFailed("Hit a MONGO_UNIMPLEMENTED!", MONGO_SOURCE_LOCATION());

/**
 * Like `MONGO_UNIMPLEMENTED`, but triggers a `tassert` instead of an `invariant`
 */
#define MONGO_UNIMPLEMENTED_TASSERT(msgid) tasserted(msgid, "Hit a MONGO_UNIMPLEMENTED_TASSERT!")

/**
 * A stack of auxiliary information to be dumped on invariant failure.
 * These are intended to carry only very lightweight objects like short strings
 * and numbers, to give some basic clue as to what was going on when a thread
 * suffered an invariant failure.
 */
class ScopedDebugInfoStack {
public:
    struct Rec {
        virtual ~Rec() = default;
        virtual std::string toString() const = 0;
        virtual StringData label() const = 0;
    };

    void push(const Rec* rec) {
        _stack.push_back(rec);
    }
    void pop() {
        _stack.pop_back();
    }

    /**
     * Returns the result of calling `toString` on every element in the stack in a way that prevents
     * re-entry by keeping track of how many times we have recursively called this function. This
     * prevents an infinite loop from occurring during a call to `getAll` via any exceptions thrown.
     * Note that it is not correct to write a `ScopedDebugInfo` that throws an exception, and that
     * the behavior described here is just a failsafe backstop against buggy formatters.
     */
    std::vector<std::string> getAll();

private:
    std::vector<const Rec*> _stack;
    int _loggingDepth = 0;
};

/** Each thread has its own stack of scoped debug info. */
inline ScopedDebugInfoStack& scopedDebugInfoStack() {
    thread_local ScopedDebugInfoStack tls;
    return tls;
}

/**
 * An RAII type that attaches a datum to a ScopedDebugInfoStack, intended as a
 * broad hint as to what the thread is doing. Pops that datum at scope
 * exit. By default, attaches to the thread_local ScopedDebugInfoStack.
 * If the thread encounters a fatal error, the thread's ScopedDebugInfoStack
 * is logged. The formatting of the data is done lazily during failure handling.
 *
 * Example:
 *
 *     void doSomethingAsUser(const User& currentUser) {
 *         ScopedDebugInfo userNameDbg("userName", currentUser.nameStringData());
 *         ScopedDebugInfo userIdDbg("userId", currentUser.id());
 *         somethingThatMightCrash(currentUser);
 *     }
 *
 * ScopedDebugInfo must only be used with trivially formattable values. Since it's diagnostic
 * information, formatted during error handling, formatting must not itself fail. For example,
 * formatting should not require synchronization or reading from disk. Since the result is logged,
 * the formatting must follow the usual log redaction guidance.
 *
 * The lifetime of the ScopedDebugInfo must be carefully considered. By default, a ScopedDebugInfo
 * is attached to a thread_local stack, so it must not outlive the thread that created it.
 */
template <typename T>
class ScopedDebugInfo {
public:
    ScopedDebugInfo(StringData label, T v, ScopedDebugInfoStack* stack = &scopedDebugInfoStack())
        : label(label), v(std::move(v)), stack(stack) {
        stack->push(&rec);
    }
    ~ScopedDebugInfo() {
        stack->pop();
    }
    ScopedDebugInfo(const ScopedDebugInfo&) noexcept = delete;
    ScopedDebugInfo& operator=(const ScopedDebugInfo&) noexcept = delete;

private:
    struct ThisRec : ScopedDebugInfoStack::Rec {
        explicit ThisRec(const ScopedDebugInfo* owner) : owner(owner) {}
        std::string toString() const override {
            return fmt::format("{}: {}", owner->label, owner->v);
        }
        StringData label() const override {
            return owner->label;
        }
        const ScopedDebugInfo* owner;
    };

    StringData label;
    T v;
    ScopedDebugInfoStack* stack;
    ThisRec rec{this};
};

/** Convert string-likes, exceptions, and Status to formatted "caused by" strings. */
std::string causedBy(StringData e);

inline std::string causedBy(const std::exception& e) {
    return causedBy(e.what());
}

inline std::string causedBy(const DBException& e) {
    return causedBy(e.toString());
}

inline std::string causedBy(const Status& e) {
    return causedBy(e.toString());
}

/**
 * Catch and swallow all exceptions, leaving a uniform log message.
 * Should be used only in destructors, to prevent termination.
 *
 * Usage:
 *     try {
 *         block
 *     } catch (...) {
 *         reportFailedDestructor(MONGO_SOURCE_LOCATION());
 *     }
 */
void reportFailedDestructor(SourceLocation loc = MONGO_SOURCE_LOCATION());

}  // namespace mongo
