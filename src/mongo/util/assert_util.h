/**   Copyright 2009 10gen Inc.
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

#include <cstdlib>
#include <string>
#include <typeinfo>

#include "mongo/base/status.h"  // NOTE: This is safe as utils depend on base
#include "mongo/base/status_with.h"
#include "mongo/platform/compiler.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"

#define MONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "mongo/util/invariant.h"
#undef MONGO_INCLUDE_INVARIANT_H_WHITELISTED

namespace mongo {

class AssertionCount {
public:
    AssertionCount();
    void rollover();
    void condrollover(int newValue);

    int regular;
    int warning;
    int msg;
    int user;
    int rollovers;
};

extern AssertionCount assertionCount;


class DBException;
std::string causedBy(const DBException& e);
std::string causedBy(const std::string& e);

/** Most mongo exceptions inherit from this; this is commonly caught in most threads */
class DBException : public std::exception {
public:
    const char* what() const throw() final {
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
        return ErrorCodes::isA<category>(code());
    }

    static AtomicBool traceExceptions;

protected:
    DBException(const Status& status) : _status(status) {
        invariant(!status.isOK());
        traceIfNeeded(*this);
    }

    DBException(int code, StringData msg)
        : DBException(Status(code ? ErrorCodes::Error(code) : ErrorCodes::UnknownError, msg)) {}

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
    AssertionException(int code, StringData msg) : DBException(code, msg) {}
};

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


/**
 * This namespace contains implementation details for our error handling code and should not be used
 * directly in general code.
 */
namespace error_details {

template <ErrorCodes::Error kCode, typename... Bases>
class ExceptionForImpl final : public Bases... {
public:
    MONGO_STATIC_ASSERT(isNamedCode<kCode>);

    ExceptionForImpl(const Status& status) : AssertionException(status) {
        invariant(status.code() == kCode);
    }

private:
    void defineOnlyInFinalSubclassToPreventSlicing() final {}
};

template <ErrorCodes::Error code, typename categories = ErrorCategoriesFor<code>>
struct ExceptionForDispatcher;

template <ErrorCodes::Error code, ErrorCategory... categories>
struct ExceptionForDispatcher<code, CategoryList<categories...>> {
    using type = std::conditional_t<sizeof...(categories) == 0,
                                    ExceptionForImpl<code, AssertionException>,
                                    ExceptionForImpl<code, ExceptionForCat<categories>...>>;
};

}  // namespace error_details


/**
 * Resolves to the concrete exception type for the given error code.
 *
 * It will be a subclass of both AssertionException, along with ExceptionForCat<> of every category
 * that the code belongs to.
 *
 * TODO in C++17 we can combine this with ExceptionForCat by doing something like:
 * template <auto codeOrCategory> using ExceptionFor = typename
 *      error_details::ExceptionForDispatcher<decltype(codeOrCategory)>::type;
 */
template <ErrorCodes::Error code>
using ExceptionFor = typename error_details::ExceptionForDispatcher<code>::type;

MONGO_COMPILER_NORETURN void verifyFailed(const char* expr, const char* file, unsigned line);
MONGO_COMPILER_NORETURN void invariantOKFailed(const char* expr,
                                               const Status& status,
                                               const char* file,
                                               unsigned line) noexcept;
void wasserted(const char* expr, const char* file, unsigned line);

#define fassertFailed MONGO_fassertFailed
#define MONGO_fassertFailed(...) ::mongo::fassertFailedWithLocation(__VA_ARGS__, __FILE__, __LINE__)
MONGO_COMPILER_NORETURN void fassertFailedWithLocation(int msgid,
                                                       const char* file,
                                                       unsigned line) noexcept;

#define fassertFailedNoTrace MONGO_fassertFailedNoTrace
#define MONGO_fassertFailedNoTrace(...) \
    ::mongo::fassertFailedNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
MONGO_COMPILER_NORETURN void fassertFailedNoTraceWithLocation(int msgid,
                                                              const char* file,
                                                              unsigned line) noexcept;

#define fassertFailedWithStatus MONGO_fassertFailedWithStatus
#define MONGO_fassertFailedWithStatus(...) \
    ::mongo::fassertFailedWithStatusWithLocation(__VA_ARGS__, __FILE__, __LINE__)
MONGO_COMPILER_NORETURN void fassertFailedWithStatusWithLocation(int msgid,
                                                                 const Status& status,
                                                                 const char* file,
                                                                 unsigned line) noexcept;

#define fassertFailedWithStatusNoTrace MONGO_fassertFailedWithStatusNoTrace
#define MONGO_fassertFailedWithStatusNoTrace(...) \
    ::mongo::fassertFailedWithStatusNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
MONGO_COMPILER_NORETURN void fassertFailedWithStatusNoTraceWithLocation(int msgid,
                                                                        const Status& status,
                                                                        const char* file,
                                                                        unsigned line) noexcept;

/** a "user assertion".  throws UserAssertion.  logs.  typically used for errors that a user
    could cause, such as duplicate key, disk full, etc.
*/
MONGO_COMPILER_NORETURN void uassertedWithLocation(int msgid,
                                                   StringData msg,
                                                   const char* file,
                                                   unsigned line);

/** msgassert and massert are for errors that are internal but have a well defined error text
    std::string.
*/

#define msgasserted MONGO_msgasserted
#define MONGO_msgasserted(...) ::mongo::msgassertedWithLocation(__VA_ARGS__, __FILE__, __LINE__)
MONGO_COMPILER_NORETURN void msgassertedWithLocation(int msgid,
                                                     StringData msg,
                                                     const char* file,
                                                     unsigned line);

/* convert various types of exceptions to strings */
std::string causedBy(StringData e);
std::string causedBy(const char* e);
std::string causedBy(const DBException& e);
std::string causedBy(const std::exception& e);
std::string causedBy(const std::string& e);
std::string causedBy(const std::string* e);
std::string causedBy(const Status& e);

#define fassert MONGO_fassert
#define MONGO_fassert(...) ::mongo::fassertWithLocation(__VA_ARGS__, __FILE__, __LINE__)

/** aborts on condition failure */
inline void fassertWithLocation(int msgid, bool testOK, const char* file, unsigned line) {
    if (MONGO_unlikely(!testOK)) {
        fassertFailedWithLocation(msgid, file, line);
    }
}

inline void fassertWithLocation(int msgid, const Status& status, const char* file, unsigned line) {
    if (MONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, status, file, line);
    }
}

#define fassertNoTrace MONGO_fassertNoTrace
#define MONGO_fassertNoTrace(...) \
    ::mongo::fassertNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void fassertNoTraceWithLocation(int msgid, bool testOK, const char* file, unsigned line) {
    if (MONGO_unlikely(!testOK)) {
        fassertFailedNoTraceWithLocation(msgid, file, line);
    }
}

inline void fassertNoTraceWithLocation(int msgid,
                                       const Status& status,
                                       const char* file,
                                       unsigned line) {
    if (MONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatusNoTraceWithLocation(msgid, status, file, line);
    }
}

/**
 * "user assert".  if asserts, user did something wrong, not our code.
 *
 * Using an immediately invoked lambda to give the compiler an easy way to inline the check (expr)
 * and out-of-line the error path. This is most helpful when the error path involves building a
 * complex error message in the expansion of msg. The call to the lambda is followed by
 * MONGO_COMPILER_UNREACHABLE as it is impossible to mark a lambda noreturn.
 */
#define uassert MONGO_uassert
#define MONGO_uassert(msgid, msg, expr)                                         \
    do {                                                                        \
        if (MONGO_unlikely(!(expr))) {                                          \
            [&]() MONGO_COMPILER_COLD_FUNCTION {                                \
                ::mongo::uassertedWithLocation(msgid, msg, __FILE__, __LINE__); \
            }();                                                                \
            MONGO_COMPILER_UNREACHABLE;                                         \
        }                                                                       \
    } while (false)

#define uasserted MONGO_uasserted
#define MONGO_uasserted(...) ::mongo::uassertedWithLocation(__VA_ARGS__, __FILE__, __LINE__)

#define uassertStatusOK MONGO_uassertStatusOK
#define MONGO_uassertStatusOK(...) \
    ::mongo::uassertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void uassertStatusOKWithLocation(const Status& status, const char* file, unsigned line) {
    if (MONGO_unlikely(!status.isOK())) {
        uassertedWithLocation(status.code(), status.reason(), file, line);
    }
}

template <typename T>
inline T uassertStatusOKWithLocation(StatusWith<T> sw, const char* file, unsigned line) {
    uassertStatusOKWithLocation(sw.getStatus(), file, line);
    return std::move(sw.getValue());
}

#define fassertStatusOK MONGO_fassertStatusOK
#define MONGO_fassertStatusOK(...) \
    ::mongo::fassertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
template <typename T>
inline T fassertStatusOKWithLocation(int msgid, StatusWith<T> sw, const char* file, unsigned line) {
    if (MONGO_unlikely(!sw.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, sw.getStatus(), file, line);
    }
    return std::move(sw.getValue());
}

inline void fassertStatusOKWithLocation(int msgid,
                                        const Status& s,
                                        const char* file,
                                        unsigned line) {
    if (MONGO_unlikely(!s.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, s, file, line);
    }
}

/* warning only - keeps going */
#define wassert MONGO_wassert
#define MONGO_wassert(_Expression)                                \
    do {                                                          \
        if (MONGO_unlikely(!(_Expression))) {                     \
            ::mongo::wasserted(#_Expression, __FILE__, __LINE__); \
        }                                                         \
    } while (false)

/* display a message, no context, and throw assertionexception

   easy way to throw an exception and log something without our stack trace
   display happening.
*/
#define massert MONGO_massert
#define MONGO_massert(msgid, msg, expr)                                           \
    do {                                                                          \
        if (MONGO_unlikely(!(expr))) {                                            \
            [&]() MONGO_COMPILER_COLD_FUNCTION {                                  \
                ::mongo::msgassertedWithLocation(msgid, msg, __FILE__, __LINE__); \
            }();                                                                  \
            MONGO_COMPILER_UNREACHABLE;                                           \
        }                                                                         \
    } while (false)


#define massertStatusOK MONGO_massertStatusOK
#define MONGO_massertStatusOK(...) \
    ::mongo::massertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void massertStatusOKWithLocation(const Status& status, const char* file, unsigned line) {
    if (MONGO_unlikely(!status.isOK())) {
        msgassertedWithLocation(status.code(), status.reason(), file, line);
    }
}

/* same as massert except no msgid */
#define verify(expression) MONGO_verify(expression)
#define MONGO_verify(_Expression)                                    \
    do {                                                             \
        if (MONGO_unlikely(!(_Expression))) {                        \
            ::mongo::verifyFailed(#_Expression, __FILE__, __LINE__); \
        }                                                            \
    } while (false)

#define invariantOK MONGO_invariantOK
#define MONGO_invariantOK(expression)                                                         \
    do {                                                                                      \
        const ::mongo::Status _invariantOK_status = expression;                               \
        if (MONGO_unlikely(!_invariantOK_status.isOK())) {                                    \
            ::mongo::invariantOKFailed(#expression, _invariantOK_status, __FILE__, __LINE__); \
        }                                                                                     \
    } while (false)

#define dassertOK MONGO_dassertOK
#define MONGO_dassertOK(expression) \
    if (kDebugBuild)                \
    invariantOK(expression)

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
Status exceptionToStatus() noexcept;

}  // namespace mongo

#define MONGO_ASSERT_ON_EXCEPTION(expression)                                         \
    try {                                                                             \
        expression;                                                                   \
    } catch (const std::exception& e) {                                               \
        std::stringstream ss;                                                         \
        ss << "caught exception: " << e.what() << ' ' << __FILE__ << ' ' << __LINE__; \
        msgasserted(13294, ss.str());                                                 \
    } catch (...) {                                                                   \
        massert(10437, "unknown exception", false);                                   \
    }

#define MONGO_ASSERT_ON_EXCEPTION_WITH_MSG(expression, msg)         \
    try {                                                           \
        expression;                                                 \
    } catch (const std::exception& e) {                             \
        std::stringstream ss;                                       \
        ss << msg << " caught exception exception: " << e.what();   \
        msgasserted(14043, ss.str());                               \
    } catch (...) {                                                 \
        msgasserted(14044, std::string("unknown exception") + msg); \
    }

/**
 * The purpose of this macro is to instruct the compiler that a line of code will never be reached.
 *
 * Example:
 *     // code above checks that expr can only be FOO or BAR
 *     switch (expr) {
 *     case FOO: { ... }
 *     case BAR: { ... }
 *     default:
 *         MONGO_UNREACHABLE;
 */

#define MONGO_UNREACHABLE ::mongo::invariantFailed("Hit a MONGO_UNREACHABLE!", __FILE__, __LINE__);
