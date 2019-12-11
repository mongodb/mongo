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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kControl

#include "mongo/platform/basic.h"

#include "mongo/util/signal_handlers.h"

#include <signal.h>
#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/db/service_context.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/concurrency/idle_thread_block.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/signal_win32.h"
#include "mongo/util/stacktrace.h"

namespace mongo {

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
    log() << "got " << controlCodeName << ", will terminate after current cmd ends";
    exitCleanly(EXIT_KILL);
}

BOOL WINAPI CtrlHandler(DWORD fdwCtrlType) {
    switch (fdwCtrlType) {
        case CTRL_C_EVENT:
            log() << "Ctrl-C signal";
            consoleTerminate("CTRL_C_EVENT");
            return TRUE;

        case CTRL_CLOSE_EVENT:
            log() << "CTRL_CLOSE_EVENT signal";
            consoleTerminate("CTRL_CLOSE_EVENT");
            return TRUE;

        case CTRL_BREAK_EVENT:
            log() << "CTRL_BREAK_EVENT signal";
            consoleTerminate("CTRL_BREAK_EVENT");
            return TRUE;

        case CTRL_LOGOFF_EVENT:
            // only sent to services, and only in pre-Vista Windows; FALSE means ignore
            return FALSE;

        case CTRL_SHUTDOWN_EVENT:
            log() << "CTRL_SHUTDOWN_EVENT signal";
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
        warning() << "eventProcessingThread CreateEvent failed: " << errnoWithDescription();
        return;
    }

    ON_BLOCK_EXIT([&] { CloseHandle(event); });

    int returnCode = WaitForSingleObject(event, INFINITE);
    if (returnCode != WAIT_OBJECT_0) {
        if (returnCode == WAIT_FAILED) {
            warning() << "eventProcessingThread WaitForSingleObject failed: "
                      << errnoWithDescription();
            return;
        } else {
            warning() << "eventProcessingThread WaitForSingleObject failed: "
                      << errnoWithDescription(returnCode);
            return;
        }
    }

    setThreadName("eventTerminate");

    log() << "shutdown event signaled, will terminate after current cmd ends";
    exitCleanly(EXIT_CLEAN);
}

#else

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
            severe() << "sigwaitinfo failed with error:" << strerror(errsv);
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
    log() << "got signal " << sig << " (" << strsignal(sig) << ")";
#ifdef __linux__
    const siginfo_t& si = waited.si;
    switch (si.si_code) {
        case SI_USER:
        case SI_QUEUE:
            log() << "kill from pid:" << si.si_pid << " uid:" << si.si_uid;
            break;
        case SI_TKILL:
            log() << "tgkill";
            break;
        case SI_KERNEL:
            log() << "kernel";
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
        fassert(16782, rotateLogs(serverGlobalParams.logRenameOnRotate, logV2Enabled()));
        if (rotation->logFileStatus == LogFileStatus::kNeedToRotateLogFile) {
            logProcessDetailsForLogRotate(getGlobalServiceContext());
        }
        return;
    }
#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
    if (sig == stackTraceSignal()) {
        auto logObj = log();
        auto& stream = logObj.setIsTruncatable(false).stream();
        OstreamStackTraceSink sink{stream};
        printAllThreadStacks(sink);
        return;
    }
#endif
    // interrupt/terminate signal
    log() << "will terminate after current cmd ends";
    exitCleanly(EXIT_CLEAN);
}

/**
 * The signals in `kSignalProcessingThreadExclusives` will be delivered to this thread only,
 * to ensure the db and log mutexes aren't held.
 */
void signalProcessingThread(LogFileStatus rotate) {
#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
    markAsStackTraceProcessingThread();
#endif
    setThreadName("signalProcessingThread");

    LogRotationState logRotationState{rotate, logRotationState.kNever};

    sigset_t waitSignals;
    sigemptyset(&waitSignals);

    for (int sig : kSignalProcessingThreadExclusives)
        sigaddset(&waitSignals, sig);

#if defined(MONGO_STACKTRACE_CAN_DUMP_ALL_THREADS)
    // On this thread, block the stackTraceSignal and rely on sigwaitinfo to deliver it.
    sigaddset(&waitSignals, stackTraceSignal());
#endif

    errno = 0;
    if (int r = pthread_sigmask(SIG_SETMASK, &waitSignals, nullptr); r != 0) {
        int errsv = errno;
        severe() << "pthread_sigmask failed with error:" << strerror(errsv);
        fassertFailed(31377);
    }

    while (true) {
        SignalWaitResult waited;
        fassert(16781, waitForSignal(waitSignals, &waited));
        handleOneSignal(waited, &logRotationState);
    }
}

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

void startSignalProcessingThread(LogFileStatus rotate) {
#ifdef _WIN32
    stdx::thread(eventProcessingThread).detach();
#else

    // The signals that should be handled by the SignalProcessingThread, once it is started via
    // startSignalProcessingThread().
    sigset_t sigset;
    sigemptyset(&sigset);
    for (int sig : kSignalProcessingThreadExclusives)
        sigaddset(&sigset, sig);

    // Mask signals in the current (only) thread. All new threads will inherit this mask.
    invariant(pthread_sigmask(SIG_SETMASK, &sigset, nullptr) == 0);
    // Spawn a thread to capture the signals we just masked off.
    stdx::thread(signalProcessingThread, rotate).detach();
#endif
}

#ifdef _WIN32
void removeControlCHandler() {
    massert(28600,
            "Couldn't unregister Windows Ctrl-C handler",
            SetConsoleCtrlHandler(static_cast<PHANDLER_ROUTINE>(CtrlHandler), FALSE));
}
#endif

}  // namespace mongo
