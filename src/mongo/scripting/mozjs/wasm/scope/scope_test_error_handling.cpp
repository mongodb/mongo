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
