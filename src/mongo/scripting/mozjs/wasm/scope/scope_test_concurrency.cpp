// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/client.h"
#include "mongo/db/operation_context.h"
#include "mongo/db/service_context.h"
#include "mongo/db/service_context_test_fixture.h"
#include "mongo/scripting/deadline_monitor_gen.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/transport/mock_session.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"
#include "mongo/util/fail_point.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

using namespace mongo;
using namespace mongo::mozjs;

// --- Concurrency ---

// N threads each create their own scope from a shared engine and invoke JS concurrently.
// Each scope is thread-local (no sharing), so this exercises the pool and Engine creation
// under concurrent load without any data races on scope state.
TEST(WasmtimeScopeConcurrency, ConcurrentIndependentScopes) {
    // 8 threads is enough concurrency to exercise the pool + on-demand Engine creation path
    // while bounding peak memory: each scope holds a ~1.2 GB WASM linear-memory store, so larger
    // counts pile up tens of GB of resident memory and tip slower CI hosts (notably macOS) into
    // swapping, which previously blew the unit-test timeout.
    constexpr int kThreads = 8;
    WasmtimeScriptEngine engine;

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&engine, i, &successCount] {
            std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
            ScriptingFunction fn = scope->createFunction("return 1 + 1;");
            if (scope->invoke(fn, nullptr, nullptr, 5000) == 0 &&
                scope->getNumber("__returnValue") == 2.0) {
                successCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads)
        t.join();

    ASSERT_EQ(successCount.load(), kThreads);
}

// Many threads create scopes concurrently from the single shared WasmEngineContext — verifies that
// concurrent Store instantiation from the shared Engine/Component/Linker (serialised under
// wasmLifecycleMutex()) is race-free. 12 keeps several threads racing while bounding peak memory:
// each scope holds a ~1.2 GB store, so larger counts swap slower CI hosts into the unit-test
// timeout.
TEST(WasmtimeScopeConcurrency, ConcurrentSharedContextScopeCreation) {
    constexpr int kThreads = 12;
    WasmtimeScriptEngine engine;

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&engine, i, &successCount] {
            std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
            ScriptingFunction fn = scope->createFunction("return 1 + 1;");
            if (scope->invoke(fn, nullptr, nullptr, 5000) == 0 &&
                scope->getNumber("__returnValue") == 2.0) {
                successCount.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads)
        t.join();

    ASSERT_EQ(successCount.load(), kThreads);
}

// Multiple threads repeatedly create-reset-destroy scopes from a shared engine to verify
// that concurrent resets don't race on engine or pool state.
TEST(WasmtimeScopeConcurrency, ConcurrentResetCycles) {
    constexpr int kThreads = 8;
    constexpr int kCycles = 3;
    WasmtimeScriptEngine engine;

    std::vector<std::thread> threads;
    std::atomic<int> successCount{0};

    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&engine, &successCount] {
            for (int c = 0; c < kCycles; ++c) {
                std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
                scope->reset();
                ScriptingFunction fn = scope->createFunction("return 42;");
                if (scope->invoke(fn, nullptr, nullptr, 5000) == 0 &&
                    scope->getNumber("__returnValue") == 42.0) {
                    successCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    ASSERT_EQ(successCount.load(), kThreads * kCycles);
}

// A killed scope must refuse further invocations with a non-zero return.
TEST(WasmtimeScopeConcurrency, KillPreventsInvoke) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("return 1;");
    scope->kill();
    int result = 0;
    try {
        result = scope->invoke(fn, nullptr, nullptr, 5000);
    } catch (...) {
        result = -1;
    }
    ASSERT_NE(0, result);
}

// N threads each create a scope, invoke a function, and destroy — simulating the
// scopes, simulating the ScriptPool reuse pattern (create scope, invoke, discard).
TEST(WasmtimeScopeConcurrency, ConcurrentFunctionInvoke) {
    // Bounded to 8 for the same memory reason as ConcurrentIndependentScopes: each concurrent
    // scope holds a ~1.2 GB store, and larger counts swap slower CI hosts into the timeout.
    constexpr int kThreads = 8;
    WasmtimeScriptEngine engine;

    std::atomic<int> successCount{0};
    std::vector<std::thread> threads;
    threads.reserve(kThreads);

    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&engine, i, &successCount] {
            std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
            BSONObj args = BSON("a" << i << "b" << i * 2);
            ScriptingFunction fn =
                scope->createFunction("function(a, b) { return { result: a + b }; }");
            if (scope->invoke(fn, &args, nullptr, 5000) == 0) {
                BSONObj ret = scope->getObject("__returnValue");
                if (ret.getIntField("result") == i + i * 2) {
                    successCount.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    for (auto& t : threads)
        t.join();

    ASSERT_EQ(successCount.load(), kThreads);
}

// ---------------------------------------------------------------------------
// Interrupt translation tests: scope-level opCtx error code propagation
// ---------------------------------------------------------------------------
//
// These tests confirm the end-to-end behaviour: when the opCtx is killed
// with MaxTimeMSExpired before (or during) a JS invocation, the scope must
// throw MaxTimeMSExpired, not Interrupted.
// ---------------------------------------------------------------------------

// A mock session whose connectivity can be flipped from the test, to simulate the client's
// socket closing while an operation is running.
class DisconnectableMockSession : public transport::MockSession {
public:
    DisconnectableMockSession() : MockSession(nullptr) {}

    bool isConnected() override {
        return _connected.load();
    }

    void setConnected(bool connected) {
        _connected.store(connected);
    }

private:
    Atomic<bool> _connected{true};
};

// Fixture that provides a full ServiceContext so we can create OperationContexts.
class WasmtimeScopeInterruptTranslationTest : public unittest::Test,
                                              public ScopedGlobalServiceContextForTest {
protected:
    void setUp() override {
        // Shrink the DeadlineMonitor poll interval from its 1s default: the monitor-poll tests
        // below wait a full interval for a kill to be delivered, and if the poll ever regressed
        // the only backstop is the 60s JS-fn timeout. A small interval keeps the suite fast and
        // turns a regression into a fast failure instead of a 60s hang.
        _savedInterruptIntervalMs = gScriptingEngineInterruptIntervalMS.load();
        gScriptingEngineInterruptIntervalMS.store(20);

        // Register the WASM engine as the global script engine, which is required by
        // registerOperation().
        setGlobalScriptEngine(new WasmtimeScriptEngine());
    }

    void tearDown() override {
        setGlobalScriptEngine(nullptr);
        gScriptingEngineInterruptIntervalMS.store(_savedInterruptIntervalMs);
    }

private:
    int _savedInterruptIntervalMs = 0;
};

// Runs invokerBody in a background thread with a pre-killed OperationContext (marked with
// 'killCode', e.g. MaxTimeMSExpired or CursorKilled) and registerOperation() already called,
// then fires kill() from the calling thread so _opCtx is set when the scope translates
// Interrupted → the real code via _invokeWithDeadlineMonitoring.
//
// The caller is responsible for creating the scope and compiling any function handles
// before calling this helper (see KillFromAnotherThread for the same cross-thread
// scope-usage pattern). _killPending is sticky, so kill() fired a few instructions
// before WASM execution begins still causes the bridge to trap on the first epoch check.
//
// Returns the exception thrown by invokerBody, or nullptr if it returned normally.
template <typename InvokerBody>
static std::exception_ptr runWithKillAndMarkedOpCtx(WasmtimeScopeInterruptTranslationTest& fixture,
                                                    Scope& scope,
                                                    ErrorCodes::Error killCode,
                                                    InvokerBody&& invokerBody) {
    std::mutex killReadyMutex;
    std::condition_variable killReadyCv;
    bool killReady = false;
    std::exception_ptr result;

    std::thread invoker([&] {
        auto client = fixture.getService()->makeClient("interrupt-translation-test");
        AlternativeClientRegion acr(client);
        auto opCtx = cc().makeOperationContext();
        opCtx->markKilled(killCode);
        scope.registerOperation(opCtx.get());
        {
            std::lock_guard<std::mutex> lk(killReadyMutex);
            killReady = true;
        }
        killReadyCv.notify_one();
        try {
            invokerBody(scope);
        } catch (...) {
            result = std::current_exception();
        }
        scope.unregisterOperation();
    });

    {
        std::unique_lock<std::mutex> lk(killReadyMutex);
        killReadyCv.wait(lk, [&] { return killReady; });
    }
    // Mirrors WasmtimeScriptEngine::interrupt(): the caller already knows opCtx's kill code
    // (it's what we just marked it with above), so pass it straight through via
    // killWithReason() rather than the reason-less kill() -- exactly as production does.
    static_cast<WasmtimeImplScope&>(scope).killWithReason(killCode);
    invoker.join();
    return result;
}

// Asserts that 'ex' is a DBException with the given code, failing with 'opName' in the message
// otherwise.
static void assertTranslatedCode(std::exception_ptr ex,
                                 ErrorCodes::Error expectedCode,
                                 std::string_view opName) {
    ASSERT(ex) << opName << " should have thrown";
    try {
        std::rethrow_exception(ex);
    } catch (const DBException& e) {
        ASSERT_EQ(e.code(), expectedCode)
            << "Expected " << ErrorCodes::errorString(expectedCode) << ", got: " << e.toString();
    } catch (...) {
        FAIL(std::string(opName) + " threw an unexpected exception type");
    }
}

// invoke() with a looping predicate (recv != nullptr) translates Interrupted → the real code,
// for both of the codes the bridge/scope contract calls out as needing translation:
// MaxTimeMSExpired (query timeout) and CursorKilled (an explicit killCursors on the operation).
TEST_F(WasmtimeScopeInterruptTranslationTest, Invoke_PredicateTranslatesInterruptedToRealCode) {
    for (auto killCode : {ErrorCodes::MaxTimeMSExpired, ErrorCodes::CursorKilled}) {
        BSONObj doc = BSON("x" << 1);
        WasmtimeScriptEngine engine;
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ScriptingFunction fn =
            scope->createFunction("function() { while (true) {}; return true; }");
        auto ex = runWithKillAndMarkedOpCtx(*this, *scope, killCode, [&](Scope& s) {
            s.invoke(fn, nullptr, &doc, /*timeoutMs=*/60000);
        });
        assertTranslatedCode(ex, killCode, "invoke() predicate path");
    }
}

// execPredicate() translates Interrupted → the real code.
TEST_F(WasmtimeScopeInterruptTranslationTest, ExecPredicateTranslatesInterruptedToRealCode) {
    for (auto killCode : {ErrorCodes::MaxTimeMSExpired, ErrorCodes::CursorKilled}) {
        BSONObj doc = BSON("x" << 1);
        WasmtimeScriptEngine engine;
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ScriptingFunction fn =
            scope->createFunction("function() { while (true) {}; return true; }");
        auto ex = runWithKillAndMarkedOpCtx(*this, *scope, killCode, [&](Scope& s) {
            static_cast<WasmtimeImplScope*>(&s)->execPredicate(fn, doc, /*timeoutMs=*/60000);
        });
        assertTranslatedCode(ex, killCode, "execPredicate()");
    }
}

// invoke() with a regular function (no recv) translates Interrupted → the real code.
TEST_F(WasmtimeScopeInterruptTranslationTest, Invoke_FunctionTranslatesInterruptedToRealCode) {
    for (auto killCode : {ErrorCodes::MaxTimeMSExpired, ErrorCodes::CursorKilled}) {
        WasmtimeScriptEngine engine;
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = scope->createFunction("while (true) {}");
        auto ex = runWithKillAndMarkedOpCtx(*this, *scope, killCode, [&](Scope& s) {
            s.invoke(fn, nullptr, nullptr, /*timeoutMs=*/60000);
        });
        assertTranslatedCode(ex, killCode, "invoke() function path");
    }
}

// Covers the scope's own JS-fn-only timeout path: when WasmtimeImplScope's internal
// DeadlineMonitor is what triggers kill() -- not the opCtx being externally marked -- the plain,
// reason-less kill() call defaults the bridge's recorded kill reason to Interrupted, and the
// bridge throws exactly that from _throwAfterTrap(). This must hold regardless of how much of the
// opCtx's own maxTimeMS remains: this scope's JS-fn timeout is never a client-facing maxTimeMS.
// Unlike runWithKillAndMarkedOpCtx, this never calls scope.kill()/killWithReason() or
// opCtx->markKilled() itself: the short 'timeoutMs' passed to invoke() lets the DeadlineMonitor
// fire the kill on its own, exactly as it would for a real $where/$function that runs past
// internalQueryJavaScriptFnTimeoutMillis while comfortably inside a much longer maxTimeMS.
TEST_F(WasmtimeScopeInterruptTranslationTest,
       JsFnTimeoutWithUnexpiredOpCtxDeadlineStaysInterrupted) {
    auto client = getService()->makeClient("interrupt-translation-fallback-test");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    // A real, far-future maxTimeMS, to make explicit that it plays no role in this scenario.
    opCtx->setDeadlineAfterNowBy(Hours(1), ErrorCodes::MaxTimeMSExpired);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    scope->registerOperation(opCtx.get());
    ScriptingFunction fn = scope->createFunction("function() { while (true) {}; return true; }");

    std::exception_ptr result;
    try {
        // Short JS-fn timeout so the scope's own DeadlineMonitor -- not the opCtx -- is what
        // calls kill() here.
        scope->invoke(fn, nullptr, nullptr, /*timeoutMs=*/200);
    } catch (...) {
        result = std::current_exception();
    }
    scope->unregisterOperation();

    assertTranslatedCode(result, ErrorCodes::Interrupted, "invoke() under a JS-fn-only timeout");
}

TEST_F(WasmtimeScopeInterruptTranslationTest, RegisterOperationForwardsRealCodeWhenAlreadyKilled) {
    for (auto killCode : {ErrorCodes::MaxTimeMSExpired, ErrorCodes::CursorKilled}) {
        auto client = getService()->makeClient("register-operation-already-killed-test");
        AlternativeClientRegion acr(client);
        auto opCtx = cc().makeOperationContext();

        WasmtimeScriptEngine engine;
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = scope->createFunction("return true;");

        // Mark the opCtx killed *before* registering, simulating the deadline having expired in
        // between two consecutive per-document predicate calls.
        opCtx->markKilled(killCode);
        scope->registerOperation(opCtx.get());

        std::exception_ptr result;
        try {
            scope->invoke(fn, nullptr, nullptr, /*timeoutMs=*/60000);
        } catch (...) {
            result = std::current_exception();
        }
        scope->unregisterOperation();

        assertTranslatedCode(
            result, killCode, "invoke() after registerOperation() found opCtx already killed");
    }
}

// A kill that lands after the opCtx's own deadline has already expired must surface as the
// deadline error (e.g. MaxTimeMSExpired), not the killer's code. This mirrors
// OperationContext::checkForInterruptNoAssert(), which checks hasDeadlineExpired() before
// consulting the recorded kill status, and matches the native engine (MozJSImplScope::kill()
// re-derives the status via checkForInterruptNoAssert()). Regression test for BF-44481: a
// $where stuck in sleep() past maxTimeMS was later killed via killCursors, and the getMore
// surfaced CursorKilled instead of MaxTimeMSExpired, breaking allowPartialResults on mongos.
TEST_F(WasmtimeScopeInterruptTranslationTest, ExpiredDeadlineOutranksLaterKillCode) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    // The client and opCtx are owned by this thread, not the invoker: the DeadlineMonitor's
    // opCtx poll may deliver the expired deadline on its own, letting the invoker finish (and,
    // if it owned them, destroy the opCtx) while this thread is still using it below.
    auto client = getService()->makeClient("deadline-vs-kill-code-test");
    auto opCtx = client->makeOperationContext();
    // A short, real maxTimeMS that will expire while the JS below is still running. The op
    // thread never calls checkForInterrupt() while inside JS, so nothing records
    // MaxTimeMSExpired on the opCtx before the external kill arrives.
    opCtx->setDeadlineAfterNowBy(Milliseconds(200), ErrorCodes::MaxTimeMSExpired);

    std::exception_ptr result;
    std::thread invoker([&] {
        scope->registerOperation(opCtx.get());
        try {
            scope->invoke(fn, nullptr, nullptr, /*timeoutMs=*/60000);
        } catch (...) {
            result = std::current_exception();
        }
        scope->unregisterOperation();
    });

    // Let the maxTimeMS deadline lapse while the op thread is stuck in JS.
    while (opCtx->getRemainingMaxTimeMillis() > Milliseconds::zero()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Deliver an external kill with a different code, exactly as a killCursors would in
    // production: markKilled + engine interrupt, both under the client lock. The
    // DeadlineMonitor's own opCtx poll may already have delivered the expired deadline by now;
    // either way the surfaced error must be the deadline's code, not the killer's.
    {
        ClientLock clientLock(client.get());
        opCtx->markKilled(ErrorCodes::CursorKilled);
        engine.interrupt(clientLock, opCtx.get());
    }
    invoker.join();

    assertTranslatedCode(
        result, ErrorCodes::MaxTimeMSExpired, "invoke() killed after deadline expiry");
}

// ---------------------------------------------------------------------------
// DeadlineMonitor opCtx-poll tests (SERVER-130767)
// ---------------------------------------------------------------------------
//
// Plain markKilled() (the baton client-disconnect path), silent deadline expiry, and a client
// disconnect that nothing ever records on the opCtx all bypass the kill-op listeners, so no one
// calls killWithReason() on the scope. The scope's isKillPending()/kill() consult the registered
// opCtx so that the DeadlineMonitor's periodic pass (every scriptingEngineInterruptIntervalMS,
// default 1s) delivers these kills to a thread stuck inside JS.
// ---------------------------------------------------------------------------

namespace {

// Runs 'while (true) {}' in a background thread with 'opCtx' registered on the scope, invokes
// 'triggerKill' from the calling thread once the invoker is up, and returns the exception the
// invocation threw. No explicit kill()/killWithReason()/engine.interrupt() is ever issued --
// only the DeadlineMonitor's periodic opCtx poll can stop the JS.
template <typename TriggerKill>
std::exception_ptr runUntilMonitorDeliversKill(Scope& scope,
                                               ScriptingFunction fn,
                                               OperationContext* opCtx,
                                               TriggerKill&& triggerKill) {
    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool ready = false;
    std::exception_ptr result;

    std::thread invoker([&] {
        scope.registerOperation(opCtx);
        {
            std::lock_guard<std::mutex> lk(readyMutex);
            ready = true;
        }
        readyCv.notify_one();
        try {
            scope.invoke(fn, nullptr, nullptr, /*timeoutMs=*/60000);
        } catch (...) {
            result = std::current_exception();
        }
        scope.unregisterOperation();
    });

    {
        std::unique_lock<std::mutex> lk(readyMutex);
        readyCv.wait(lk, [&] { return ready; });
    }
    triggerKill();
    invoker.join();
    return result;
}

}  // namespace

// A plain markKilled() with no kill-op listener notification (exactly what the networking baton
// does when the client socket closes) must still stop running JS, with the marked code.
TEST_F(WasmtimeScopeInterruptTranslationTest, MonitorPollDeliversPlainMarkKilled) {
    auto client = getService()->makeClient("monitor-poll-mark-killed-test");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    auto ex = runUntilMonitorDeliversKill(*scope, fn, opCtx.get(), [&] {
        // markKilled() from a non-operation thread requires the Client lock, exactly as
        // ServiceContext::killOperation() holds it in production.
        ClientLock clientLock(opCtx->getClient());
        opCtx->markKilled(ErrorCodes::CursorKilled);
    });
    assertTranslatedCode(ex, ErrorCodes::CursorKilled, "invoke() after plain markKilled()");
}

// A maxTimeMS deadline that expires while the op is stuck inside JS must stop the JS within the
// monitor's poll interval -- nothing ever records the expiry on the opCtx, so only the poll can
// discover it.
TEST_F(WasmtimeScopeInterruptTranslationTest, MonitorPollDeliversSilentDeadlineExpiry) {
    auto client = getService()->makeClient("monitor-poll-deadline-test");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Milliseconds(300), ErrorCodes::MaxTimeMSExpired);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    auto ex = runUntilMonitorDeliversKill(*scope, fn, opCtx.get(), [] {});
    assertTranslatedCode(
        ex, ErrorCodes::MaxTimeMSExpired, "invoke() past a silently expired deadline");
}

// A disconnected client session must stop running JS with the client's disconnect error code,
// even though the disconnect is never recorded on the opCtx by anyone (SERVER-130767).
TEST_F(WasmtimeScopeInterruptTranslationTest, MonitorPollDeliversClientDisconnect) {
    auto session = std::make_shared<DisconnectableMockSession>();
    auto client = getService()->makeClient("monitor-poll-disconnect-test", session);
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    auto ex =
        runUntilMonitorDeliversKill(*scope, fn, opCtx.get(), [&] { session->setConnected(false); });
    assertTranslatedCode(
        ex, cc().getDisconnectErrorCode(), "invoke() after client session disconnected");
}

// The maxTimeNeverTimeOut failpoint must suppress deadline expiry in scriptKillReasonFor() exactly
// as it does in OperationContext: an op with a lapsed deadline that is then killed with a different
// code must surface that code, not the (suppressed) deadline error. This pins the
// maxTimeNeverTimeOut branch of opCtxDeadlineHasExpired().
TEST_F(WasmtimeScopeInterruptTranslationTest, MaxTimeNeverTimeOutSuppressesDeadline) {
    auto client = getService()->makeClient("monitor-poll-never-timeout-test");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    opCtx->setDeadlineAfterNowBy(Milliseconds(100), ErrorCodes::MaxTimeMSExpired);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    FailPointEnableBlock fp("maxTimeNeverTimeOut");
    auto ex = runUntilMonitorDeliversKill(*scope, fn, opCtx.get(), [&] {
        // Give the deadline time to lapse in wall-clock terms, then kill with a non-deadline code.
        // With the deadline suppressed, that code -- not MaxTimeMSExpired -- must win.
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        ClientLock clientLock(opCtx->getClient());
        opCtx->markKilled(ErrorCodes::CursorKilled);
    });
    assertTranslatedCode(ex, ErrorCodes::CursorKilled, "invoke() with maxTimeNeverTimeOut");
}

// The maxTimeAlwaysTimeOut failpoint must force deadline expiry in scriptKillReasonFor() before the
// wall-clock deadline arrives: an op with a far-future deadline is killed with its timeout error
// via the monitor poll. This pins the maxTimeAlwaysTimeOut branch of opCtxDeadlineHasExpired().
TEST_F(WasmtimeScopeInterruptTranslationTest, MaxTimeAlwaysTimeOutForcesDeadline) {
    auto client = getService()->makeClient("monitor-poll-always-timeout-test");
    AlternativeClientRegion acr(client);
    auto opCtx = cc().makeOperationContext();
    // A deadline far enough out that only the failpoint, not the clock, can trip it.
    opCtx->setDeadlineAfterNowBy(Hours(1), ErrorCodes::MaxTimeMSExpired);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("while (true) {}");

    FailPointEnableBlock fp("maxTimeAlwaysTimeOut");
    auto ex = runUntilMonitorDeliversKill(*scope, fn, opCtx.get(), [] {});
    assertTranslatedCode(ex, ErrorCodes::MaxTimeMSExpired, "invoke() with maxTimeAlwaysTimeOut");
}

// ---------------------------------------------------------------------------
// Epoch bleed test
// ---------------------------------------------------------------------------
//
// Without the per-bridge epoch_deadline_callback, killing one scope calls
// increment_epoch() on the shared engine and interrupts every other
// concurrently-running scope.  The callback re-arms non-killed bridges so
// they continue executing.  This test verifies that property at the scope
// layer (the bridge-level equivalent is EpochIsolation_BridgeBCompletesWhile
// BridgeAIsKilled in bridge_test.cpp, which is disabled under TSan due to an
// unrelated wasmtime signal-handler issue; this test does not exercise the
// store limiter so it is TSan-safe).
// ---------------------------------------------------------------------------

// Killing scope A must not prevent scope B from executing on the calling thread.
TEST(WasmtimeScopeConcurrency, EpochBleed_KillingOneScopeDoesNotInterruptAnother) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scopeA(engine.createScopeForCurrentThread(boost::none));
    std::unique_ptr<Scope> scopeB(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fnA = scopeA->createFunction("while (true) {}");
    ScriptingFunction fnB = scopeB->createFunction("return 42;");

    std::mutex readyMutex;
    std::condition_variable readyCv;
    bool invokeReady = false;

    std::thread threadA([&] {
        {
            std::lock_guard<std::mutex> lk(readyMutex);
            invokeReady = true;
        }
        readyCv.notify_one();
        try {
            scopeA->invoke(fnA, nullptr, nullptr, /*timeoutMs=*/30000);
        } catch (...) {
        }
    });

    {
        std::unique_lock<std::mutex> lk(readyMutex);
        readyCv.wait(lk, [&] { return invokeReady; });
    }
    scopeA->kill();
    threadA.join();

    // Without the per-bridge epoch callback, increment_epoch() would have advanced the global
    // epoch to B's deadline, causing B to trap on its first epoch check (epoch >= deadline).
    bool scopeBSucceeded = (scopeB->invoke(fnB, nullptr, nullptr, /*timeoutMs=*/30000) == 0);
    ASSERT_TRUE(scopeBSucceeded) << "Killing scope A must not interrupt scope B (epoch bleed)";
}
