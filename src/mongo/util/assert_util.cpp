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

#include "mongo/util/assert_util.h"

#include "mongo/config.h"
#include "mongo/logv2/log.h"
#include "mongo/util/debugger.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/stacktrace.h"
#include "mongo/util/str.h"

#include <exception>
#include <ostream>

#include <boost/exception/diagnostic_information.hpp>
#include <boost/exception/exception.hpp>

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kAssert


#define TRIPWIRE_ASSERTION_ID 4457000
#define XSTR_INNER_(x) #x
#define XSTR(x) XSTR_INNER_(x)

namespace mongo {

// Used by `logScopedDebugInfo` below to determine if we should log anything.
Atomic<bool> shouldLogScopedDebugInfoInAssertUtil{true};

void setDiagnosticLoggingInAssertUtil(bool newVal) {
    shouldLogScopedDebugInfoInAssertUtil.store(newVal);
}
namespace {
void logScopedDebugInfo() {
    if (!shouldLogScopedDebugInfoInAssertUtil.load()) {
        return;
    }
    auto diagStack = scopedDebugInfoStack().getAll();
    if (diagStack.empty())
        return;
    LOGV2_FATAL_OPTIONS(
        4106400,
        logv2::LogOptions(logv2::FatalMode::kContinue, logv2::LogTruncation::Disabled),
        "ScopedDebugInfo",
        "scopedDebugInfo"_attr = diagStack);
}

void logErrorBlock() {
    // Logging in this order ensures that the stack trace is printed even if something goes wrong
    // while printing ScopedDebugInfo.
    printStackTrace();
    logScopedDebugInfo();
}

/**
 * Rather than call std::abort directly, assertion and invariant failures that wish to abort the
 * process should call this function, which ensures that std::abort is invoked at most once per
 * process even if multiple threads attempt to abort the process concurrently.
 */
MONGO_COMPILER_NORETURN void callAbort() {
    thread_local int reentry = 0;
    if (reentry++)
        endProcessWithSignal(SIGABRT);

    [[maybe_unused]] static auto initOnce = (std::abort(), 0);
    MONGO_COMPILER_UNREACHABLE;
}
}  // namespace

AssertionCount assertionCount;

AssertionCount::AssertionCount() : regular(0), warning(0), msg(0), user(0), rollovers(0) {}

void AssertionCount::rollover() {
    rollovers.fetchAndAdd(1);
    regular.store(0);
    warning.store(0);
    msg.store(0);
    user.store(0);
}

void AssertionCount::condrollover(int newvalue) {
    static const int rolloverPoint = (1 << 30);
    if (newvalue >= rolloverPoint)
        rollover();
}

void DBException::traceIfNeeded(const DBException& e) {
    const bool traceNeeded = traceExceptions.load() ||
        (e.code() == ErrorCodes::WriteConflict && traceWriteConflictExceptions.load());
    if (traceNeeded) {
        LOGV2_WARNING(23075, "DBException thrown", "error"_attr = e);
        logErrorBlock();
    }
}

namespace {
MONGO_COMPILER_NORETURN void invariantFailedImpl(const char* expr, auto loc) noexcept {
    LOGV2_FATAL_CONTINUE(23079, "Invariant failure", "expr"_attr = expr, "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23080, "\n\n***aborting after invariant() failure\n\n");
    callAbort();
}

MONGO_COMPILER_NORETURN void invariantFailedImpl(const char* expr,
                                                 const std::string& msg,
                                                 auto loc) noexcept {
    LOGV2_FATAL_CONTINUE(
        23081, "Invariant failure", "expr"_attr = expr, "msg"_attr = msg, "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23082, "\n\n***aborting after invariant() failure\n\n");
    callAbort();
}
}  // namespace

#ifdef MONGO_SOURCE_LOCATION_HAVE_STD
MONGO_COMPILER_NOINLINE void invariantFailed(const char* expr,
                                             WrappedStdSourceLocation loc) noexcept {
    invariantFailedImpl(expr, loc);
}

MONGO_COMPILER_NOINLINE void invariantFailedWithMsg(const char* expr,
                                                    const std::string& msg,
                                                    WrappedStdSourceLocation loc) noexcept {
    invariantFailedImpl(expr, msg, loc);
}
#endif  // MONGO_SOURCE_LOCATION_HAVE_STD

MONGO_COMPILER_NOINLINE void invariantFailed(const char* expr,
                                             SyntheticSourceLocation loc) noexcept {
    invariantFailedImpl(expr, loc);
}

MONGO_COMPILER_NOINLINE void invariantFailedWithMsg(const char* expr,
                                                    const std::string& msg,
                                                    SyntheticSourceLocation loc) noexcept {
    invariantFailedImpl(expr, msg, loc);
}

MONGO_COMPILER_NOINLINE void verifyFailed(const char* expr, SourceLocation loc) {
    assertionCount.condrollover(assertionCount.regular.addAndFetch(1));
    LOGV2_ERROR(23076, "Assertion failure", "expr"_attr = expr, "location"_attr = loc);
    logErrorBlock();
    std::stringstream temp;
    temp << "assertion " << loc.file_name() << ":" << loc.line();

    breakpoint();
#if defined(MONGO_CONFIG_DEBUG_BUILD)
    // this is so we notice in buildbot
    LOGV2_FATAL_CONTINUE(
        23078, "\n\n***aborting after verify() failure as this is a debug/test build\n\n");
    callAbort();
#endif
    error_details::throwExceptionForStatus(Status(ErrorCodes::UnknownError, temp.str()));
}

MONGO_COMPILER_NOINLINE void invariantOKFailed(const char* expr,
                                               const Status& status,
                                               SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23083,
                         "Invariant failure",
                         "expr"_attr = expr,
                         "error"_attr = redact(status),
                         "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23084, "\n\n***aborting after invariant() failure\n\n");
    callAbort();
}

MONGO_COMPILER_NOINLINE void invariantOKFailedWithMsg(const char* expr,
                                                      const Status& status,
                                                      const std::string& msg,
                                                      SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23085,
                         "Invariant failure",
                         "expr"_attr = expr,
                         "msg"_attr = msg,
                         "error"_attr = redact(status),
                         "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23086, "\n\n***aborting after invariant() failure\n\n");
    callAbort();
}

MONGO_COMPILER_NOINLINE void invariantStatusOKFailed(const Status& status,
                                                     SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(
        23087, "Invariant failure", "error"_attr = redact(status), "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23088, "\n\n***aborting after invariant() failure\n\n");
    callAbort();
}

namespace fassert_detail {

MONGO_COMPILER_NOINLINE void failed(MsgId msgid, SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23089, "Fatal assertion", "msgid"_attr = msgid.id, "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23090, "\n\n***aborting after fassert() failure\n\n");
    callAbort();
}

MONGO_COMPILER_NORETURN void failed(MsgId msgid,
                                    const Status& status,
                                    SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23093,
                         "Fatal assertion",
                         "msgid"_attr = msgid.id,
                         "error"_attr = redact(status),
                         "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23094, "\n\n***aborting after fassert() failure\n\n");
    callAbort();
}

MONGO_COMPILER_NOINLINE void failedNoTrace(MsgId msgid, SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23091, "Fatal assertion", "msgid"_attr = msgid.id, "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23092, "\n\n***aborting after fassert() failure\n\n");
    quickExit(ExitCode::abrupt);
}
MONGO_COMPILER_NORETURN void failedNoTrace(MsgId msgid,
                                           const Status& status,
                                           SourceLocation loc) noexcept {
    LOGV2_FATAL_CONTINUE(23095,
                         "Fatal assertion",
                         "msgid"_attr = msgid.id,
                         "error"_attr = redact(status),
                         "location"_attr = loc);
    breakpoint();
    LOGV2_FATAL_CONTINUE(23096, "\n\n***aborting after fassert() failure\n\n");
    quickExit(ExitCode::abrupt);
}

}  // namespace fassert_detail

MONGO_COMPILER_NOINLINE void uassertedWithLocation(const Status& status, SourceLocation loc) {
    assertionCount.condrollover(assertionCount.user.addAndFetch(1));
    LOGV2_DEBUG(23074, 1, "User assertion", "error"_attr = redact(status), "location"_attr = loc);
    error_details::throwExceptionForStatus(status);
}

MONGO_COMPILER_NOINLINE void msgassertedWithLocation(const Status& status, SourceLocation loc) {
    assertionCount.condrollover(assertionCount.msg.addAndFetch(1));
    LOGV2_ERROR(23077, "Assertion", "error"_attr = redact(status), "location"_attr = loc);
    error_details::throwExceptionForStatus(status);
}

void iassertFailed(const Status& status, SourceLocation loc) {
    LOGV2_DEBUG(4892201, 3, "Internal assertion", "error"_attr = status, "location"_attr = loc);
    error_details::throwExceptionForStatus(status);
}

void tassertFailed(const Status& status, SourceLocation loc) {
    assertionCount.condrollover(assertionCount.tripwire.addAndFetch(1));
    LOGV2_ERROR(
        TRIPWIRE_ASSERTION_ID, "Tripwire assertion", "error"_attr = status, "location"_attr = loc);
    logErrorBlock();
    breakpoint();
    error_details::throwExceptionForStatus(status);
}

bool haveTripwireAssertionsOccurred() {
    return assertionCount.tripwire.load() != 0;
}

void warnIfTripwireAssertionsOccurred() {
    if (haveTripwireAssertionsOccurred()) {
        LOGV2(4457002,
              "Detected prior failed tripwire assertions, "
              "please check your logs for \"Tripwire assertion\" entries with log "
              "id " XSTR(TRIPWIRE_ASSERTION_ID) ".",
              "occurrences"_attr = assertionCount.tripwire.load());
    }
}

std::string causedBy(StringData e) {
    static constexpr auto prefix = " :: caused by :: "_sd;
    std::string out;
    out.reserve(prefix.size() + e.size());
    out += prefix;
    out += e;
    return out;
}

std::string demangleName(const std::type_info& typeinfo) {
#ifdef _WIN32
    return typeinfo.name();
#else
    int status;

    char* niceName = abi::__cxa_demangle(typeinfo.name(), nullptr, nullptr, &status);
    if (!niceName)
        return typeinfo.name();

    std::string s = niceName;
    free(niceName);
    return s;
#endif
}

Status exceptionToStatus() {
    try {
        throw;
    } catch (const DBException& ex) {
        return ex.toStatus();
    } catch (const std::exception& ex) {
        return Status(ErrorCodes::UnknownError,
                      str::stream() << "Caught std::exception of type " << demangleName(typeid(ex))
                                    << ": " << ex.what());
    } catch (const boost::exception& ex) {
        return Status(ErrorCodes::UnknownError,
                      str::stream()
                          << "Caught boost::exception of type " << demangleName(typeid(ex)) << ": "
                          << boost::diagnostic_information(ex));

    } catch (...) {
        return Status(ErrorCodes::UnknownError, "Caught exception of unknown type");
    }
}

std::vector<std::string> ScopedDebugInfoStack::getAll() {
    if (_loggingDepth > 0) {
        return {};  // Re-entry detected.
    }

    _loggingDepth++;
    ScopeGuard updateDepth = [&] {
        _loggingDepth--;
    };

    std::vector<std::string> r;
    r.reserve(_stack.size());
    for (const auto& e : _stack) {
        try {
            r.push_back(e->toString());
        } catch (...) {
            LOGV2(9513400,
                  "ScopedDebugInfo failed",
                  "label"_attr = e->label(),
                  "error"_attr = describeActiveException());
        }
    }

    return r;
}

void reportFailedDestructor(SourceLocation loc) {
    try {
        throw;
    } catch (...) {
        std::ostringstream oss;
        globalActiveExceptionWitness().describe(oss);
        LOGV2_IMPL(4615600,
                   logv2::LogSeverity::Log(),
                   {logv2::LogComponent::kDefault},
                   "Caught exception in destructor",
                   "exception"_attr = oss.str(),
                   "function"_attr = loc.function_name());
    }
}
}  // namespace mongo
