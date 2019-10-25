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

#include <condition_variable>
#include <exception>
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
    return EXIT_SUCCESS;
}

#else  // MONGO_HAS_SIGALTSTACK

#if !defined(__has_feature)
#define __has_feature(x) 0
#endif

namespace mongo {
namespace stdx {
namespace {

/** Make sure sig is unblocked. */
void unblockSignal(int sig) {
    sigset_t sigset;
    sigemptyset(&sigset);
    sigaddset(&sigset, sig);
    if (sigprocmask(SIG_UNBLOCK, &sigset, nullptr)) {
        perror("sigprocmask");
        exit(EXIT_FAILURE);
    }
}

/** Install action for signal sig. Be careful to specify SA_ONSTACK. */
void installAction(int sig, void (*action)(int, siginfo_t*, void*)) {
    struct sigaction sa;
    sa.sa_sigaction = action;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    if (sigaction(sig, &sa, nullptr)) {
        perror("sigaction");
        exit(EXIT_FAILURE);
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
struct H {
    explicit H(const T& t) : _t(t) {}
    friend std::ostream& operator<<(std::ostream& os, const H& h) {
        return os << std::hex << std::showbase << h._t << std::noshowbase << std::dec;
    }
    const T& _t;
};

template <typename T>
auto Hex(const T& x) {
    return H<T>{x};
}

int stackLocationTest() {
    struct ChildThreadInfo {
        stack_t ss;
        const char* handlerLocal;
    };
    static ChildThreadInfo childInfo{};

    stdx::thread childThread([&] {
        static const int kSignal = SIGUSR1;
        // Use sigaltstack's `old_ss` parameter to query the installed sigaltstack.
        if (sigaltstack(nullptr, &childInfo.ss)) {
            perror("sigaltstack");
            abort();
        }
        unblockSignal(kSignal);
        installAction(kSignal, [](int, siginfo_t*, void*) {
            char n;
            childInfo.handlerLocal = &n;
        });
        // `raise` waits for signal handler to complete.
        // https://pubs.opengroup.org/onlinepubs/009695399/functions/raise.html
        raise(kSignal);
    });
    childThread.join();

    if (childInfo.ss.ss_flags & SS_DISABLE) {
        std::cerr << "Child thread unexpectedly had sigaltstack disabled." << std::endl;
        exit(EXIT_FAILURE);
    }

    uintptr_t altStackBegin = reinterpret_cast<uintptr_t>(childInfo.ss.ss_sp);
    uintptr_t altStackEnd = altStackBegin + childInfo.ss.ss_size;
    uintptr_t handlerLocal = reinterpret_cast<uintptr_t>(childInfo.handlerLocal);

    std::cerr << "child sigaltstack[" << Hex(altStackEnd - altStackBegin) << "] = ["
              << Hex(altStackBegin) << ", " << Hex(altStackEnd) << ")\n"
              << "handlerLocal = " << Hex(handlerLocal) << "(sigaltstack + "
              << Hex(handlerLocal - altStackBegin) << ")" << std::endl;
    if (handlerLocal < altStackBegin || handlerLocal >= altStackEnd) {
        std::cerr << "Handler local address " << Hex(handlerLocal) << " was outside of: ["
                  << Hex(altStackBegin) << ", " << Hex(altStackEnd) << ")" << std::endl;
        exit(EXIT_FAILURE);
    }
    return EXIT_SUCCESS;
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
    static sigjmp_buf sigjmp;

    unblockSignal(SIGSEGV);
    installAction(SIGSEGV, [](int, siginfo_t*, void*) { siglongjmp(sigjmp, 1); });

    stdx::thread childThread([=] {
        if (!useSigAltStack) {
            uninstallSigAltStack();
            std::cout << "child thread uninstalled its sigaltstack" << std::endl;
        }

        struct MostlyInfiniteRecursion {
            // Recurse to run out of stack on purpose. There can be no destructors or
            // AS-unsafe code here, as this function terminates via `siglongjmp`.
            void run() {
                if (++depth == std::numeric_limits<size_t>::max())
                    return;  // Avoid the undefined behavior of truly infinite recursion.
                char localVar;
                deepestAddress = &localVar;
                run();
            }
            size_t depth;
            void* deepestAddress;
        };
        MostlyInfiniteRecursion recursion = {0, &recursion};

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
    return EXIT_SUCCESS;
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
    pid_t kidPid = fork();
    if (kidPid == -1) {
        perror("fork");
        return EXIT_FAILURE;
    } else if (kidPid == 0) {
        // Child process: run the recursion test with no sigaltstack protection.
        return recursionTestImpl(false);
    } else {
        // Parent process: expect child to die from a SIGSEGV.
        int wstatus;
        pid_t waited = waitpid(kidPid, &wstatus, 0);
        if (waited == -1) {
            perror("waitpid");
            return EXIT_FAILURE;
        }
        if (WIFEXITED(wstatus)) {
            std::cerr << "child unexpectedly exited with: " << WEXITSTATUS(wstatus) << std::endl;
            return EXIT_FAILURE;
        }
        if (!WIFSIGNALED(wstatus)) {
            std::cerr << "child did not die from a signal" << std::endl;
            return EXIT_FAILURE;
        }
        int kidSignal = WTERMSIG(wstatus);
        if (kidSignal != SIGSEGV) {
            std::cerr << "child died from the wrong signal: " << kidSignal << std::endl;
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }
}

int runTests() {
    struct Test {
        const char* name;
        int (*func)();
    } static constexpr kTests[] = {
        {"stackLocationTest", &stackLocationTest},
// These tests violate the memory space deliberately, so they generate false positives from ASAN.
#if !__has_feature(address_sanitizer)
        {"recursionTest", &recursionTest},
        {"recursionDeathTest", &recursionDeathTest},
#endif
    };
    for (auto& test : kTests) {
        std::cout << "\n===== " << test.name << " begin:" << std::endl;
        int r = test.func();
        if (r != EXIT_SUCCESS) {
            std::cout << test.name << " FAIL" << std::endl;
            return r;
        }
        std::cout << "===== " << test.name << " PASS" << std::endl;
    }
    return EXIT_SUCCESS;
}

}  // namespace
}  // namespace stdx
}  // namespace mongo

int main() {
    return mongo::stdx::runTests();
}

#endif  // MONGO_HAS_SIGALTSTACK
