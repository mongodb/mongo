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

#include "mongo/platform/basic.h"

#include "mongo/util/debugger.h"

#include <cstdlib>
#include <mutex>

#ifndef _WIN32
#include <csignal>
#include <cstdio>
#include <unistd.h>
#endif

#include "mongo/util/debug_util.h"

#ifndef _WIN32
namespace {
std::once_flag breakpointOnceFlag;
}  // namespace
#endif

namespace mongo {
void breakpoint() {
#ifdef _WIN32
    if (IsDebuggerPresent()) {
        DebugBreak();
    };
#endif
#ifndef _WIN32
    // code to raise a breakpoint in GDB
    std::call_once(breakpointOnceFlag, []() {
        // prevent SIGTRAP from crashing the program if default action is specified and we are not
        // in gdb
        struct sigaction current;
        if (sigaction(SIGTRAP, nullptr, &current) != 0) {
            std::abort();
        }
        if (current.sa_handler == SIG_DFL) {
            signal(SIGTRAP, SIG_IGN);
        }
    });

    raise(SIGTRAP);
#endif
}

/* Magic debugging trampoline
 * Do not call directly! call setupSIGTRAPforDebugger()
 * Assumptions:
 *  1) the debugging server binary is on your path
 *  2) You have run "handle SIGSTOP noprint" in gdb
 *  3) serverGlobalParams.port + 2000 is free
 */
namespace {
#ifndef _WIN32
template <typename Exec>
void launchDebugger(Exec debugger) {
    // Don't come back here
    signal(SIGTRAP, SIG_IGN);

    char pidToDebug[16];
    int pidRet = snprintf(pidToDebug, sizeof(pidToDebug), "%d", getpid());
    if (!(pidRet >= 0 && size_t(pidRet) < sizeof(pidToDebug)))
        std::abort();

    char msg[128];
    int msgRet = snprintf(msg,
                          sizeof(msg),
                          "\n\n\t**** Launching debugging server (ensure binary is in PATH, use "
                          "lsof to find port) ****\n\n");
    if (!(msgRet >= 0 && size_t(msgRet) < sizeof(msg)))
        std::abort();

    if (!(write(STDERR_FILENO, msg, msgRet) == msgRet))
        std::abort();

    if (fork() == 0) {
        // child
        debugger(pidToDebug);
        perror(nullptr);
        _exit(1);
    } else {
        // parent
        raise(SIGSTOP);  // pause all threads until debugger connects and continues
        raise(SIGTRAP);  // break inside server
    }
}

#if defined(USE_GDBSERVER)

extern "C" void execCallback(int) {
    launchDebugger([](char* pidToDebug) {
        execlp("gdbserver", "gdbserver", "--attach", ":0", pidToDebug, nullptr);
    });
}

#elif defined(USE_LLDB_SERVER)

extern "C" void execCallback(int) {
    launchDebugger([](char* pidToDebug) {
#ifdef __linux__
        execlp("lldb-server", "lldb-server", "g", "--attach", pidToDebug, "*:12345", nullptr);
#else
        execlp("debugserver", "debugserver", "*:12345", "--attach", pidToDebug, nullptr);
#endif
    });
}

#endif

}  // namespace

void setupSIGTRAPforDebugger() {
#if defined(USE_GDBSERVER) || defined(USE_LLDB_SERVER)
    if (signal(SIGTRAP, execCallback) == SIG_ERR) {
        std::abort();
    }
#endif
#endif
}
}  // namespace mongo
