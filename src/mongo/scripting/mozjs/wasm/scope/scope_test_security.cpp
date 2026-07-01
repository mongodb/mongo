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
#include "mongo/bson/bsontypes.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/duration.h"

using namespace mongo;
using namespace mongo::mozjs;

// ---------------------------------------------------------------------------
// Constructor freeze security tests
// ---------------------------------------------------------------------------

// Frozen constructors: user JS cannot replace built-in static methods.
TEST(WasmtimeScope, Security_FrozenConstructors_PreventStaticMethodReplacement) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // Attempting to overwrite Object.keys must fail silently (sloppy mode).
    ScriptingFunction pollute =
        scope->createFunction("Object.keys = function() { return ['evil']; }; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    // Object.keys must still work correctly.
    ScriptingFunction check =
        scope->createFunction("var keys = Object.keys({a:1, b:2}); return keys.length;");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_EQ(2.0, scope->getNumber("__returnValue"));
}

// Frozen constructors: user JS cannot replace Array.from.
TEST(WasmtimeScope, Security_FrozenConstructors_PreventArrayFromReplacement) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction pollute =
        scope->createFunction("Array.from = function() { return []; }; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    ScriptingFunction check = scope->createFunction("return Array.from([1, 2, 3]).length;");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_EQ(3.0, scope->getNumber("__returnValue"));
}

// TypedArray, Map, Set, Promise, Symbol prototypes are frozen so user JS cannot
// pollute them with persistent properties that survive into the next request.
TEST(WasmtimeScope, Security_FrozenPrototypes_TypedArrayMapSetPromiseSymbol) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction check = scope->createFunction(
        "return Object.isFrozen(Uint8Array.prototype) &&"
        "       Object.isFrozen(Int32Array.prototype) &&"
        "       Object.isFrozen(Float64Array.prototype) &&"
        "       Object.isFrozen(ArrayBuffer.prototype) &&"
        "       Object.isFrozen(DataView.prototype) &&"
        "       Object.isFrozen(Object.getPrototypeOf(Uint8Array.prototype)) &&"
        "       Object.isFrozen(Map.prototype) &&"
        "       Object.isFrozen(Set.prototype) &&"
        "       Object.isFrozen(WeakMap.prototype) &&"
        "       Object.isFrozen(WeakSet.prototype) &&"
        "       Object.isFrozen(Promise.prototype) &&"
        "       Object.isFrozen(Symbol.prototype);");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Prototype pollution: writing to a frozen prototype fails silently in sloppy mode
// and the property is not visible on instances. Verifies cross-request isolation.
TEST(WasmtimeScope, Security_FrozenPrototypes_PreventPollutionViaInstance) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    // First request attempts to pollute Map.prototype.
    ScriptingFunction pollute = scope->createFunction(
        "Map.prototype.poisoned = 'pwned';"
        "return new Map().poisoned;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));
    // Even if the write silently failed, instances must not see the property.
    auto t = scope->type("__returnValue");
    ASSERT_TRUE(t == static_cast<int>(BSONType::undefined) ||
                t == static_cast<int>(BSONType::null));

    // Simulate next-request reset; the pollution attempt should still have left no trace.
    scope->reset();

    ScriptingFunction check = scope->createFunction("return new Map().poisoned;");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    auto t2 = scope->type("__returnValue");
    ASSERT_TRUE(t2 == static_cast<int>(BSONType::undefined) ||
                t2 == static_cast<int>(BSONType::null));
}

// Frozen constructors: Array.sum (installed as an engine helper) is immutable.
TEST(WasmtimeScope, Security_FrozenConstructors_PreventArrayHelperReplacement) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // Attempting to overwrite Array.sum fails silently (frozen).
    ScriptingFunction pollute =
        scope->createFunction("Array.sum = function() { return 999; }; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    // Array.sum must still compute the correct result.
    ScriptingFunction check = scope->createFunction("return Array.sum([1, 2, 3]);");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_EQ(6.0, scope->getNumber("__returnValue"));
}

// After resetRealm(), the new Realm has brand-new constructors (different identity).
// This verifies the realm was actually replaced, not just scrubbed.
TEST(WasmtimeScope, Security_RealmReset_FreshConstructors) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    // Capture a reference to Object in the first Realm.
    ScriptingFunction captureObj =
        scope->createFunction("globalThis.__savedObj = Object; return 1;");
    ASSERT_EQ(0, scope->invoke(captureObj, nullptr, nullptr, 0));

    scope->reset();

    // After reset, __savedObj is gone (new Realm) and Object is a new object.
    ScriptingFunction checkClean =
        scope->createFunction("return typeof globalThis.__savedObj === 'undefined';");
    ScriptingFunction fn2 = scope->createFunction("return 1;");
    ASSERT_EQ(0, scope->invoke(fn2, nullptr, nullptr, 0));
    ASSERT_EQ(0, scope->invoke(checkClean, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Array helpers are available on a fresh Realm after resetRealm().
TEST(WasmtimeScope, Security_RealmReset_ArrayHelpersPresent) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    scope->reset();

    ScriptingFunction fn = scope->createFunction("return Array.sum([10, 20, 12]);");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// ---------------------------------------------------------------------------
// Idle bridge lifecycle
// ---------------------------------------------------------------------------

// A bridge parked within kMaxBridgeIdleTime is reused by the next createScopeForCurrentThread
// on the same thread (fast resetRealm path rather than full re-instantiation).
TEST(WasmtimeScope, IdleBridge_FreshBridgeIsReused) {
    GlobalEngineGuard engineGuard;

    // Park a bridge.
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
    }

    // Immediately create a second scope — bridge should be reused (no full re-init).
    std::unique_ptr<Scope> scope2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope2->createFunction("return 1 + 1;");
    ASSERT_EQ(0, scope2->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(2.0, scope2->getNumber("__returnValue"));
}

// ---------------------------------------------------------------------------
// Cross-request isolation security tests
//
// Each test simulates an attacker-controlled first request attempting to plant
// state that a subsequent innocent request could observe.  After scope1 goes
// out of scope the bridge is parked; scope2's construction reuses it via
// resetRealm(), which must erase all attacker JS state.
//
// Pattern:
//   { scope1 (attacker) → invoke evil JS → destroy } // bridge parked
//   scope2 (victim) → assert nothing leaked          // bridge reused
// ---------------------------------------------------------------------------

// Attacker stashes a secret in globalThis.  The next request must not see it.
TEST(WasmtimeScope, Security_CrossRequest_GlobalVarDoesNotLeak) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn =
            s->createFunction("globalThis.__secret__ = { token: 'hunter2', uid: 42 }; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check =
        s2->createFunction("return typeof globalThis.__secret__ === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker installs a getter trap on Object.prototype that returns a sentinel.
// After realm reset, the trap must not fire when innocent code accesses properties.
TEST(WasmtimeScope, Security_CrossRequest_ObjectPrototypeGetterTrap) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "try {"
            "  Object.defineProperty(Object.prototype, '__trap__', {"
            "    get: function() { return 'EXFILTRATED'; }, configurable: true"
            "  });"
            "} catch(e) {}"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction("return typeof ({}).__trap__ === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker replaces JSON.stringify to intercept serialized documents.
// After realm reset, JSON.stringify must behave natively.
TEST(WasmtimeScope, Security_CrossRequest_JSONStringifyPoisoning) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn =
            s->createFunction("JSON.stringify = function() { return 'POISONED'; }; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction("return JSON.stringify({x: 1}) !== 'POISONED';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker poisons Array.prototype.push to capture all arrays that get appended to.
// After realm reset, push must work correctly on fresh arrays.
TEST(WasmtimeScope, Security_CrossRequest_ArrayPrototypePush) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn =
            s->createFunction("Array.prototype.push = function() { return 99; }; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check =
        s2->createFunction("var a = [1, 2]; a.push(3); return a.length === 3 && a[2] === 3;");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker replaces Function.prototype.call to intercept all method calls.
// After realm reset, Function.prototype.call must be the native implementation.
TEST(WasmtimeScope, Security_CrossRequest_FunctionPrototypeCallPoisoning) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "Function.prototype.call = function() { return 'INTERCEPTED'; }; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction(
        "function add(a, b) { return a + b; }"
        "return add.call(null, 20, 22) === 42;");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker installs a non-configurable, non-writable property on globalThis
// — the kind that cannot be deleted.  After realm reset it must be gone.
TEST(WasmtimeScope, Security_CrossRequest_FrozenBackdoorProperty) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "try {"
            "  Object.defineProperty(globalThis, '__backdoor__', {"
            "    value: 'PERSISTENT', writable: false, configurable: false, enumerable: false"
            "  });"
            "} catch(e) {}"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check =
        s2->createFunction("return typeof globalThis.__backdoor__ === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker captures sensitive data in a closure installed as a global function.
// The next request must not be able to call the closure or observe its data.
TEST(WasmtimeScope, Security_CrossRequest_ClosureCapturedInGlobal) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "(function() {"
            "  var secret = 'TOP_SECRET_PASSWORD';"
            "  globalThis.__exfil = function() { return secret; };"
            "})();"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check =
        s2->createFunction("return typeof globalThis.__exfil === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker replaces the global Object constructor to poison object literals.
// After realm reset, {} must produce a plain empty object.
TEST(WasmtimeScope, Security_CrossRequest_ObjectConstructorReplacement) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "var _orig = Object;"
            "Object = function(v) { var o = new _orig(v); o.__pwned__ = true; return o; };"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check =
        s2->createFunction("var o = {}; return typeof o.__pwned__ === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker uses Symbol.toPrimitive to poison coercion of all objects.
// After realm reset, numeric coercion of {valueOf: ...} must work normally.
TEST(WasmtimeScope, Security_CrossRequest_SymbolToPrimitivePoisoning) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "try {"
            "  Object.defineProperty(Object.prototype, Symbol.toPrimitive, {"
            "    value: function() { return 'POISONED'; }, configurable: true"
            "  });"
            "} catch(e) {}"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction(
        "var obj = { valueOf: function() { return 42; } };"
        "return +obj === 42;");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker uses Array[Symbol.species] to hijack derived array construction in
// map/filter/slice so outputs land in an attacker-controlled class.
// After realm reset, [1,2,3].map(f) must return a genuine Array.
TEST(WasmtimeScope, Security_CrossRequest_ArraySpeciesHijack) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "try {"
            "  Object.defineProperty(Array, Symbol.species, {"
            "    get: function() {"
            "      function PoisonedArray() {}"
            "      PoisonedArray.prototype = Array.prototype;"
            "      PoisonedArray.__pwned__ = true;"
            "      return PoisonedArray;"
            "    }, configurable: true"
            "  });"
            "} catch(e) {}"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction(
        "var result = [1, 2, 3].map(function(x) { return x * 2; });"
        "return result[0] === 2 && result[1] === 4 && result[2] === 6"
        "    && typeof result.__pwned__ === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker plants data across multiple nested objects to defeat shallow scrubbing.
// All of it must be gone after realm reset.
TEST(WasmtimeScope, Security_CrossRequest_DeepGlobalStateDoesNotLeak) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction(
            "globalThis.__a = { b: { c: { d: 'deep_secret' } } };"
            "globalThis.__arr = [1, 2, 3, { secret: 'in_array' }];"
            "(function() {"
            "  var captured = 'closure_secret';"
            "  globalThis.__fn = function() { return captured; };"
            "})();"
            "return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction(
        "return typeof globalThis.__a === 'undefined' &&"
        "       typeof globalThis.__arr === 'undefined' &&"
        "       typeof globalThis.__fn === 'undefined';");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(s2->getBoolean("__returnValue"));
}

// Attacker mutates the Array constructor directly. Array is a fresh, non-shared object
// per child realm (_setupChildRealm() skips copyMissingProps() whenever pObj == cObj), so
// this must not be visible from a sibling realm reusing the same parked bridge.
TEST(WasmtimeScope, Security_CrossRequest_ArrayConstructorMutationDoesNotLeak) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction("Array.injected = 'poison'; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction("return typeof Array.injected;");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_EQ("undefined", s2->getString("__returnValue"));
}

// Attacker mutates a types.js extension function (Array.tojson) instead of the constructor
// itself. copyMissingProps() in _setupChildRealm() copies these extensions onto every child
// realm *by reference* — the same JS object is shared with _parentGlobal and with every
// other child on the thread — and kFreezeBuiltinsScript only Object.freeze()s the standard
// constructors/prototypes, never the extension function objects hanging off them. A mutation
// to Array.tojson therefore lands on the literal object every future realm on this thread
// will inherit from the parent, leaking across the realm boundary that's supposed to
// separate unrelated requests.
TEST(WasmtimeScope, Security_CrossRequest_SharedExtensionFunctionMutationLeaks) {
    GlobalEngineGuard engineGuard;
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
        ScriptingFunction fn = s->createFunction("Array.tojson.injected = 'poison'; return 1;");
        ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    }
    std::unique_ptr<Scope> s2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction check = s2->createFunction("return typeof Array.tojson.injected;");
    ASSERT_EQ(0, s2->invoke(check, nullptr, nullptr, 0));
    ASSERT_EQ("undefined", s2->getString("__returnValue"));
}

// A bridge parked more than kMaxBridgeIdleTime ago is evicted; the next
// createScopeForCurrentThread creates a fresh scope rather than reusing stale linear memory.
TEST(WasmtimeScope, IdleBridge_StaleBridgeIsEvicted) {
    GlobalEngineGuard engineGuard;

    // Park a bridge, then backdate its park timestamp past the expiry window.
    {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
    }
    WasmtimeScriptEngine::backdateIdleBridgeForTest(Milliseconds{15000});

    // The next scope creation must succeed and be fully functional despite the stale bridge.
    std::unique_ptr<Scope> scope2(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ASSERT(scope2);
    ScriptingFunction fn = scope2->createFunction("return 42;");
    ASSERT_EQ(0, scope2->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope2->getNumber("__returnValue"));
}

// reuseCount must increment on each park so bridges are evicted after
// kMaxBridgeReuseCount reuses. The previous bug always reset reuseCount to 1 instead of
// incrementing, allowing indefinite reuse and unbounded linear-memory accumulation that
// eventually caused CannotLeaveComponent traps under sustained $function load.
TEST(WasmtimeScope, IdleBridge_ReuseCountIncrements) {
    GlobalEngineGuard engineGuard;
    constexpr uint32_t kLimit = WasmtimeScriptEngine::kMaxBridgeReuseCount;

    // Create+destroy kLimit+1 scopes to drive reuseCount to kLimit+1 in the idle slot.
    for (uint32_t i = 0; i <= kLimit; ++i) {
        std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
    }
    // Idle slot must hold the over-limit bridge (reuseCount == kLimit+1).
    ASSERT_TRUE(WasmtimeScriptEngine::hasIdleBridgeForTest());

    // The next createScopeForCurrentThread must detect reuseCount > kLimit, drop the bridge,
    // and create a fresh one. The resulting scope must be fully functional.
    std::unique_ptr<Scope> s(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = s->createFunction("return 99;");
    ASSERT_EQ(0, s->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(99.0, s->getNumber("__returnValue"));
}

// ---------------------------------------------------------------------------
// Cross-scope isolation
// ---------------------------------------------------------------------------

// Two scopes from the same engine have independent global state.
TEST(WasmtimeScope, ScopeIsolation_IndependentGlobals) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope1(engine.createScopeForCurrentThread(boost::none));
    std::unique_ptr<Scope> scope2(engine.createScopeForCurrentThread(boost::none));

    scope1->setNumber("x", 1.0);
    scope2->setNumber("x", 2.0);

    ASSERT_EQ(1.0, scope1->getNumber("x"));
    ASSERT_EQ(2.0, scope2->getNumber("x"));
}

// ---------------------------------------------------------------------------
// reset() scrubs cross-request state
// ---------------------------------------------------------------------------

// Direct globalThis property writes (bypassing setGlobal) must not survive reset().
// This is a security requirement: cross-request data leakage via globalThis pollution.
TEST(WasmtimeScope, Security_Reset_ClearsDirectGlobalThisWrites) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    // Write directly to globalThis — bypasses _userGlobalNames tracking.
    ScriptingFunction pollute =
        scope->createFunction("globalThis.__secret__ = 'leaked'; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    scope->reset();

    // After reset, the property must be gone.
    ScriptingFunction check =
        scope->createFunction("return typeof globalThis.__secret__ === 'undefined';");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Non-enumerable properties written to globalThis must not survive reset().
TEST(WasmtimeScope, Security_Reset_ClearsNonEnumerableGlobalWrites) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    ScriptingFunction pollute = scope->createFunction(
        "Object.defineProperty(globalThis, '__hidden__', "
        "  { value: 'leaked', enumerable: false, configurable: true });"
        "return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    scope->reset();

    ScriptingFunction check =
        scope->createFunction("return typeof globalThis.__hidden__ === 'undefined';");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Object.prototype pollution must not persist across reset().
TEST(WasmtimeScope, Security_Reset_ClearsPrototypePollution) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    ScriptingFunction pollute =
        scope->createFunction("Object.prototype.__evil__ = 'leaked'; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    scope->reset();

    ScriptingFunction check = scope->createFunction("return typeof ({}).__evil__ === 'undefined';");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Replacing built-in functions (e.g. Math.abs) must not persist across reset().
TEST(WasmtimeScope, Security_Reset_RestoresBuiltinFunctions) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    ScriptingFunction pollute =
        scope->createFunction("Math.abs = function() { return 99; }; return 1;");
    ASSERT_EQ(0, scope->invoke(pollute, nullptr, nullptr, 0));

    scope->reset();

    ScriptingFunction check = scope->createFunction("return Math.abs(-1) === 1;");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// Properties attached to a cached function object must not survive reset().
TEST(WasmtimeScope, Security_Reset_ClearsFunctionSlotState) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

    // Compile the function once — it will be cached in _slots.
    ScriptingFunction fn = scope->createFunction("return 1;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));

    // Stash data on the function object itself.
    ScriptingFunction stash = scope->createFunction(
        "var fns = Object.keys(globalThis).map(function(k) { return globalThis[k]; })"
        "  .filter(function(v) { return typeof v === 'function'; });"
        "if (fns.length > 0) { fns[0].__stash__ = 'SENSITIVE_DATA'; }"
        "return 1;");
    ASSERT_EQ(0, scope->invoke(stash, nullptr, nullptr, 0));

    scope->reset();

    // Re-invoke the cached function handle — __stash__ must be gone.
    ScriptingFunction check = scope->createFunction(
        "var leaked = false;"
        "var fns = Object.keys(globalThis).map(function(k) { return globalThis[k]; })"
        "  .filter(function(v) { return typeof v === 'function'; });"
        "for (var i = 0; i < fns.length; i++) {"
        "  if (fns[i].__stash__ !== undefined) { leaked = true; break; }"
        "}"
        "return !leaked;");
    ASSERT_EQ(0, scope->invoke(check, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}
