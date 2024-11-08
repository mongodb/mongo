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


#include <csignal>
#include <exception>
#include <fmt/format.h>
#include <string>

#include "mongo/base/string_data.h"
#include "mongo/logv2/log.h"
#include "mongo/platform/atomic.h"
#include "mongo/stdx/type_traits.h"
#include "mongo/unittest/assert.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/framework.h"
#include "mongo/util/assert_util.h"
#include "mongo/util/errno_util.h"
#include "mongo/util/fixed_string.h"
#include "mongo/util/quick_exit.h"
#include "mongo/util/signal_handlers_synchronous.h"
#include "mongo/util/str.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest


namespace mongo {
namespace {
using namespace fmt::literals;

// Tests of signals that should be ignored raise each signal twice, to ensure that the handler isn't
// reset.
#define IGNORED_SIGNAL(SIGNUM)           \
    TEST(IgnoredSignalTest, SIGNUM##_) { \
        ASSERT_EQ(0, raise(SIGNUM));     \
        ASSERT_EQ(0, raise(SIGNUM));     \
    }

#define FATAL_SIGNAL(SIGNUM)                                                                   \
    DEATH_TEST(FatalSignalTest, SIGNUM##_, str::stream() << "Got signal: " << SIGNUM << " ") { \
        ASSERT_EQ(0, raise(SIGNUM));                                                           \
    }

#ifdef __linux__
// The si_code field is always SI_TKILL when using raise
#define DUMP_SIGINFO(SIGNUM)                                                                    \
    DEATH_TEST(DumpSiginfoTest, SIGNUM##_, "Dumping siginfo (si_code={}): "_format(SI_TKILL)) { \
        ASSERT_EQ(0, raise(SIGNUM));                                                            \
    }
#else
#define DUMP_SIGINFO(SIGNUM)
#endif

IGNORED_SIGNAL(SIGUSR2)
IGNORED_SIGNAL(SIGHUP)
IGNORED_SIGNAL(SIGPIPE)
FATAL_SIGNAL(SIGQUIT)
FATAL_SIGNAL(SIGILL)
DUMP_SIGINFO(SIGILL)
FATAL_SIGNAL(SIGABRT)

#if !__has_feature(address_sanitizer)
// These signals trip the leak sanitizer
FATAL_SIGNAL(SIGSEGV)
DUMP_SIGINFO(SIGSEGV)
FATAL_SIGNAL(SIGBUS)
DUMP_SIGINFO(SIGBUS)
FATAL_SIGNAL(SIGFPE)
DUMP_SIGINFO(SIGFPE)
#endif

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithoutException,
           "terminate() called. No exception") {
    std::terminate();
}

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithDBException,
           "terminate() called. An exception is active") {
    try {
        uasserted(28720, "Fatal DBException occurrence");
    } catch (...) {
        std::terminate();
    }
}

DEATH_TEST(FatalTerminateTest,
           TerminateIsFatalWithDoubleException,
           "terminate() called. An exception is active") {
    class ThrowInDestructor {
    public:
        ~ThrowInDestructor() {
            uasserted(28721, "Fatal second exception");
        }
    } tid;

    // This is a workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=83400. We should delete
    // this variable once we are on a compiler that doesn't require it.
    volatile bool workaroundGCCBug = true;  // NOLINT
    if (workaroundGCCBug)
        uasserted(28719, "Non-fatal first exception");
}

/**
 * `MallocFreeOStreamGuard` is locked when certain synchronous signal handlers
 * run. Here we test its self-deadlock mitigation.
 */
class MallocFreeOStreamGuardTest : public unittest::Test {
public:
    /**
     * We use a constexpr id and late-bind that to a consistent unused signal
     * number.
     *
     * This abstraction works around SIGRTMIN being a non-constexpr function,
     * but also lets the test logicaly organize the signal numbers it uses.
     */
    template <size_t id>
    struct SignalIndex {
        explicit(false) operator int() const {
            return SIGRTMIN + id;
        }
    };
    template <size_t i>
    static constexpr SignalIndex<i> sig = SignalIndex<i>{};

    static inline Atomic<int> handlerCount{0};

    static void blockSignal(int signo, bool block) {
        sigset_t sigset;
        sigemptyset(&sigset);
        sigaddset(&sigset, signo);
        if (sigprocmask(block ? SIG_BLOCK : SIG_UNBLOCK, &sigset, nullptr) != 0) {
            auto ec = lastSystemError();
            LOGV2_FATAL(9493800,
                        "sigprocmask",
                        "block"_attr = block,
                        "signal"_attr = signo,
                        "error"_attr = errorMessage(ec));
        }
    }

    static void installSignalHandler(int signo, void (*handler)(int)) {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = handler;
        sigemptyset(&sa.sa_mask);
        if (sigaction(signo, &sa, nullptr) != 0) {
            auto ec = lastSystemError();
            LOGV2_FATAL(9493801,
                        "Error installing signal handler",
                        "signal"_attr = signo,
                        "error"_attr = errorMessage(ec));
        }
    }

    static void write(StringData s) {
        logv2::signalSafeWriteToStderr(s);
    }

    /**
     * For the duration of these tests, we'll have a global Deadlock listener installed.
     */
    void setUp() override {
        setMallocFreeOStreamGuardDeadlockCallback_forTest([](int) { write("[deadlock]"); });
    }

    void tearDown() override {
        setMallocFreeOStreamGuardDeadlockCallback_forTest(nullptr);
    }

    static std::shared_ptr<void> makeGuard(int signo) {
        return makeMallocFreeOStreamGuard_forTest(signo);
    }

    template <FixedString message, auto nextAction>
    static void handler(int signo) {
        {
            write(StringData{message});
            auto guard = makeGuard(signo);
            nextAction();
        }
        handlerCount.fetchAndAdd(1);
    }

    template <SignalIndex idx>
    static void thenRaise() {
        raise(idx);
    }

    /** The clean exit will deliberately fail death tests that use this handler. */
    static void thenExitCleanly() {
        quickExit(ExitCode::clean);
    }
};

DEATH_TEST_F(MallocFreeOStreamGuardTest, SecondGuardQuits, "[deadlock]") {
    auto guard1 = makeGuard(sig<0>);
    auto guard2 = makeGuard(sig<1>);
}

DEATH_TEST_F(MallocFreeOStreamGuardTest, DeadlockCounterOnlyIncreases, "[deadlock]") {
    // The deadlock avoidance counter remains incremented after guard dies.
    (void)makeGuard(sig<0>);
    (void)makeGuard(sig<1>);
}

DEATH_TEST_F(MallocFreeOStreamGuardTest, RaiseWithinHandler, "[sig<1>][sig<0>][deadlock]") {
    installSignalHandler(sig<0>, handler<"[sig<0>]", thenExitCleanly>);
    installSignalHandler(sig<1>, handler<"[sig<1>]", thenRaise<sig<0>>>);
    raise(sig<1>);
}

/**
 * Because we made a guard, the `sig<0>` signal handlers will die, and the
 * `exitCleanly` action won't happen.
 */
DEATH_TEST_F(MallocFreeOStreamGuardTest, WithoutBlockedSignal, "[sig<0>][deadlock]") {
    installSignalHandler(sig<0>, handler<"[sig<0>]", thenExitCleanly>);
    auto guard = makeGuard(sig<1>);
    raise(sig<0>);
}

/**
 * Exactly like the `WithoutBlockedSignal` case, but with signal blocking, so
 * the second guard inside the handler is never created. When `sig<0>` is
 * unblocked, the pending `sig<0>` will activate and trigger the deadlock
 * mitigation.
 */
DEATH_TEST_F(MallocFreeOStreamGuardTest, WithBlockedSignal, "[survived][sig<0>][deadlock]") {
    blockSignal(sig<0>, true);
    installSignalHandler(sig<0>, handler<"[sig<0>]", thenExitCleanly>);
    auto guard = makeGuard(sig<1>);

    raise(sig<0>);
    write("[survived]");

    // raise() will return only after the signal handler has returned, so we don't need
    // synchronization on raise calls, but unblocking has no guarantee. The signal handlers
    // may not have run when unblock call returns, so we spin wait.
    auto h0 = handlerCount.load();
    blockSignal(sig<0>, false);
    while (handlerCount.load() == h0)
        std::this_thread::yield();
}

class BlockInsideSignalHandlerTest : public MallocFreeOStreamGuardTest {
public:
    void setUp() override {
        setMallocFreeOStreamGuardDeadlockCallback_forTest([](int) {
            blockSignal(sig<1>, true);
            write("[deadlock]");
        });
    }
};

// Tests that even if the signal is blocked (e.g. by another thread) between the start of the signal
// handler and the deadlock mitigation, the deadlock mitigation ends the process as expected.
DEATH_TEST_F(BlockInsideSignalHandlerTest, ProcessEnds, "[sig<0>][sig<1>][deadlock]") {
    installSignalHandler(sig<0>, handler<"[sig<0>]", thenRaise<sig<1>>>);
    installSignalHandler(sig<1>, handler<"[sig<1>]", thenExitCleanly>);
    raise(sig<0>);
}

}  // namespace
}  // namespace mongo
