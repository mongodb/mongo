/**
 *    Copyright (C) 2014 MongoDB Inc.
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
 *    must comply with the GNU Affero General Public License in all respects for
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

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "mongo/db/log_process_details.h"
#include "mongo/db/server_options.h"
#include "mongo/platform/process_id.h"
#include "mongo/stdx/thread.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/exit.h"
#include "mongo/util/log.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/scopeguard.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/signal_win32.h"

#if defined(_WIN32)
namespace {
const char* strsignal(int signalNum) {
    // should only see SIGABRT on windows
    switch (signalNum) {
        case SIGABRT:
            return "SIGABRT";
        default:
            return "UNKNOWN";
    }
}
}
#endif

namespace mongo {

using std::endl;

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
    log() << "got " << controlCodeName << ", will terminate after current cmd ends" << endl;
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

    HANDLE event = CreateEventA(NULL, TRUE, FALSE, eventName.c_str());
    if (event == NULL) {
        warning() << "eventProcessingThread CreateEvent failed: " << errnoWithDescription();
        return;
    }

    ON_BLOCK_EXIT(CloseHandle, event);

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

// The signals in asyncSignals will be processed by this thread only, in order to
// ensure the db and log mutexes aren't held. Because this is run in a different thread, it does
// not need to be safe to call in signal context.
sigset_t asyncSignals;
void signalProcessingThread() {
    setThreadName("signalProcessingThread");

    while (true) {
        int actualSignal = 0;
        int status = sigwait(&asyncSignals, &actualSignal);
        fassert(16781, status == 0);
        switch (actualSignal) {
            case SIGUSR1:
                // log rotate signal
                fassert(16782, rotateLogs(serverGlobalParams.logRenameOnRotate));
                logProcessDetailsForLogRotate();
                break;
            default:
                // interrupt/terminate signal
                log() << "got signal " << actualSignal << " (" << strsignal(actualSignal)
                      << "), will terminate after current cmd ends" << endl;
                exitCleanly(EXIT_CLEAN);
                break;
        }
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
    // asyncSignals is a global variable listing the signals that should be handled by the
    // interrupt thread, once it is started via startSignalProcessingThread().
    sigemptyset(&asyncSignals);
    sigaddset(&asyncSignals, SIGHUP);
    sigaddset(&asyncSignals, SIGINT);
    sigaddset(&asyncSignals, SIGTERM);
    sigaddset(&asyncSignals, SIGUSR1);
    sigaddset(&asyncSignals, SIGXCPU);
#endif
}

void startSignalProcessingThread() {
#ifdef _WIN32
    stdx::thread(eventProcessingThread).detach();
#else
    // Mask signals in the current (only) thread. All new threads will inherit this mask.
    invariant(pthread_sigmask(SIG_SETMASK, &asyncSignals, 0) == 0);
    // Spawn a thread to capture the signals we just masked off.
    stdx::thread(signalProcessingThread).detach();
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
