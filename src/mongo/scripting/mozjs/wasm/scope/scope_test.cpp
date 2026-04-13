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

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/js_regex.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

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

// --- Getter coverage ---

TEST(WasmtimeScope, Getter_StringReturn) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("return 'hello world';");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(std::string("hello world"), scope->getString("__returnValue"));
}

TEST(WasmtimeScope, Getter_IntegerTypes) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("return 7;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(7, scope->getNumberInt("__returnValue"));
    ASSERT_EQ(7LL, scope->getNumberLongLong("__returnValue"));
}

// --- Setter/getter round-trips ---

TEST(WasmtimeScope, Setters_NumberRoundTrip) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setNumber("x", 42.0);
    ASSERT_EQ(42.0, scope->getNumber("x"));
}

TEST(WasmtimeScope, Setters_StringRoundTrip) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setString("s", "mongo");
    ASSERT_EQ(std::string("mongo"), scope->getString("s"));
}

TEST(WasmtimeScope, Setters_BooleanRoundTrip) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setBoolean("flag", true);
    ASSERT_TRUE(scope->getBoolean("flag"));
    scope->setBoolean("flag", false);
    ASSERT_FALSE(scope->getBoolean("flag"));
}

TEST(WasmtimeScope, Setters_ObjectRoundTrip) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj obj = BSON("n" << 42);
    scope->setObject("doc", obj, false);

    // Direct C++ round-trip via getObject().
    BSONObj result = scope->getObject("doc");
    ASSERT_EQ(42, result.getIntField("n"));

    // Also verify JS can access the field.
    ScriptingFunction fn = scope->createFunction("return doc.n;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// setFunction installs a named JS function callable from subsequent invocations.
TEST(WasmtimeScope, Setters_SetFunctionInstalledAndCallable) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setFunction("double_it", "function(x) { return x * 2; }");
    ScriptingFunction fn = scope->createFunction("return double_it(21);");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

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
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->kill();
    ASSERT_TRUE(scope->isKillPending());
    scope->reset();
    ASSERT_FALSE(scope->isKillPending());
}

// Emit state is cleared by reset(); the new bridge can accept a fresh injectNative("emit").
TEST(WasmtimeScope, Lifecycle_Reset_ClearsEmitState) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

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

// Function handles created before reset() are invalidated; using one throws.
TEST(WasmtimeScope, Lifecycle_Reset_InvalidatesHandles) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction staleHandle = scope->createFunction("return 1;");
    scope->reset();

    ASSERT_THROWS_CODE(scope->invoke(staleHandle, nullptr, nullptr, 0),
                       DBException,
                       ErrorCodes::JSInterpreterFailure);
}

// reset() re-initializes the bridge; globals from before reset are cleared.
TEST(WasmtimeScope, Lifecycle_Reset_ClearsGlobals) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

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

// --- Isolation ---

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

// --- Array helpers ---

// Array.sum and Array.avg are installed on every scope by _installHelpers.
TEST(WasmtimeScope, ArrayHelpers_SumAndAvg) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction sumFn = scope->createFunction("return Array.sum([1, 2, 3, 4]);");
    ASSERT_EQ(0, scope->invoke(sumFn, nullptr, nullptr, 0));
    ASSERT_EQ(10.0, scope->getNumber("__returnValue"));

    ScriptingFunction avgFn = scope->createFunction("return Array.avg([2, 4, 6]);");
    ASSERT_EQ(0, scope->invoke(avgFn, nullptr, nullptr, 0));
    ASSERT_EQ(4.0, scope->getNumber("__returnValue"));
}

// --- type() ---

// type() returns the BSON type of the stored __value field.
TEST(WasmtimeScope, Type_ReturnsCorrectBSONType) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction numFn = scope->createFunction("return 42;");
    scope->invoke(numFn, nullptr, nullptr, 0);
    ASSERT_EQ(static_cast<int>(BSONType::numberDouble), scope->type("__returnValue"));

    ScriptingFunction strFn = scope->createFunction("return 'hello';");
    scope->invoke(strFn, nullptr, nullptr, 0);
    ASSERT_EQ(static_cast<int>(BSONType::string), scope->type("__returnValue"));

    ScriptingFunction boolFn = scope->createFunction("return true;");
    scope->invoke(boolFn, nullptr, nullptr, 0);
    ASSERT_EQ(static_cast<int>(BSONType::boolean), scope->type("__returnValue"));
}

// --- setElement ---

// setElement with a numeric BSON element sets it as a JS global.
TEST(WasmtimeScope, SetElement_Number) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj doc = BSON("x" << 99.5);
    scope->setElement("x", doc["x"], doc);

    ScriptingFunction fn = scope->createFunction("return x;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(99.5, scope->getNumber("__returnValue"));
}

// setElement with a string BSON element.
TEST(WasmtimeScope, SetElement_String) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj doc = BSON("msg" << "hello");
    scope->setElement("msg", doc["msg"], doc);

    ScriptingFunction fn = scope->createFunction("return msg;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(std::string("hello"), scope->getString("__returnValue"));
}

// setElement with a Code BSON element installs it as a callable JS function — the
// system.js stored-procedure path (Scope::loadStored) uses setElement with Code elements.
TEST(WasmtimeScope, SetElement_Code_InstalledAsCallable) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj doc = BSON("double_it" << BSONCode("function(x) { return x * 2; }"));
    scope->setElement("double_it", doc["double_it"], doc);

    ScriptingFunction fn = scope->createFunction("return double_it(21);");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// setElement with an embedded object BSON element.
TEST(WasmtimeScope, SetElement_Object) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    BSONObj inner = BSON("n" << 7);
    BSONObj doc = BSON("obj" << inner);
    scope->setElement("obj", doc["obj"], doc);

    ScriptingFunction fn = scope->createFunction("return obj.n;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(7.0, scope->getNumber("__returnValue"));
}

// --- getRegEx ---

// getRegEx extracts pattern and flags from a regex returned by a JS function.
TEST(WasmtimeScope, GetRegEx_PatternAndFlags) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("return /hello/i;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));

    JSRegEx re = scope->getRegEx("__returnValue");
    ASSERT_EQ(std::string("hello"), re.pattern);
    ASSERT_EQ(std::string("i"), re.flags);
}

// getRegEx on a regex with no flags.
TEST(WasmtimeScope, GetRegEx_NoFlags) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("return /^foo$/;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));

    JSRegEx re = scope->getRegEx("__returnValue");
    ASSERT_EQ(std::string("^foo$"), re.pattern);
    ASSERT_EQ(std::string(""), re.flags);
}

// --- wasmtimeStoreMemoryLimitMB server parameter ---

// The default server parameter value (256 MB) allows normal scope creation and execution.
TEST(WasmtimeScope, MemoryLimit_DefaultValueWorks) {
    // Don't override — use whatever the IDL default is.
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 1 + 2;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(3.0, scope->getNumber("__returnValue"));
}

// Changing the parameter at runtime affects newly created scopes.
TEST(WasmtimeScope, MemoryLimit_RuntimeChangeAffectsNewScope) {
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    auto savedHeap = gJSHeapLimitMB.load();
    // heap=200, overhead=max(64,20)=64, minStore=264.  store=512 satisfies.
    gJSHeapLimitMB.store(200);
    gWasmtimeStoreMemoryLimitMB.store(512);
    ON_BLOCK_EXIT([&] {
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
        gJSHeapLimitMB.store(savedHeap);
    });

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 'hello';");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(std::string("hello"), scope->getString("__returnValue"));
}

// After reset(), the scope re-reads the store memory limit from the server parameter.
// Note: the JS heap limit is cached per-scope at construction time, so only the store
// limit changes on reset.
TEST(WasmtimeScope, MemoryLimit_ResetPicksUpNewValue) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    // Change the store limit after the scope was created, then reset.
    // The scope's cached heap is 1100 (default), overhead=110, minStore=1210.
    // Use store=1500 which differs from the default (1210) and satisfies the constraint.
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    gWasmtimeStoreMemoryLimitMB.store(1500);
    ON_BLOCK_EXIT([&] { gWasmtimeStoreMemoryLimitMB.store(savedStore); });
    scope->reset();

    // The scope should still work — init() re-reads the parameter.
    ScriptingFunction fn = scope->createFunction("return 99;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(99.0, scope->getNumber("__returnValue"));
}

// --- jsHeapLimitMB vs wasmtimeStoreMemoryLimitMB ---

// jsHeapLimitMB must never exceed wasmtimeStoreMemoryLimitMB (the JS heap lives
// inside WASM linear memory, so the store must be large enough for the heap plus
// SpiderMonkey runtime overhead).  The bridge constructor enforces this.

// The IDL defaults (jsHeapLimitMB=1100, wasmtimeStoreMemoryLimitMB=1210) must
// satisfy the constraint: store >= heap + max(64, heap/10) = 1100 + 110 = 1210.
TEST(WasmtimeScope, MemoryLimit_DefaultIDLValuesSatisfyConstraint) {
    // Verify the raw default values encode the expected relationship.
    ASSERT_EQ(1100, gJSHeapLimitMB.load());
    ASSERT_EQ(1210, gWasmtimeStoreMemoryLimitMB.load());

    uint32_t heap = static_cast<uint32_t>(gJSHeapLimitMB.load());
    uint32_t minStore = heap + std::max(64u, heap / 10);
    ASSERT_GTE(static_cast<uint32_t>(gWasmtimeStoreMemoryLimitMB.load()), minStore);
}

// Setting jsHeapLimitMB equal to wasmtimeStoreMemoryLimitMB leaves no room for
// overhead, so scope creation must fail.
TEST(WasmtimeScope, MemoryLimit_HeapEqualToStoreLimitFails) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=256, store=256.  overhead=max(64,25)=64, minStore=320 > 256.
    gJSHeapLimitMB.store(256);
    gWasmtimeStoreMemoryLimitMB.store(256);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// Setting jsHeapLimitMB above wasmtimeStoreMemoryLimitMB must fail.
TEST(WasmtimeScope, MemoryLimit_HeapAboveStoreLimitFails) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    gJSHeapLimitMB.store(512);
    gWasmtimeStoreMemoryLimitMB.store(256);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// A store limit that satisfies heap + overhead succeeds.
TEST(WasmtimeScope, MemoryLimit_StoreExceedsHeapPlusOverheadSucceeds) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=200, overhead=max(64,20)=64, minStore=264.  store=264 is exactly enough.
    gJSHeapLimitMB.store(200);
    gWasmtimeStoreMemoryLimitMB.store(264);

    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return 42;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// For large heap values the overhead switches from the 64 MB floor to 10%.
TEST(WasmtimeScope, MemoryLimit_LargeHeapUsesPercentOverhead) {
    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // heap=1000, overhead=max(64,100)=100, minStore=1100.
    // store=1099 is one short → must fail.
    gJSHeapLimitMB.store(1000);
    gWasmtimeStoreMemoryLimitMB.store(1099);

    WasmtimeScriptEngine engine;
    ASSERT_THROWS_CODE(
        engine.createScopeForCurrentThread(boost::none), DBException, ErrorCodes::BadValue);
}

// reset() re-reads the server params; if the new combination is invalid it must fail.
TEST(WasmtimeScope, MemoryLimit_ResetWithInvalidParamsFails) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    auto savedHeap = gJSHeapLimitMB.load();
    auto savedStore = gWasmtimeStoreMemoryLimitMB.load();
    ON_BLOCK_EXIT([&] {
        gJSHeapLimitMB.store(savedHeap);
        gWasmtimeStoreMemoryLimitMB.store(savedStore);
    });

    // Make the combination invalid after the scope already exists, then reset.
    gJSHeapLimitMB.store(512);
    gWasmtimeStoreMemoryLimitMB.store(256);

    ASSERT_THROWS_CODE(scope->reset(), DBException, ErrorCodes::BadValue);
}

// --- OOM detection ---

// hasOutOfMemoryException() starts false and is set when an OOM occurs.
TEST(WasmtimeScope, OOM_FlagInitiallyFalse) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT_FALSE(scope->hasOutOfMemoryException());
}

// After a JS heap OOM, hasOutOfMemoryException() returns true.
TEST(WasmtimeScope, OOM_SetOnJsHeapOom) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // Allocate until the JS heap runs out of memory.
    ScriptingFunction fn = scope->createFunction(
        "function() {"
        "  var arr = [];"
        "  var s = new Array(1024 * 1024 + 1).join('a');"
        "  for (var i = 0; i < 10; i++) {"
        "    arr.push(s);"
        "    s = s + s;"
        "  }"
        "  return arr.length;"
        "}");

    ASSERT_THROWS(scope->invoke(fn, nullptr, nullptr, 0), DBException);
    ASSERT_TRUE(scope->hasOutOfMemoryException());
}

// reset() clears the OOM flag.
TEST(WasmtimeScope, OOM_ResetClearsFlag) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction(
        "function() {"
        "  var arr = [];"
        "  var s = new Array(1024 * 1024 + 1).join('a');"
        "  for (var i = 0; i < 10; i++) {"
        "    arr.push(s);"
        "    s = s + s;"
        "  }"
        "  return arr.length;"
        "}");

    ASSERT_THROWS(scope->invoke(fn, nullptr, nullptr, 0), DBException);
    ASSERT_TRUE(scope->hasOutOfMemoryException());

    scope->reset();
    ASSERT_FALSE(scope->hasOutOfMemoryException());
}
