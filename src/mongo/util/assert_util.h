// assert_util.h

/*    Copyright 2009 10gen Inc.
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

#include <typeinfo>
#include <string>

#include "mongo/base/status.h"  // NOTE: This is safe as utils depend on base
#include "mongo/platform/compiler.h"
#include "mongo/logger/log_severity.h"
#include "mongo/logger/logger.h"
#include "mongo/logger/logstream_builder.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/debug_util.h"

#define MONGO_INCLUDE_INVARIANT_H_WHITELISTED
#include "mongo/util/invariant.h"
#undef MONGO_INCLUDE_INVARIANT_H_WHITELISTED

namespace mongo {

enum CommonErrorCodes {
    OkCode = 0,
    SendStaleConfigCode = 13388,       // uassert( 13388 )
    RecvStaleConfigCode = 9996,        // uassert( 9996 )
    PrepareConfigsFailedCode = 13104,  // uassert( 13104 )
    NotMasterOrSecondaryCode = 13436,  // uassert( 13436 )
    NotMasterNoSlaveOkCode = 13435,    // uassert( 13435 )
    NotMaster = 10107,                 // uassert( 10107 )
};

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

/** Most mongo exceptions inherit from this; this is commonly caught in most threads */
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
    static bool traceExceptions;

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

MONGO_COMPILER_NORETURN void verifyFailed(const char* expr, const char* file, unsigned line);
MONGO_COMPILER_NORETURN void invariantOKFailed(const char* expr,
                                               const Status& status,
                                               const char* file,
                                               unsigned line);
void wasserted(const char* expr, const char* file, unsigned line);
MONGO_COMPILER_NORETURN void fassertFailed(int msgid);
MONGO_COMPILER_NORETURN void fassertFailedNoTrace(int msgid);
MONGO_COMPILER_NORETURN void fassertFailedWithStatus(int msgid, const Status& status);
MONGO_COMPILER_NORETURN void fassertFailedWithStatusNoTrace(int msgid, const Status& status);

/** a "user assertion".  throws UserAssertion.  logs.  typically used for errors that a user
    could cause, such as duplicate key, disk full, etc.
*/
MONGO_COMPILER_NORETURN void uasserted(int msgid, const char* msg);
MONGO_COMPILER_NORETURN void uasserted(int msgid, const std::string& msg);

/** msgassert and massert are for errors that are internal but have a well defined error text
    std::string.  a stack trace is logged.
*/
MONGO_COMPILER_NORETURN void msgassertedNoTrace(int msgid, const char* msg);
MONGO_COMPILER_NORETURN void msgassertedNoTrace(int msgid, const std::string& msg);
MONGO_COMPILER_NORETURN void msgasserted(int msgid, const char* msg);
MONGO_COMPILER_NORETURN void msgasserted(int msgid, const std::string& msg);

/* convert various types of exceptions to strings */
std::string causedBy(const char* e);
std::string causedBy(const DBException& e);
std::string causedBy(const std::exception& e);
std::string causedBy(const std::string& e);
std::string causedBy(const std::string* e);
std::string causedBy(const Status& e);

/** aborts on condition failure */
inline void fassert(int msgid, bool testOK) {
    if (MONGO_unlikely(!testOK))
        fassertFailed(msgid);
}

inline void fassert(int msgid, const Status& status) {
    if (MONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatus(msgid, status);
    }
}

inline void fassertNoTrace(int msgid, const Status& status) {
    if (MONGO_unlikely(!status.isOK())) {
        fassertFailedWithStatusNoTrace(msgid, status);
    }
}


/* "user assert".  if asserts, user did something wrong, not our code */
#define MONGO_uassert(msgid, msg, expr)     \
    do {                                    \
        if (MONGO_unlikely(!(expr))) {      \
            ::mongo::uasserted(msgid, msg); \
        }                                   \
    } while (false)

inline void uassertStatusOK(const Status& status) {
    if (MONGO_unlikely(!status.isOK())) {
        uasserted((status.location() != 0 ? status.location() : status.code()), status.reason());
    }
}

template <typename T>
inline T uassertStatusOK(StatusWith<T> sw) {
    if (MONGO_unlikely(!sw.isOK())) {
        const auto& status = sw.getStatus();
        uasserted((status.location() != 0 ? status.location() : status.code()), status.reason());
    }
    return std::move(sw.getValue());
}

template <typename T>
inline T fassertStatusOK(int msgid, StatusWith<T> sw) {
    if (MONGO_unlikely(!sw.isOK())) {
        fassertFailedWithStatus(msgid, sw.getStatus());
    }
    return std::move(sw.getValue());
}

inline void fassertStatusOK(int msgid, const Status& s) {
    if (MONGO_unlikely(!s.isOK())) {
        fassertFailedWithStatus(msgid, s);
    }
}

/* warning only - keeps going */
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
#define MONGO_massert(msgid, msg, expr)       \
    do {                                      \
        if (MONGO_unlikely(!(expr))) {        \
            ::mongo::msgasserted(msgid, msg); \
        }                                     \
    } while (false)

inline void massertStatusOK(const Status& status) {
    if (MONGO_unlikely(!status.isOK())) {
        msgasserted((status.location() != 0 ? status.location() : status.code()), status.reason());
    }
}

inline void massertNoTraceStatusOK(const Status& status) {
    if (MONGO_unlikely(!status.isOK())) {
        msgassertedNoTrace((status.location() != 0 ? status.location() : status.code()),
                           status.reason());
    }
}

/* same as massert except no msgid */
#define MONGO_verify(_Expression)                                    \
    do {                                                             \
        if (MONGO_unlikely(!(_Expression))) {                        \
            ::mongo::verifyFailed(#_Expression, __FILE__, __LINE__); \
        }                                                            \
    } while (false)


#define MONGO_invariantOK(expression)                                                         \
    do {                                                                                      \
        const ::mongo::Status _invariantOK_status = expression;                               \
        if (MONGO_unlikely(!_invariantOK_status.isOK())) {                                    \
            ::mongo::invariantOKFailed(#expression, _invariantOK_status, __FILE__, __LINE__); \
        }                                                                                     \
    } while (false)

#define verify(expression) MONGO_verify(expression)
#define invariantOK MONGO_invariantOK
#define uassert MONGO_uassert
#define wassert MONGO_wassert
#define massert MONGO_massert

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
Status exceptionToStatus();

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

#define DESTRUCTOR_GUARD MONGO_DESTRUCTOR_GUARD
#define MONGO_DESTRUCTOR_GUARD(expression)                                                     \
    try {                                                                                      \
        expression;                                                                            \
    } catch (const std::exception& e) {                                                        \
        ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),                  \
                                          ::mongo::getThreadName(),                            \
                                          ::mongo::logger::LogSeverity::Log())                 \
            << "caught exception (" << e.what() << ") in destructor (" << __FUNCTION__ << ")"  \
            << std::endl;                                                                      \
    } catch (...) {                                                                            \
        ::mongo::logger::LogstreamBuilder(::mongo::logger::globalLogDomain(),                  \
                                          ::mongo::getThreadName(),                            \
                                          ::mongo::logger::LogSeverity::Log())                 \
            << "caught unknown exception in destructor (" << __FUNCTION__ << ")" << std::endl; \
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
