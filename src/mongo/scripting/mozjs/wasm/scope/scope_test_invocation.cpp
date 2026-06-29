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

#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

#include <vector>

using namespace mongo;
using namespace mongo::mozjs;

TEST(WasmtimeScope, CreateAndInvoke_SimpleReturn_ProxyAndThreadLocal) {
    WasmtimeScriptEngine engine;

    std::unique_ptr<Scope> implScope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(implScope);

    ScriptingFunction fn = implScope->createFunction("return 42;");
    ASSERT(fn != 0);
    ASSERT_EQ(0, implScope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42, implScope->getNumber("__returnValue"));

    std::unique_ptr<Scope> proxyScope(engine.createScope());
    ASSERT(proxyScope);

    ScriptingFunction pfn = proxyScope->createFunction("return 7;");
    ASSERT(pfn != 0);
    ASSERT_EQ(0, proxyScope->invoke(pfn, nullptr, nullptr, 0));
    ASSERT_EQ(7, proxyScope->getNumber("__returnValue"));
}

// $where pattern: invoke(func, nullptr, &document) with document as `this`
TEST(WasmtimeScope, WherePattern_PredicateWithRecv) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("function() { return this.age >= 18; }");
    ASSERT(fn != 0);

    BSONObj doc1 = BSON("age" << 25);
    ASSERT_EQ(0, scope->invoke(fn, nullptr, &doc1, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));

    BSONObj doc2 = BSON("age" << 10);
    ASSERT_EQ(0, scope->invoke(fn, nullptr, &doc2, 0));
    ASSERT_FALSE(scope->getBoolean("__returnValue"));
}

// $function/$accumulator pattern: invoke(func, &args, nullptr)
TEST(WasmtimeScope, FunctionPattern_WithArgs) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("function(a, b) { return { sum: a + b }; }");
    ASSERT(fn != 0);

    BSONObj args = BSON("a" << 10 << "b" << 32);
    ASSERT_EQ(0, scope->invoke(fn, &args, nullptr, 0));

    BSONObj retVal = scope->getObject("__returnValue");
    ASSERT_EQ(retVal.getIntField("sum"), 42);
}

// 'this' inside $function must be an empty plain object, not the global.
TEST(WasmtimeScope, FunctionPattern_ThisIsEmpty) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn =
        scope->createFunction("function(obj) { return Object.getOwnPropertyNames(this).length; }");
    ASSERT(fn != 0);

    BSONObj args = BSON("obj" << BSON("x" << 1));
    ASSERT_EQ(0, scope->invoke(fn, &args, nullptr, 0));
    // 'this' must be a fresh empty object — zero own properties.
    ASSERT_EQ(scope->getNumberInt("__returnValue"), 0);
}

// hex_md5 must be available as a global inside $function bodies, matching the
// legacy MozJS engine behavior from installGlobalUtils(). Regression test for
// the missing hex_md5 global in the WASM engine's SpiderMonkey scope.
TEST(WasmtimeScope, FunctionPattern_HexMd5Available) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("function(s) { return hex_md5(s); }");
    ASSERT(fn != 0);

    BSONObj args = BSON("s" << "hello");
    ASSERT_EQ(0, scope->invoke(fn, &args, nullptr, 0));
    // MD5("hello") = 5d41402abc4b2a76b9719d911017c592
    ASSERT_EQ(scope->getString("__returnValue"), "5d41402abc4b2a76b9719d911017c592");
}

// $accumulator init/accumulate/merge pattern
TEST(WasmtimeScope, AccumulatorPattern_InitAccumulateMerge) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction initFn = scope->createFunction("function() { return 0; }");
    ASSERT(initFn != 0);

    ScriptingFunction accFn = scope->createFunction("function(state, val) { return state + val; }");
    ASSERT(accFn != 0);

    ScriptingFunction mergeFn = scope->createFunction("function(s1, s2) { return s1 + s2; }");
    ASSERT(mergeFn != 0);

    // init
    BSONObj emptyArgs;
    ASSERT_EQ(0, scope->invoke(initFn, &emptyArgs, nullptr, 0));
    double state = scope->getNumber("__returnValue");
    ASSERT_EQ(state, 0.0);

    // accumulate: state + 10
    BSONObj accArgs1 = BSON("0" << state << "1" << 10);
    ASSERT_EQ(0, scope->invoke(accFn, &accArgs1, nullptr, 0));
    state = scope->getNumber("__returnValue");
    ASSERT_EQ(state, 10.0);

    // accumulate: state + 20
    BSONObj accArgs2 = BSON("0" << state << "1" << 20);
    ASSERT_EQ(0, scope->invoke(accFn, &accArgs2, nullptr, 0));
    state = scope->getNumber("__returnValue");
    ASSERT_EQ(state, 30.0);

    // merge: 30 + 12
    BSONObj mergeArgs = BSON("0" << state << "1" << 12.0);
    ASSERT_EQ(0, scope->invoke(mergeFn, &mergeArgs, nullptr, 0));
    double merged = scope->getNumber("__returnValue");
    ASSERT_EQ(merged, 42.0);
}

// $accumulator with object state.
// State is {buckets:[n,n,n,n,n], count:N}.
// Verifies that object-state accumulators work correctly across many iterations.
TEST(WasmtimeScope, AccumulatorPattern_ObjectState) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction initFn =
        scope->createFunction("function() { return { buckets: [0,0,0,0,0], count: 0 }; }");
    ASSERT(initFn != 0);

    ScriptingFunction accFn = scope->createFunction(
        "function(state, doc) {"
        "  var val = (typeof doc === 'object' && doc !== null && 'value' in doc) ? doc.value : "
        "+doc;"
        "  var b = Math.min(4, Math.floor(val * 5));"
        "  state.buckets[b]++;"
        "  state.count++;"
        "  return state;"
        "}");
    ASSERT(accFn != 0);

    ScriptingFunction mergeFn = scope->createFunction(
        "function(s1, s2) {"
        "  for (var i = 0; i < 5; i++) s1.buckets[i] += s2.buckets[i];"
        "  s1.count += s2.count;"
        "  return s1;"
        "}");
    ASSERT(mergeFn != 0);

    ScriptingFunction finalFn = scope->createFunction("function(s) { return s; }");
    ASSERT(finalFn != 0);

    // init
    BSONObj emptyArgs;
    ASSERT_EQ(0, scope->invoke(initFn, &emptyArgs, nullptr, 0));
    BSONObj state = scope->getObject("__returnValue");

    // accumulate 100 docs with value=0.1 (bucket 0)
    for (int i = 0; i < 100; i++) {
        BSONObj args = BSON("0" << state << "1" << BSON("value" << 0.1));
        ASSERT_EQ(0, scope->invoke(accFn, &args, nullptr, 0));
        state = scope->getObject("__returnValue");
    }
    // accumulate 50 docs with value=0.9 (bucket 4)
    for (int i = 0; i < 50; i++) {
        BSONObj args = BSON("0" << state << "1" << BSON("value" << 0.9));
        ASSERT_EQ(0, scope->invoke(accFn, &args, nullptr, 0));
        state = scope->getObject("__returnValue");
    }

    // build a second partial state via init+accumulate and then merge
    ASSERT_EQ(0, scope->invoke(initFn, &emptyArgs, nullptr, 0));
    BSONObj state2 = scope->getObject("__returnValue");
    for (int i = 0; i < 25; i++) {
        BSONObj args = BSON("0" << state2 << "1" << BSON("value" << 0.5));
        ASSERT_EQ(0, scope->invoke(accFn, &args, nullptr, 0));
        state2 = scope->getObject("__returnValue");
    }

    // merge
    BSONObj mergeArgs = BSON("0" << state << "1" << state2);
    ASSERT_EQ(0, scope->invoke(mergeFn, &mergeArgs, nullptr, 0));
    BSONObj merged = scope->getObject("__returnValue");

    // finalize
    BSONObj finalArgs = BSON("0" << merged);
    ASSERT_EQ(0, scope->invoke(finalFn, &finalArgs, nullptr, 0));
    BSONObj result = scope->getObject("__returnValue");

    // 100 (bucket0) + 50 (bucket4) + 25 (bucket2) = 175 total
    ASSERT_EQ(result["count"].numberInt(), 175);
    ASSERT_EQ(result["buckets"].embeddedObject()["0"].numberInt(), 100);  // 0.1 → bucket 0
    ASSERT_EQ(result["buckets"].embeddedObject()["4"].numberInt(), 50);   // 0.9 → bucket 4
    ASSERT_EQ(result["buckets"].embeddedObject()["2"].numberInt(), 25);   // 0.5 → bucket 2
}

// mapReduce.map pattern: injectNative("emit", ...) + invoke(func, nullptr, &doc, timeout, true)
TEST(WasmtimeScope, MapReducePattern_EmitAndDrain) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    struct EmitState {
        std::vector<BSONObj> emitted;
    };
    EmitState emitState;

    NativeFunction emitFn = [](const BSONObj& args, void* data) -> BSONObj {
        auto* state = static_cast<EmitState*>(data);
        state->emitted.push_back(args.getOwned());
        return BSONObj();
    };

    scope->injectNative("emit", emitFn, &emitState);

    ScriptingFunction mapFn =
        scope->createFunction("function() { emit(this.category, this.price); }");
    ASSERT(mapFn != 0);

    BSONObj doc1 = BSON("category" << "electronics" << "price" << 100);
    BSONObj doc2 = BSON("category" << "books" << "price" << 25);

    ASSERT_EQ(0, scope->invoke(mapFn, nullptr, &doc1, 0, true));
    ASSERT_EQ(0, scope->invoke(mapFn, nullptr, &doc2, 0, true));

    ASSERT_EQ(emitState.emitted.size(), 2u);
    ASSERT_EQ(emitState.emitted[0]["k"].str(), "electronics");
    ASSERT_EQ(emitState.emitted[0]["v"].numberInt(), 100);
    ASSERT_EQ(emitState.emitted[1]["k"].str(), "books");
    ASSERT_EQ(emitState.emitted[1]["v"].numberInt(), 25);
}

// A scope that called setupEmit must NOT be parked in the idle pool.
// WASM linear memory only grows; each setupEmit allocates ~117 MB that is never freed.
TEST(WasmtimeScope, EmitConfiguredBridge_IsNotParkedOnScopeTeardown) {
    GlobalEngineGuard engineGuard;
    auto& engine = engineGuard.engine();

    // Sanity: clean slate.
    ASSERT_FALSE(WasmtimeScriptEngine::hasIdleBridgeForTest());

    // First scope: do NOT call injectNative. On teardown the bridge SHOULD be
    // parked (we want the non-emit fast-path to remain intact).
    {
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ASSERT(scope);
        // No setupEmit here.
        ASSERT_EQ(0, scope->invoke(scope->createFunction("return 1;"), nullptr, nullptr, 0));
    }
    ASSERT_TRUE(WasmtimeScriptEngine::hasIdleBridgeForTest())
        << "Non-emit bridge should still be parked on teardown — parking fast-path "
           "must not regress.";

    // Second scope: pick up the parked bridge (via createScopeForCurrentThread),
    // then call setupEmit by registering an emit callback. On teardown the
    // emit-configured bridge MUST be shut down, not re-parked.
    std::vector<BSONObj> emitted;
    {
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ASSERT(scope);
        scope->injectNative(
            "emit",
            [](const BSONObj& args, void* data) -> BSONObj {
                static_cast<std::vector<BSONObj>*>(data)->push_back(args.getOwned());
                return BSONObj();
            },
            &emitted);

        ScriptingFunction mapFn = scope->createFunction("function() { emit(1, this.v); }");
        BSONObj doc = BSON("v" << 42);
        ASSERT_EQ(0, scope->invoke(mapFn, nullptr, &doc, 0, true));
        ASSERT_EQ(1u, emitted.size());
    }
    ASSERT_FALSE(WasmtimeScriptEngine::hasIdleBridgeForTest())
        << "Emit-configured bridge MUST NOT be parked — WASM linear memory only "
           "grows, so reusing it for another MapReduce would accumulate ~117 MB "
           "of orphaned linear memory per operation and eventually trip the "
           "CannotLeaveComponent trap (mr_bigobject.js).";

    // A subsequent scope must then run on a freshly instantiated bridge.
    {
        std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
        ASSERT(scope);
        ASSERT_EQ(0, scope->invoke(scope->createFunction("return 1;"), nullptr, nullptr, 0));
    }
    // After teardown of this third non-emit scope, the bridge IS parked again
    // (we did not call setupEmit, so the fast path applies).
    ASSERT_TRUE(WasmtimeScriptEngine::hasIdleBridgeForTest());
}

// Repeated injectNative("emit") calls must not re-invoke setupEmit.
// Pre-fix each document allocated ~117 MB of WASM linear memory, exhausting the store.
TEST(WasmtimeScope, MapReducePattern_RepeatedInjectEmit_DoesNotReallocate) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    struct EmitState {
        std::vector<BSONObj> emitted;
    };
    EmitState emitState;
    NativeFunction emitFn = [](const BSONObj& args, void* data) -> BSONObj {
        static_cast<EmitState*>(data)->emitted.push_back(args.getOwned());
        return BSONObj();
    };

    ScriptingFunction mapFn = scope->createFunction("function() { emit(this.k, this.v); }");
    ASSERT_NE(0u, mapFn);

    // Simulate evaluate_javascript.cpp's per-document pattern:
    //   inject(non-null) -> invokeMap -> inject(null)
    // …repeated for a large number of documents.  We just need enough docs that the
    // accumulated linear-memory pressure would have exceeded the store limit prior
    // to the fix.  200 docs × 117 MB == 23 GB, well above any reasonable store cap.
    constexpr int kDocs = 200;
    for (int i = 0; i < kDocs; ++i) {
        scope->injectNative("emit", emitFn, &emitState);
        BSONObj doc = BSON("k" << "x" << "v" << i);
        ASSERT_EQ(0, scope->invoke(mapFn, nullptr, &doc, 0, true));
        scope->injectNative("emit", emitFn, nullptr);  // invalidation
    }
    ASSERT_EQ(static_cast<size_t>(kDocs), emitState.emitted.size());
}

// mapReduce.reduce pattern: invoke(func, &args, nullptr) with [key, values]
TEST(WasmtimeScope, MapReduceReducePattern) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction reduceFn = scope->createFunction(
        "function(key, values) {"
        "  var sum = 0;"
        "  for (var i = 0; i < values.length; i++) sum += values[i];"
        "  return sum;"
        "}");
    ASSERT(reduceFn != 0);

    BSONObj args = BSON("0" << "electronics" << "1" << BSON_ARRAY(100 << 250 << 75));
    ASSERT_EQ(0, scope->invoke(reduceFn, &args, nullptr, 0));
    ASSERT_EQ(scope->getNumber("__returnValue"), 425.0);
}
