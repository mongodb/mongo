/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/stdx/thread.h"
#include "mongo/util/exit_code.h"

#include <condition_variable>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <setjmp.h>
#include <stdexcept>

#ifndef _WIN32
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if !MONGO_HAS_SIGALTSTACK

int main() {
    std::cout << "`sigaltstack` testing skipped on this platform." << std::endl;
    return static_cast<int>(mongo::ExitCode::clean);
}

#else  // MONGO_HAS_SIGALTSTACK

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo::stdx {
namespace {

/** Make sure sig is unblocked. */
void unblockSignal(int sig) {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, sig);
    if (sigprocmask(SIG_UNBLOCK, &sigset, nullptr)) {
        perror("sigprocmask");
        exit(static_cast<int>(mongo::ExitCode::fail));
    }
}

extern "C" typedef void(sigAction_t)(int signum, siginfo_t* info, void* context);

/** Install action for signal sig. Be careful to specify SA_ONSTACK. */
void installAction(int sig, sigAction_t* action) {
    struct sigaction sa;
    sa.sa_sigaction = action;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, nullptr)) {
        perror("sigaction");
        exit(static_cast<int>(mongo::ExitCode::fail));
    }
}

void uninstallSigAltStack() {
    // Disable sigaltstack to see what happens. Process should die.
    stack_t ss{};
    ss.ss_flags = SS_DISABLE;
    if (sigaltstack(&ss, nullptr)) {
        perror("uninstall sigaltstack");
        abort();
    }
}

template <typename T>
struct Hex {
    explicit Hex(const T& t) : _t(t) {}
    friend std::ostream& operator<<(std::ostream& os, const Hex& h) {
        return os << std::hex << std::showbase << h._t << std::noshowbase << std::dec;
    }
    const T& _t;
};

struct StackLocationTestChildThreadInfo {
    stack_t ss;
    const char* handlerLocal;
};
static StackLocationTestChildThreadInfo stackLocationTestChildInfo{};

extern "C" void stackLocationTestAction(int, siginfo_t*, void*) {
    char n;
    stackLocationTestChildInfo.handlerLocal = &n;
}

int stackLocationTest() {

    stdx::thread childThread([&] {
        static const int kSignal = SIGUSR1;
        // Use sigaltstack's `old_ss` parameter to query the installed sigaltstack.
        if (sigaltstack(nullptr, &stackLocationTestChildInfo.ss)) {
            perror("sigaltstack");
            abort();
        }
        unblockSignal(kSignal);
        installAction(kSignal, &stackLocationTestAction);
        // `raise` waits for signal handler to complete.
        // https://pubs.opengroup.org/onlinepubs/009695399/functions/raise.html
        raise(kSignal);
    });
    childThread.join();

    if (stackLocationTestChildInfo.ss.ss_flags & SS_DISABLE) {
        std::cerr << "Child thread unexpectedly had sigaltstack disabled." << std::endl;
        exit(static_cast<int>(mongo::ExitCode::fail));
    }

    uintptr_t altStackBegin = reinterpret_cast<uintptr_t>(stackLocationTestChildInfo.ss.ss_sp);
    uintptr_t altStackEnd = altStackBegin + stackLocationTestChildInfo.ss.ss_size;
    uintptr_t handlerLocal = reinterpret_cast<uintptr_t>(stackLocationTestChildInfo.handlerLocal);

    std::cerr << "child sigaltstack[" << Hex(altStackEnd - altStackBegin) << "] = ["
              << Hex(altStackBegin) << ", " << Hex(altStackEnd) << ")\n"
              << "handlerLocal = " << Hex(handlerLocal) << "(sigaltstack + "
              << Hex(handlerLocal - altStackBegin) << ")" << std::endl;
    if (handlerLocal < altStackBegin || handlerLocal >= altStackEnd) {
        std::cerr << "Handler local address " << Hex(handlerLocal) << " was outside of: ["
                  << Hex(altStackBegin) << ", " << Hex(altStackEnd) << ")" << std::endl;
        exit(static_cast<int>(mongo::ExitCode::fail));
    }
    return static_cast<int>(mongo::ExitCode::clean);
}

static sigjmp_buf sigjmp;

extern "C" void siglongjmpAction(int, siginfo_t*, void*) {
    siglongjmp(sigjmp, 1);
}

/**
 * Start a child thread which overflows its stack, causing it to receive a SIGSEGV.  If
 * !useSigAltStack, disable that child thread's sigaltstack.
 *
 * We install a signal handler for SIGSEGV that gives the child thread a way out of the
 * SIGSEGV: it can siglongjmp to a sigsetjmp point before the recursion started. This
 * allows the child thread to recover and exit normally.
 *
 * This can only happen if the signal handler can be activated safely while the thread
 * is in the stack overflow condition. The sigaltstack is what makes it possible to do
 * so. Without sigaltstack, there's no stack space for the signal handler to run, so the
 * SIGSEGV is process-fatal.
 */
int recursionTestImpl(bool useSigAltStack) {

    unblockSignal(SIGSEGV);
    installAction(SIGSEGV, siglongjmpAction);

    stdx::thread childThread([=] {
        if (!useSigAltStack) {
            uninstallSigAltStack();
            std::cout << "child thread uninstalled its sigaltstack" << std::endl;
        }

        struct MostlyInfiniteRecursion {
            // Recurse to run out of stack on purpose. There can be no
            // destructors or AS-unsafe code here, as this function
            // terminates via `siglongjmp`. Hide the recursion via the
            // `recur` callback to frustrate ambitious optimizers.
            void run() {
                if (++depth == std::numeric_limits<size_t>::max())
                    return;  // Avoid the undefined behavior of truly infinite recursion.
                char localVar;
                deepestAddress = &localVar;
                recur();
            }
            size_t depth;
            void* deepestAddress;
            const std::function<void()> recur;
        };
        MostlyInfiniteRecursion recursion = {0, &recursion, [&] { recursion.run(); }};

        // When the signal handler fires, it will return to this sigsetjmp call, causing
        // it to return a nonzero value. This makes the child thread viable again, and
        // it prints a few diagnostics before exiting gracefully.
        // There are special rules about the kinds of expressions in which `setjmp` can appear.
        if (sigsetjmp(sigjmp, 1)) {
            // Nonzero: a fake return from the signal handler's `siglongjmp`.
            ptrdiff_t stackSpan = (const char*)&recursion - (const char*)recursion.deepestAddress;
            std::cout << "Recovered from SIGSEGV after stack depth=" << recursion.depth
                      << ", stack spans approximately " << (1. * stackSpan / (1 << 20))
                      << " MiB.\n";
            std::cout << "That is " << (1. * stackSpan / recursion.depth) << " bytes per frame"
                      << std::endl;
        } else {
            // Does not return, but recovers via signal handler's `siglongjmp`.
            recursion.run();
        }
    });
    childThread.join();
    return static_cast<int>(mongo::ExitCode::clean);
}

/**
 * Cause an infinite recursion to check that the sigaltstack recovery mechanism
 * built into `stdx::thread` works.
 */
int recursionTest() {
    return recursionTestImpl(true);
}

/**
 * Check that stack overflow will crash the process and signal delivery can't happen.
 * Verifies that the sigaltstack is necessary.
 */
int recursionDeathTest() {
    if (pid_t kidPid = fork(); kidPid == -1) {
        perror("fork");
        return static_cast<int>(mongo::ExitCode::fail);
    } else if (kidPid == 0) {
        // Child process: run the recursion test with no sigaltstack protection.
        return recursionTestImpl(false);
    } else {
        // Parent process: expect child to die from a SIGSEGV.
        int wstatus;
        if (pid_t waited = waitpid(kidPid, &wstatus, 0); waited == -1) {
            perror("waitpid");
            return static_cast<int>(mongo::ExitCode::fail);
        }
        if (WIFEXITED(wstatus)) {
            std::cerr << "child unexpectedly exited with: " << WEXITSTATUS(wstatus) << std::endl;
            return static_cast<int>(mongo::ExitCode::fail);
        }
        if (!WIFSIGNALED(wstatus)) {
            std::cerr << "child did not die from a signal" << std::endl;
            return static_cast<int>(mongo::ExitCode::fail);
        }
        int kidSignal = WTERMSIG(wstatus);
        if (kidSignal != SIGSEGV) {
            std::cerr << "child died from the wrong signal: " << kidSignal << std::endl;
            return static_cast<int>(mongo::ExitCode::fail);
        }
        return static_cast<int>(mongo::ExitCode::clean);
    }
}

int runTests() {
    struct Test {
        const char* name;
        int (*func)();
    } static constexpr kTests[] = {
        {"stackLocationTest", &stackLocationTest},
// These tests violate the memory space deliberately, so they generate false
// positives from ASAN. TSAN also fails because we rescue a thread from a
// SIGSEGV due to intentional thread exhaustion with creative use of a longjmp.
// TSAN is more clever than we want here and intercepts the SIGSEGV before we
// can do the thread necromancy that lets us verify it died for the right reasons
// without killing the test harness.
#if !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
        {"recursionTest", &recursionTest},
        {"recursionDeathTest", &recursionDeathTest},
#endif
    };
    for (auto& test : kTests) {
        std::cout << "\n===== " << test.name << " begin:" << std::endl;
        if (int r = test.func(); r != static_cast<int>(mongo::ExitCode::clean)) {
            std::cout << test.name << " FAIL" << std::endl;
            return r;
        }
        std::cout << "===== " << test.name << " PASS" << std::endl;
    }
    return static_cast<int>(mongo::ExitCode::clean);
}

}  // namespace
}  // namespace mongo::stdx

int main() {
    return mongo::stdx::runTests();
}

#endif  // MONGO_HAS_SIGALTSTACK
