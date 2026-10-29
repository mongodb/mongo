// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/util/debugger.h"

#include "mongo/config.h"  // IWYU pragma: keep

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string_view>

#if defined(MONGO_CONFIG_HAVE_HEADER_UNISTD_H)
#include <unistd.h>
#endif

// If building for WASI/wasm, noop all debugger functionality to avoid POSIX/signal/fork issues.
// See https://github.com/WebAssembly/WASI/issues/166
#if defined(__wasi__) || defined(__WASI__) || defined(__wasm__) || defined(__wasm32__)

namespace mongo {

void breakpoint() {
    // no-op under WASI
}

void setupSIGTRAPforDebugger() {
    // no-op under WASI
}

void waitForDebugger() {
    // no-op under WASI
}

bool isDebuggerActive() {
    return false;
}

}  // namespace mongo

#else  // non-WASI builds

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
}  // namespace

/**
 * If the environment variable "MONGODB_WAIT_FOR_DEBUGGER" is set, then raise SIGSTOP signal
 *
 * A SIGSTOP cannot be caught, blocked or ignored by a process. The SIGSTOP will freeze the process.
 * This gives the debugger time to start, attach and then resume the process.
 */
void waitForDebugger() {
#if WAIT_FOR_DEBUGGER
#ifndef _WIN32
    if (getenv("MONGODB_WAIT_FOR_DEBUGGER")) {
        std::cout << "WAITING FOR DEBUGGER TO ATTACH (pid: " << getpid() << ")..................."
                  << std::endl;
        raise(SIGSTOP);  // pause all threads until debugger connects and continues
    }
#else
#error "Wait for debugger is not supported on Windows"
#endif

#endif
}

// isDebuggerActive code taken from
// https://github.com/catchorg/Catch2/blob/v3.12.0/src/catch2/internal/catch_debugger.cpp
bool isDebuggerActive() {
#if defined(__linux__)
    // Preserve `errno`, which might be clobbered by `std::ifstream` operations.
    struct ErrnoGuard {
        ~ErrnoGuard() {
            errno = saved;
        }
        int saved = errno;
    };

    ErrnoGuard errnoGuard;
    std::ifstream in("/proc/self/status");
    for (std::string line; std::getline(in, line);) {
        static constexpr std::string_view prefix("TracerPid:\t");
        if (line.starts_with(prefix))
            return line.substr(prefix.size()) != "0";
    }

    return false;
#elif defined(_WIN32)
    return IsDebuggerPresent() != 0;
#else
    return false;
#endif
}

}  // namespace mongo
#endif

