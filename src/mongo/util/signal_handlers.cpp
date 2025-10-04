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


#include "mongo/util/signal_handlers.h"

#include <boost/move/utility_core.hpp>
// IWYU pragma: no_include "bits/types/siginfo_t.h"
#include "mongo/base/status.h"
#include "mongo/base/string_data.h"
#include "mongo/config.h"  // IWYU pragma: keep
#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/logv2/log.h"
#include "mongo/logv2/log_util.h"
#include "mongo/platform/random.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/concurrency/thread_name.h"
#include "mongo/util/duration.h"
#include "mongo/util/exit.h"
#include "mongo/util/exit_code.h"
#include "mongo/util/fail_point.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/signal_win32.h"  // IWYU pragma: keep
#include "mongo/util/stacktrace.h"
#include "mongo/util/thread_util.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <ctime>
#include <memory>
#include <random>


#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kControl


namespace mongo {

MONGO_FAIL_POINT_DEFINE(enableSignalTesting);

/*
 * WARNING: PLEASE READ BEFORE CHANGING THIS MODULE
 *
 * All code in this module must be signal-friendly. Before adding any system
 * call or other dependency, please make sure that this still holds.
 *
 * All code in this file follows this pattern:
 *   Generic code
 *   #ifdef _WIN32
 *       Windows code
 *   #else
 *       Posix code
 *   #endif
 *
 */

namespace {

#ifdef _WIN32
void consoleTerminate(const char* controlCodeName) {
    setThreadName("consoleTerminate");
    LOGV2(23371,
          "Received event, will terminate after current command ends",
          "controlCode"_attr = controlCodeName);
    exitCleanly(ExitCode::kill);
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
        case CTRL_C_EVENT:
            LOGV2(23372, "Ctrl-C signal");
            consoleTerminate("CTRL_C_EVENT");
            return TRUE;

        case CTRL_CLOSE_EVENT:
            LOGV2(23373, "CTRL_CLOSE_EVENT signal");
            consoleTerminate("CTRL_CLOSE_EVENT");
            return TRUE;

        case CTRL_BREAK_EVENT:
            LOGV2(23374, "CTRL_BREAK_EVENT signal");
            consoleTerminate("CTRL_BREAK_EVENT");
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            // only sent to services, and only in pre-Vista Windows; FALSE means ignore
            return FALSE;

        case CTRL_SHUTDOWN_EVENT:
            LOGV2(23375, "CTRL_SHUTDOWN_EVENT signal");
            consoleTerminate("CTRL_SHUTDOWN_EVENT");
            return TRUE;

        default:
            return FALSE;
    }
}

void eventProcessingThread() {
    std::string eventName = getShutdownSignalName(ProcessId::getCurrent().asUInt32());

    HANDLE event = CreateEventA(nullptr, TRUE, FALSE, eventName.c_str());
    if (event == nullptr) {
        auto ec = lastSystemError();
        LOGV2_WARNING(
            23382, "eventProcessingThread CreateEvent failed", "error"_attr = errorMessage(ec));
        return;
    }

    ON_BLOCK_EXIT([&] { CloseHandle(event); });

    int returnCode = WaitForSingleObject(event, INFINITE);
    if (returnCode != WAIT_OBJECT_0) {
        if (returnCode == WAIT_FAILED) {
            auto ec = lastSystemError();
            LOGV2_WARNING(23383,
                          "eventProcessingThread WaitForSingleObject failed",
                          "error"_attr = errorMessage(ec));
            return;
        } else {
            auto ec = systemError(returnCode);
            LOGV2_WARNING(23384,
                          "eventProcessingThread WaitForSingleObject failed",
                          "error"_attr = errorMessage(ec));
            return;
        }
    }

    setThreadName("eventTerminate");

    LOGV2(23376, "shutdown event signaled, will terminate after current cmd ends");
    exitCleanly(ExitCode::clean);
}

#else

void noopSignalHandler(int signalNum, siginfo_t*, void*) {}

/**
 * Filled by the `waitForSignal` function.
 * Only Linux has `sigwaitinfo` to synchronously get a signal with its `siginfo_t`.
 * Only Linux has a procfs from which to read metadata for such signal sending process.
 * So outside Linux we just get the `sig`.
 */
struct SignalWaitResult {
    int sig;
#if defined(__linux__)
    siginfo_t si;
#endif  // defined(__linux__)
};

/** Fill `result` with a caught signal from `sigset`. Return true on success. */
bool waitForSignal(const sigset_t& sigset, SignalWaitResult* result) {
    MONGO_IDLE_THREAD_BLOCK;
#if defined(__linux__)
    while (true) {
        errno = 0;
        result->sig = sigwaitinfo(&sigset, &result->si);
        int errsv = errno;
        if (result->sig == -1) {
            if (errsv == EINTR)
                continue;
            LOGV2_FATAL_CONTINUE(
                23385, "sigwaitinfo failed with error", "error"_attr = strerror(errsv));
            return false;
        }
        return true;
    }
#else
    return sigwait(&sigset, &result->sig) == 0;
#endif
}

const int kSignalProcessingThreadExclusives[] = {SIGHUP, SIGINT, SIGTERM, SIGUSR1, SIGXCPU};

struct LogRotationState {
    static constexpr auto kNever = static_cast<time_t>(-1);
    LogFileStatus logFileStatus;
    time_t previous;
};

void handleOneSignal(const SignalWaitResult& waited, LogRotationState* rotation) {
    int sig = waited.sig;
    LOGV2(23377, "Received signal", "signal"_attr = sig, "error"_attr = strsignal(sig));
#if defined(__linux__)
    const siginfo_t& si = waited.si;
    switch (si.si_code) {
        case SI_USER:
        case SI_QUEUE:
            LOGV2(23378,
                  "Signal was sent by kill(2)",
                  "pid"_attr = si.si_pid,
                  "uid"_attr = si.si_uid);
            break;
        case SI_TKILL:
            LOGV2(23379, "Signal was sent by tgkill(2)");
            break;
        case SI_KERNEL:
            LOGV2(23380, "Signal was sent by the kernel");
            break;
    }
#endif  // __linux__

    if (sig == SIGUSR1) {
        // log rotate signal
        {
            // Rate limit: 1 second per signal
            auto now = time(nullptr);
            if (rotation->previous != rotation->kNever && difftime(now, rotation->previous) <= 1.0)
                return;
            rotation->previous = now;
        }

        if (auto status = logv2::rotateLogs(serverGlobalParams.logRenameOnRotate, {}, {});
            !status.isOK()) {
            LOGV2_ERROR(4719800, "Log rotation failed", "error"_attr = status);
        }
        if (rotation->logFileStatus == LogFileStatus::kNeedToRotateLogFile) {
            logProcessDetailsForLogRotate(getGlobalServiceContext());
        }
        return;
    }

#if defined(MONGO_STACKTRACE_HAS_SIGNAL)
    if (sig == stackTraceSignal()) {
        // If there's a stackTraceSignal at all, catch it here so we don't die from it.
        // Can dump all threads if we can, else silently ignore it.
#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
        printAllThreadStacks();
#endif
        return;
    }
#endif

    // interrupt/terminate signal
    LOGV2(23381, "will terminate after current cmd ends");
    exitCleanly(ExitCode::clean);
}

/**
 * The signals in `kSignalProcessingThreadExclusives` will be delivered to this thread only,
 * to ensure the db and log mutexes aren't held.
 */
void signalProcessingThread(LogFileStatus rotate) {
    setThreadName("SignalHandler");

    LogRotationState logRotationState{rotate, logRotationState.kNever};

    sigset_t waitSignals;
    sigemptyset(&waitSignals);

    for (int sig : kSignalProcessingThreadExclusives)
        sigaddset(&waitSignals, sig);

#if defined(MONGO_STACKTRACE_HAS_SIGNAL)
    // On this thread, block the stackTraceSignal and rely on a signal wait to deliver it.
    sigaddset(&waitSignals, stackTraceSignal());
#endif

    errno = 0;
    if (int r = pthread_sigmask(SIG_SETMASK, &waitSignals, nullptr); r != 0) {
        int errsv = errno;
        LOGV2_FATAL(31377, "pthread_sigmask failed with error", "error"_attr = strerror(errsv));
    }

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
    // Must happen after blocking the signal, so this thread doesn't get
    // confused and forward the stack trace signal to itself.
    markAsStackTraceProcessingThread();
#endif

    while (true) {
        SignalWaitResult waited;
        fassert(16781, waitForSignal(waitSignals, &waited));
        handleOneSignal(waited, &logRotationState);
    }
}

#if defined(__linux__)
/**
 * Generates durations to sleep between events in order to simulate
 * independent arrivals at an average arrival rate of 1 per `period`.
 */
class PoissonArrival {
public:
    using DurationType = Microseconds;
    explicit PoissonArrival(DurationType period)
        : _distro{1.0 / durationCount<DurationType>(period)} {}
    DurationType operator()(auto& engine) {
        return DurationType{_distro(engine)};
    }

private:
    std::geometric_distribution<DurationType::rep> _distro;
};

boost::optional<int> chooseVictimTid(auto& engine) {
    std::vector<int> tids;
    iterateTids([&](int tid) { tids.push_back(tid); });
    if (tids.empty())
        return {};
    std::uniform_int_distribution<std::size_t> dist{0, tids.size() - 1};
    return tids[dist(engine)];
}

void signalTestingThread(Milliseconds period) {
    LOGV2(9494400, "Starting signal testing thread", "period"_attr = period);
    int signum = interruptResilienceTestingSignal();
    XorShift128 engine{SecureRandom{}.nextUInt32()};
    PoissonArrival arrivals{period};
    while (true) {
        auto sleepDuration = arrivals(engine);
        sleepFor(sleepDuration);
        auto victimTid = chooseVictimTid(engine);
        if (!victimTid)
            continue;
        LOGV2_DEBUG(9494402,
                    1,
                    "Signalling thread",
                    "tid"_attr = *victimTid,
                    "name"_attr = readThreadName(*victimTid),
                    "sig"_attr = signum,
                    "slept"_attr = sleepDuration);
        terminateThread(getpid(), *victimTid, signum);
    }
}
#endif  // __linux__
#endif
}  // namespace

void setupSignalHandlers() {
    setupSynchronousSignalHandlers();
#ifdef _WIN32
    massert(10297,
            "Couldn't register Windows Ctrl-C handler",
            SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(CtrlHandler), TRUE));
#else
#endif
}

#ifndef _WIN32
void maskSignals(const std::vector<int>& blocked) {
    // The signals that should be handled by the SignalProcessingThread, once it is started via
    // startSignalProcessingThread().
    sigset_t sigset;
    sigemptyset(&sigset);
    for (int sig : blocked)
        sigaddset(&sigset, sig);

    if (pthread_sigmask(SIG_SETMASK, &sigset, nullptr) != 0) {
        auto ec = lastSystemError();
        LOGV2_FATAL(9570503, "Failed to mask signal", "error"_attr = errorMessage(ec));
    }
}

// Reset the action of the passed signals to use the default signal handler.
void resetSignalHandlers(const std::vector<int>& blocked) {
    for (int signum : blocked) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sigemptyset(&sa.sa_mask);
        sa.sa_handler = SIG_DFL;
        if (sigaction(signum, &sa, nullptr) != 0) {
            auto ec = lastSystemError();
            LOGV2_FATAL(9570502,
                        "Failed to reset signal handler",
                        "signal"_attr = signum,
                        "error"_attr = errorMessage(ec));
        }
    }
}
#endif

void startSignalProcessingThread(LogFileStatus rotate) {
#ifdef _WIN32
    stdx::thread(eventProcessingThread).detach();
#else

    // The signals that should be handled by the SignalProcessingThread, once it is started via
    // startSignalProcessingThread().
    std::vector<int> blocked;
    for (int sig : kSignalProcessingThreadExclusives)
        blocked.push_back(sig);

#if defined(MONGO_STACKTRACE_HAS_SIGNAL) && !defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
    // On a Unixlike build without the stacktrace behavior, we still want to handle SIGUSR2 to
    // print a message, but it must only go to the signalProcessingThread, not on other threads.
    blocked.push_back(stackTraceSignal());
#endif

    // Mask signals in the current (only) thread. All new threads will inherit this mask.
    maskSignals(blocked);

    // Use default signal handlers for all of the signals we block. This is because on some systems
    // such as macOS, "if sa_handler is set to SIG_IGN current and pending instances of the signal
    // are ignored and discarded." See macOS `sigaction` man page for more information.
    resetSignalHandlers(blocked);

    // Spawn a thread to capture the signals we just masked off.
    stdx::thread(signalProcessingThread, rotate).detach();
#endif

#if defined(__linux__)
    enableSignalTesting.execute([](const BSONObj& data) {
        auto periodMsElement = data["periodMs"];
        static constexpr Milliseconds defaultPeriod = Milliseconds(25);
        Milliseconds periodMs =
            periodMsElement.eoo() ? defaultPeriod : Milliseconds(periodMsElement.numberInt());
        startSignalTestingThread(periodMs);
    });
#endif  // __linux__
}

#ifdef _WIN32
void removeControlCHandler() {
    massert(28600,
            "Couldn't unregister Windows Ctrl-C handler",
            SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(CtrlHandler), FALSE));
}
#endif

void startSignalTestingThread(Milliseconds periodMs) {
#ifdef __linux__
    stdx::thread(signalTestingThread, periodMs).detach();
#endif
}

}  // namespace mongo
