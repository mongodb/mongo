// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/scripting/config_engine_wasm_gen.h"
#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

#include <vector>

using namespace mongo;
using namespace mongo::mozjs;

// --- _createFunction wrapping ---

// "return" appearing as a prefix of an identifier (e.g. "returnCode") must not be
// mistaken for a return statement. The naive substring check would see "return" and
// skip the auto-return wrapper, producing undefined. The word-boundary fix ensures the
// expression is wrapped correctly.
TEST(WasmtimeScope, CreateFunction_ReturnSubstringInIdentifier) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setNumber("returnCode", 42.0);
    // "returnCode" contains "return" as a prefix but is just a variable reference.
    // Should be auto-wrapped as: function() { return returnCode; }
    ScriptingFunction fn = scope->createFunction("returnCode");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// Code with no "function" prefix and no "return" is wrapped as "function() { return <code>; }".
TEST(WasmtimeScope, CreateFunction_BareExpression) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("6 * 7");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// --- invoke edge cases ---

// With ignoreReturn=true the function is still invoked but the C++ scope does NOT
// cache the return value (the WASM side still stores it in JS __returnValue, but the
// scope's _lastReturnValue is not updated).  Verify the function executes by checking
// a side effect (global mutation) rather than the return value.
TEST(WasmtimeScope, Invoke_IgnoreReturn_FunctionIsStillInvoked) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setNumber("sideEffect", 0.0);
    ScriptingFunction fn = scope->createFunction("sideEffect = 42; return 99;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0, /*ignoreReturn=*/true));
    ASSERT_EQ(42.0, scope->getNumber("sideEffect"));
    // The return value (99) must NOT be cached in the scope.
    ASSERT_EQ(0.0, scope->getNumber("__returnValue"));
}

// --- Lifecycle ---

// kill() flag is cleared by reset().
TEST(WasmtimeScope, Lifecycle_Reset_ClearsKillFlag) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    scope->kill();
    ASSERT_TRUE(scope->isKillPending());
    scope->reset();
    ASSERT_FALSE(scope->isKillPending());
}

// Emit state is cleared by reset(); the new bridge can accept a fresh injectNative("emit").
TEST(WasmtimeScope, Lifecycle_Reset_ClearsEmitState) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    std::vector<BSONObj> emitted;
    scope->injectNative(
        "emit",
        [](const BSONObj& args, void* data) -> BSONObj {
            static_cast<std::vector<BSONObj>*>(data)->push_back(args.getOwned());
            return BSONObj();
        },
        &emitted);

    scope->reset();

    // After reset, a new emit can be registered and used without crashing.
    std::vector<BSONObj> emitted2;
    scope->injectNative(
        "emit",
        [](const BSONObj& args, void* data) -> BSONObj {
            static_cast<std::vector<BSONObj>*>(data)->push_back(args.getOwned());
            return BSONObj();
        },
        &emitted2);

    ScriptingFunction mapFn = scope->createFunction("function() { emit(this.k, this.v); }");
    BSONObj doc = BSON("k" << "x" << "v" << 1);
    ASSERT_EQ(0, scope->invoke(mapFn, nullptr, &doc, 0, true));
    ASSERT_EQ(1u, emitted2.size());
    ASSERT_EQ(emitted.size(), 0u);  // original callback was not called
}

// resetRealm() creates a fresh SpiderMonkey Realm, which invalidates every
// previously-compiled function handle. This is the slow-path semantic that
// scope->reset() deliberately avoids on the healthy fast path. Exercise it
// directly against the bridge to lock the invalidation contract in place.
TEST(WasmtimeBridge, RealmReset_InvalidatesHandles) {
    WasmtimeScriptEngine engine;
    auto ctx = engine.getWasmEngineContext();
    wasm::MozJSWasmBridge::Options opts;
    opts.linearMemoryLimitMB = static_cast<uint32_t>(gWasmtimeStoreMemoryLimitMB.load());
    auto bridge = std::make_unique<wasm::MozJSWasmBridge>(ctx, opts);
    ASSERT_TRUE(bridge->initialize());

    auto handle = bridge->createFunction("function() { return 42; }");
    ASSERT_NE(0u, handle);

    bridge->resetRealm();

    // The old handle must no longer resolve to a callable function — invokeFunction
    // throws DBException(JSInterpreterFailure) with "invalid function handle".
    ASSERT_THROWS_CODE(
        bridge->invokeFunction(handle, wasm::wasm_helpers::convertBsonToWcVal(BSONObj{})),
        DBException,
        ErrorCodes::JSInterpreterFailure);

    bridge->shutdown();
}

// scope->reset() uses resetEngine() (per-document fast path): compiled function handles
// survive because _slots are preserved.  The scope is fully usable without recompiling.
TEST(WasmtimeScope, Lifecycle_Reset_PreservesHandles) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("return 42;");
    scope->reset();

    // Handle survives reset() — slots preserved across resetEngine().
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// reset() re-initializes the bridge; globals from before reset are cleared.
TEST(WasmtimeScope, Lifecycle_Reset_ClearsGlobals) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    scope->setNumber("x", 99.0);
    scope->reset();

    ScriptingFunction checkFn = scope->createFunction("return typeof x === 'undefined';");
    ASSERT_EQ(0, scope->invoke(checkFn, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));

    // Scope should still be usable after reset.
    ScriptingFunction fn = scope->createFunction("return 2;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(2.0, scope->getNumber("__returnValue"));
}

// init(data) seeds JS globals from a BSONObj.
TEST(WasmtimeScope, Lifecycle_InitWithSeedData) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj data = BSON("seedVar" << 123);
    scope->init(&data);

    ScriptingFunction fn = scope->createFunction("return seedVar;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(123.0, scope->getNumber("__returnValue"));
}

// kill() sets the pending-kill flag; isKillPending() reads it.
TEST(WasmtimeScope, Lifecycle_Kill_SetsFlag) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ASSERT_FALSE(scope->isKillPending());
    scope->kill();
    ASSERT_TRUE(scope->isKillPending());
}

TEST(WasmtimeScope, ScopeIsolation_IndependentGlobals) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope1(engine.createScopeForCurrentThread(boost::none));
    std::unique_ptr<Scope> scope2(engine.createScopeForCurrentThread(boost::none));

    scope1->setNumber("x", 1.0);
    scope2->setNumber("x", 2.0);

    ASSERT_EQ(1.0, scope1->getNumber("x"));
    ASSERT_EQ(2.0, scope2->getNumber("x"));
}

