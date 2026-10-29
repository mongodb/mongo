// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes_util.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

#include <vector>

using namespace mongo;
using namespace mongo::mozjs;

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
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));

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

// Test killing a process.
TEST(WasmtimeScope, KillProcess) {
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

// Test timeouts with a regular function.
TEST(WasmtimeScope, TimeoutFunction) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("while (true) {}");


    ASSERT_THROWS_WITH_CHECK(scope->invoke(fn, nullptr, nullptr, 1),
                             AssertionException,
                             [](auto&& ex) { ASSERT_STRING_CONTAINS(ex.reason(), "interrupt"); });
}

// Test timeouts with predicate.
TEST(WasmtimeScope, TimeoutPredicate) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn =
        scope->createFunction("function() { while (true) {}; return this.age >= 18; }");
    ASSERT(fn != 0);

    BSONObj doc1 = BSON("age" << 25);
    ASSERT_THROWS_WITH_CHECK(scope->invoke(fn, nullptr, &doc1, 1),
                             AssertionException,
                             [](auto&& ex) { ASSERT_STRING_CONTAINS(ex.reason(), "interrupt"); });
}

// Test timeouts with map.
TEST(WasmtimeScope, TimeoutMap) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    std::vector<BSONObj> emitted2;
    scope->injectNative(
        "emit",
        [](const BSONObj& args, void* data) -> BSONObj {
            static_cast<std::vector<BSONObj>*>(data)->push_back(args.getOwned());
            return BSONObj();
        },
        &emitted2);

    ScriptingFunction mapFn =
        scope->createFunction("function() { while(true) {}; emit(this.k, this.v); }");
    BSONObj doc = BSON("k" << "x" << "v" << 1);
    ASSERT_THROWS_WITH_CHECK(scope->invoke(mapFn, nullptr, &doc, 1, true),
                             AssertionException,
                             [](auto&& ex) { ASSERT_STRING_CONTAINS(ex.reason(), "interrupt"); });
}

// Test timeouts with sleep.
TEST(WasmtimeScope, TimeoutSleep) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ScriptingFunction fn = scope->createFunction("sleep(10000)");
    ASSERT_THROWS_WITH_CHECK(scope->invoke(fn, nullptr, nullptr, 1),
                             AssertionException,
                             [](auto&& ex) { ASSERT_STRING_CONTAINS(ex.reason(), "interrupt"); });
}

// --- exec() ---

// exec() runs an arbitrary JS statement; side effects are visible afterward.
// Pre-seed the global so the assignment targets an existing global property
TEST(WasmtimeScope, Exec_StatementSideEffect) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    scope->setNumber("x", 0.0);
    ASSERT_TRUE(scope->exec("x = 42;", "test", false, true, true));
    ScriptingFunction fn = scope->createFunction("return x;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(42.0, scope->getNumber("__returnValue"));
}

// exec() successfully runs a delete statement — the loadStored cleanup path calls
// execSetup("delete <name>") to remove stored functions from the scope that were
// removed while loading stored procedures.
TEST(WasmtimeScope, Exec_DeleteGlobal) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // Mirror loadStored: install a Code element (system.js function).
    BSONObj doc = BSON("myFunc" << BSONCode("function() { return 1; }"));
    scope->setElement("myFunc", doc["myFunc"], doc);

    // Verify the function is actually installed and callable before delete.
    ScriptingFunction callFn = scope->createFunction("return myFunc();");
    ASSERT_EQ(0, scope->invoke(callFn, nullptr, nullptr, 0));
    ASSERT_EQ(1.0, scope->getNumber("__returnValue"));

    ASSERT_TRUE(scope->exec("delete myFunc;", "clean up scope", false, false, false));

    // Verify the function is gone after delete.
    ScriptingFunction checkFn = scope->createFunction("return typeof myFunc === 'undefined';");
    ASSERT_EQ(0, scope->invoke(checkFn, nullptr, nullptr, 0));
    ASSERT_TRUE(scope->getBoolean("__returnValue"));
}

// exec() with a syntax error throws.
TEST(WasmtimeScope, Exec_ErrorHandling) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    ASSERT_THROWS(scope->exec("{{{{", "test", false, false, false), DBException);
}
