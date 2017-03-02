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

#include <string>
#include <typeinfo>

#include "bongo/base/status.h"  // NOTE: This is safe as utils depend on base
#include "bongo/base/status_with.h"
#include "bongo/platform/compiler.h"
#include "bongo/util/concurrency/thread_name.h"
#include "bongo/util/debug_util.h"

#define BONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "bongo/util/invariant.h"
#undef BONGO_INCLUDE_INVARIANT_H_WHITELISTED

namespace bongo {

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

class BSONObjBuilder;

struct ExceptionInfo {
    ExceptionInfo() : msg(""), code(-1) {}
    ExceptionInfo(const char* m, int c) : msg(m), code(c) {}
    ExceptionInfo(const std::string& m, int c) : msg(m), code(c) {}
    void append(BSONObjBuilder& b, const char* m = "$err", const char* c = "code") const;
    std::string toString() const;
    bool empty() const {
        return msg.empty();
    }
    void reset() {
        msg = "";
        code = -1;
    }
    std::string msg;
    int code;
};

class DBException;
std::string causedBy(const DBException& e);
std::string causedBy(const std::string& e);

/** Most bongo exceptions inherit from this; this is commonly caught in most threads */
class DBException : public std::exception {
public:
    DBException(const ExceptionInfo& ei) : _ei(ei) {
        traceIfNeeded(*this);
    }
    DBException(const char* msg, int code) : _ei(msg, code) {
        traceIfNeeded(*this);
    }
    DBException(const std::string& msg, int code) : _ei(msg, code) {
        traceIfNeeded(*this);
    }
    virtual ~DBException() throw() {}

    virtual const char* what() const throw() {
        return _ei.msg.c_str();
    }
    virtual int getCode() const {
        return _ei.code;
    }
    virtual void appendPrefix(std::stringstream& ss) const {}
    virtual void addContext(const std::string& str) {
        _ei.msg = str + causedBy(_ei.msg);
    }

    // Utilities for the migration to Status objects
    static ErrorCodes::Error convertExceptionCode(int exCode);

    Status toStatus(const std::string& context) const {
        return Status(convertExceptionCode(getCode()), context + causedBy(*this));
    }
    Status toStatus() const {
        return Status(convertExceptionCode(getCode()), this->what());
    }

    // context when applicable. otherwise ""
    std::string _shard;

    virtual std::string toString() const;

    const ExceptionInfo& getInfo() const {
        return _ei;
    }

private:
    static void traceIfNeeded(const DBException& e);

public:
    static AtomicBool traceExceptions;

protected:
    ExceptionInfo _ei;
};

class AssertionException : public DBException {
public:
    AssertionException(const ExceptionInfo& ei) : DBException(ei) {}
    AssertionException(const char* msg, int code) : DBException(msg, code) {}
    AssertionException(const std::string& msg, int code) : DBException(msg, code) {}

    virtual ~AssertionException() throw() {}

    virtual bool severe() const {
        return true;
    }
    virtual bool isUserAssertion() const {
        return false;
    }
};

/* UserExceptions are valid errors that a user can cause, like out of disk space or duplicate key */
class UserException : public AssertionException {
public:
    UserException(int c, const std::string& m) : AssertionException(m, c) {}
    virtual bool severe() const {
        return false;
    }
    virtual bool isUserAssertion() const {
        return true;
    }
    virtual void appendPrefix(std::stringstream& ss) const;
};

class MsgAssertionException : public AssertionException {
public:
    MsgAssertionException(const ExceptionInfo& ei) : AssertionException(ei) {}
    MsgAssertionException(int c, const std::string& m) : AssertionException(m, c) {}
    virtual bool severe() const {
        return false;
    }
    virtual void appendPrefix(std::stringstream& ss) const;
};

BONGO_COMPILER_NORETURN void verifyFailed(const char* expr, const char* file, unsigned line);
BONGO_COMPILER_NORETURN void invariantOKFailed(const char* expr,
                                               const Status& status,
                                               const char* file,
                                               unsigned line) noexcept;
void wasserted(const char* expr, const char* file, unsigned line);

#define fassertFailed BONGO_fassertFailed
#define BONGO_fassertFailed(...) ::bongo::fassertFailedWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void fassertFailedWithLocation(int msgid,
                                                       const char* file,
                                                       unsigned line) noexcept;

#define fassertFailedNoTrace BONGO_fassertFailedNoTrace
#define BONGO_fassertFailedNoTrace(...) \
    ::bongo::fassertFailedNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void fassertFailedNoTraceWithLocation(int msgid,
                                                              const char* file,
                                                              unsigned line) noexcept;

#define fassertFailedWithStatus BONGO_fassertFailedWithStatus
#define BONGO_fassertFailedWithStatus(...) \
    ::bongo::fassertFailedWithStatusWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void fassertFailedWithStatusWithLocation(int msgid,
                                                                 const Status& status,
                                                                 const char* file,
                                                                 unsigned line) noexcept;

#define fassertFailedWithStatusNoTrace BONGO_fassertFailedWithStatusNoTrace
#define BONGO_fassertFailedWithStatusNoTrace(...) \
    ::bongo::fassertFailedWithStatusNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void fassertFailedWithStatusNoTraceWithLocation(int msgid,
                                                                        const Status& status,
                                                                        const char* file,
                                                                        unsigned line) noexcept;

/** a "user assertion".  throws UserAssertion.  logs.  typically used for errors that a user
    could cause, such as duplicate key, disk full, etc.
*/
BONGO_COMPILER_NORETURN void uassertedWithLocation(int msgid,
                                                   const char* msg,
                                                   const char* file,
                                                   unsigned line);
BONGO_COMPILER_NORETURN void uassertedWithLocation(int msgid,
                                                   const std::string& msg,
                                                   const char* file,
                                                   unsigned line);

/** msgassert and massert are for errors that are internal but have a well defined error text
    std::string.  a stack trace is logged.
*/

#define msgassertedNoTrace BONGO_msgassertedNoTrace
#define BONGO_msgassertedNoTrace(...) \
    ::bongo::msgassertedNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void msgassertedNoTraceWithLocation(int msgid,
                                                            const char* msg,
                                                            const char* file,
                                                            unsigned line);
BONGO_COMPILER_NORETURN void msgassertedNoTraceWithLocation(int msgid,
                                                            const std::string& msg,
                                                            const char* file,
                                                            unsigned line);

#define msgassertedNoTraceWithStatus BONGO_msgassertedNoTraceWithStatus
#define BONGO_msgassertedNoTraceWithStatus(...) \
    ::bongo::msgassertedNoTraceWithStatusWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void msgassertedNoTraceWithStatusWithLocation(int msgid,
                                                                      const Status& status,
                                                                      const char* file,
                                                                      unsigned line);

#define msgasserted BONGO_msgasserted
#define BONGO_msgasserted(...) ::bongo::msgassertedWithLocation(__VA_ARGS__, __FILE__, __LINE__)
BONGO_COMPILER_NORETURN void msgassertedWithLocation(int msgid,
                                                     const char* msg,
                                                     const char* file,
                                                     unsigned line);
BONGO_COMPILER_NORETURN void msgassertedWithLocation(int msgid,
                                                     const std::string& msg,
                                                     const char* file,
                                                     unsigned line);

/* convert various types of exceptions to strings */
std::string causedBy(const char* e);
std::string causedBy(const DBException& e);
std::string causedBy(const std::exception& e);
std::string causedBy(const std::string& e);
std::string causedBy(const std::string* e);
std::string causedBy(const Status& e);

#define fassert BONGO_fassert
#define BONGO_fassert(...) ::bongo::fassertWithLocation(__VA_ARGS__, __FILE__, __LINE__)

/** aborts on condition failure */
inline void fassertWithLocation(int msgid, bool testOK, const char* file, unsigned line) {
    if (BONGO_unlikely(!testOK)) {
        fassertFailedWithLocation(msgid, file, line);
    }
}

inline void fassertWithLocation(int msgid, const Status& status, const char* file, unsigned line) {
    if (BONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, status, file, line);
    }
}

#define fassertNoTrace BONGO_fassertNoTrace
#define BONGO_fassertNoTrace(...) \
    ::bongo::fassertNoTraceWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void fassertNoTraceWithLocation(int msgid, bool testOK, const char* file, unsigned line) {
    if (BONGO_unlikely(!testOK)) {
        fassertFailedNoTraceWithLocation(msgid, file, line);
    }
}

inline void fassertNoTraceWithLocation(int msgid,
                                       const Status& status,
                                       const char* file,
                                       unsigned line) {
    if (BONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatusNoTraceWithLocation(msgid, status, file, line);
    }
}

/**
 * "user assert".  if asserts, user did something wrong, not our code.
 *
 * Using an immediately invoked lambda to give the compiler an easy way to inline the check (expr)
 * and out-of-line the error path. This is most helpful when the error path involves building a
 * complex error message in the expansion of msg. The call to the lambda is followed by
 * BONGO_COMPILER_UNREACHABLE as it is impossible to mark a lambda noreturn.
 */
#define uassert BONGO_uassert
#define BONGO_uassert(msgid, msg, expr)                                         \
    do {                                                                        \
        if (BONGO_unlikely(!(expr))) {                                          \
            [&]() BONGO_COMPILER_COLD_FUNCTION {                                \
                ::bongo::uassertedWithLocation(msgid, msg, __FILE__, __LINE__); \
            }();                                                                \
            BONGO_COMPILER_UNREACHABLE;                                         \
        }                                                                       \
    } while (false)

#define uasserted BONGO_uasserted
#define BONGO_uasserted(...) ::bongo::uassertedWithLocation(__VA_ARGS__, __FILE__, __LINE__)

#define uassertStatusOK BONGO_uassertStatusOK
#define BONGO_uassertStatusOK(...) \
    ::bongo::uassertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void uassertStatusOKWithLocation(const Status& status, const char* file, unsigned line) {
    if (BONGO_unlikely(!status.isOK())) {
        uassertedWithLocation((status.location() != 0 ? status.location() : status.code()),
                              status.reason(),
                              file,
                              line);
    }
}

template <typename T>
inline T uassertStatusOKWithLocation(StatusWith<T> sw, const char* file, unsigned line) {
    uassertStatusOKWithLocation(sw.getStatus(), file, line);
    return std::move(sw.getValue());
}

#define fassertStatusOK BONGO_fassertStatusOK
#define BONGO_fassertStatusOK(...) \
    ::bongo::fassertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
template <typename T>
inline T fassertStatusOKWithLocation(int msgid, StatusWith<T> sw, const char* file, unsigned line) {
    if (BONGO_unlikely(!sw.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, sw.getStatus(), file, line);
    }
    return std::move(sw.getValue());
}

inline void fassertStatusOKWithLocation(int msgid,
                                        const Status& s,
                                        const char* file,
                                        unsigned line) {
    if (BONGO_unlikely(!s.isOK())) {
        fassertFailedWithStatusWithLocation(msgid, s, file, line);
    }
}

/* warning only - keeps going */
#define wassert BONGO_wassert
#define BONGO_wassert(_Expression)                                \
    do {                                                          \
        if (BONGO_unlikely(!(_Expression))) {                     \
            ::bongo::wasserted(#_Expression, __FILE__, __LINE__); \
        }                                                         \
    } while (false)

/* display a message, no context, and throw assertionexception

   easy way to throw an exception and log something without our stack trace
   display happening.
*/
#define massert BONGO_massert
#define BONGO_massert(msgid, msg, expr)                                           \
    do {                                                                          \
        if (BONGO_unlikely(!(expr))) {                                            \
            [&]() BONGO_COMPILER_COLD_FUNCTION {                                  \
                ::bongo::msgassertedWithLocation(msgid, msg, __FILE__, __LINE__); \
            }();                                                                  \
            BONGO_COMPILER_UNREACHABLE;                                           \
        }                                                                         \
    } while (false)


#define massertStatusOK BONGO_massertStatusOK
#define BONGO_massertStatusOK(...) \
    ::bongo::massertStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void massertStatusOKWithLocation(const Status& status, const char* file, unsigned line) {
    if (BONGO_unlikely(!status.isOK())) {
        msgassertedWithLocation((status.location() != 0 ? status.location() : status.code()),
                                status.reason(),
                                file,
                                line);
    }
}

#define massertNoTraceStatusOK BONGO_massertNoTraceStatusOK
#define BONGO_massertNoTraceStatusOK(...) \
    ::bongo::massertNoTraceStatusOKWithLocation(__VA_ARGS__, __FILE__, __LINE__)
inline void massertNoTraceStatusOKWithLocation(const Status& status,
                                               const char* file,
                                               unsigned line) {
    if (BONGO_unlikely(!status.isOK())) {
        [&]() BONGO_COMPILER_COLD_FUNCTION {
            msgassertedNoTraceWithLocation(
                (status.location() != 0 ? status.location() : status.code()),
                status.reason(),
                file,
                line);
        }();
        BONGO_COMPILER_UNREACHABLE;
    }
}

/* same as massert except no msgid */
#define verify(expression) BONGO_verify(expression)
#define BONGO_verify(_Expression)                                    \
    do {                                                             \
        if (BONGO_unlikely(!(_Expression))) {                        \
            ::bongo::verifyFailed(#_Expression, __FILE__, __LINE__); \
        }                                                            \
    } while (false)

#define invariantOK BONGO_invariantOK
#define BONGO_invariantOK(expression)                                                         \
    do {                                                                                      \
        const ::bongo::Status _invariantOK_status = expression;                               \
        if (BONGO_unlikely(!_invariantOK_status.isOK())) {                                    \
            ::bongo::invariantOKFailed(#expression, _invariantOK_status, __FILE__, __LINE__); \
        }                                                                                     \
    } while (false)

#define dassertOK BONGO_dassertOK
#define BONGO_dassertOK(expression) \
    if (kDebugBuild)                \
    invariantOK(expression)

// some special ids that we want to duplicate

// > 10000 asserts
// < 10000 UserException

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

}  // namespace bongo

#define BONGO_ASSERT_ON_EXCEPTION(expression)                                         \
    try {                                                                             \
        expression;                                                                   \
    } catch (const std::exception& e) {                                               \
        std::stringstream ss;                                                         \
        ss << "caught exception: " << e.what() << ' ' << __FILE__ << ' ' << __LINE__; \
        msgasserted(13294, ss.str());                                                 \
    } catch (...) {                                                                   \
        massert(10437, "unknown exception", false);                                   \
    }

#define BONGO_ASSERT_ON_EXCEPTION_WITH_MSG(expression, msg)         \
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
 *         BONGO_UNREACHABLE;
 */

#define BONGO_UNREACHABLE ::bongo::invariantFailed("Hit a BONGO_UNREACHABLE!", __FILE__, __LINE__);
