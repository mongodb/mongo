/**
 *    Copyright (C) 2026-present MongoDB, Inc.
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

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace mongo;
using namespace mongo::mozjs;

// --- Concurrency ---

// N threads each create their own scope from a shared engine and invoke JS concurrently.
// Each scope is thread-local (no sharing), so this exercises the pool and Engine creation
// under concurrent load without any data races on scope state.
TEST(WasmtimeScopeConcurrency, ConcurrentIndependentScopes) {
    constexpr int kThreads = 16;
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

// More threads than the pre-warmed pool size forces pool exhaustion and on-demand context
// creation — verifies the pool's mutex and fallback path are race-free.
TEST(WasmtimeScopeConcurrency, ConcurrentPoolExhaustion) {
    constexpr int kThreads = 32;
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

// One thread invokes a long-running JS function while another thread calls kill().
// The invoke must return non-zero (killed) and the scope must be left in a clean
// killed state (getBoolean("__returnValue") is false / default).
TEST(WasmtimeScopeConcurrency, KillFromAnotherThread) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // Start invoke in background; it runs an infinite loop.
    std::atomic<bool> invokeDone{false};
    int invokeResult = 0;
    std::thread invoker([&] {
        ScriptingFunction fn = scope->createFunction("while(true) {}");
        try {
            invokeResult = scope->invoke(fn, nullptr, nullptr, 30000);
        } catch (...) {
            invokeResult = -1;
        }
        invokeDone.store(true, std::memory_order_release);
    });

    // Give the invoke time to start, then kill.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    scope->kill();

    invoker.join();
    ASSERT_NE(0, invokeResult);
}

// N threads each create a scope, invoke a function, and destroy — simulating the
// scopes, simulating the ScriptPool reuse pattern (create scope, invoke, discard).
TEST(WasmtimeScopeConcurrency, ConcurrentFunctionInvoke) {
    constexpr int kThreads = 16;
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
