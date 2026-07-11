// Copyright (c) MongoDB, Inc.
// SPDX-License-Identifier: SSPL-1.0

#include "mongo/bson/bsontypes.h"
#include "mongo/bson/bsontypes_util.h"
#include "mongo/scripting/config_engine_wasm_gen.h"
#include "mongo/scripting/js_regex.h"
#include "mongo/scripting/mozjs/wasm/bridge/wasm_helpers.h"
#include "mongo/scripting/mozjs/wasm/scope/scope.h"
#include "mongo/scripting/mozjs/wasm/scope/scope_test_fixture.h"
#include "mongo/scripting/mozjs/wasm/wasmtime_engine.h"
#include "mongo/unittest/unittest.h"

using namespace mongo;
using namespace mongo::mozjs;

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

// Regression test: getNumberLongLong() must return the correct milliseconds for a Date return
// value. BSONElement::numberLong() returns 0 for Date type; without special-casing, $function
// expressions that return new Date(...) would always produce 1970-01-01T00:00:00Z.
TEST(WasmtimeScope, Getter_DateReturnedAsMillis) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));

    // 2019-10-15T10:32:50.169Z in milliseconds since epoch.
    const long long expectedMs = 1571135570169LL;
    ScriptingFunction fn = scope->createFunction("return new Date('2019-10-15T10:32:50.169Z');");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    ASSERT_EQ(expectedMs, scope->getNumberLongLong("__returnValue"));
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

// --- Date handling ---

// Date.now() must remain real after repeated reset() cycles.
TEST(WasmtimeScope, DateNow_SurvivesResetEngineCycles) {
    GlobalEngineGuard engineGuard;
    std::unique_ptr<Scope> scope(engineGuard.engine().createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return Date.now();");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    const double t0 = scope->getNumber("__returnValue");
    ASSERT_GTE(t0, 1.0e12);

    for (int i = 0; i < 5; ++i) {
        scope->reset();
        ScriptingFunction fnAfterReset = scope->createFunction("return Date.now();");
        ASSERT_EQ(0, scope->invoke(fnAfterReset, nullptr, nullptr, 0));
        const double t = scope->getNumber("__returnValue");
        ASSERT_GTE(t, t0) << "Date.now() regressed below initial t0 after reset() iter " << i
                          << ": " << t << " < " << t0;
    }
}

// getNumberLongLong on a Date return value must yield real millis.
// Pre-fix BSONElement::numberLong() returned 0 for BSONType::date, collapsing every
// $function-returned Date to ISODate("1970-01-01T00:00:00Z").
TEST(WasmtimeScope, GetNumberLongLong_ReturnsMillisForDateValue) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    ScriptingFunction fn = scope->createFunction("return new Date(Date.now());");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));

    ASSERT_EQ(static_cast<int>(BSONType::date), scope->type("__returnValue"));
    const long long ms = scope->getNumberLongLong("__returnValue");
    ASSERT_GTE(ms, 1'000'000'000'000LL)
        << "getNumberLongLong on a Date return value collapsed to " << ms
        << " — Scope::append() will produce Date_t::fromMillisSinceEpoch(0) "
           "and every $function-returned Date becomes ISODate(\"1970-01-01\").";

    // Sanity: drive the actual JsExecution-style round trip via Scope::append.
    BSONObjBuilder bob;
    scope->append(bob, "out", "__returnValue");
    BSONObj appended = bob.obj();
    ASSERT_EQ(BSONType::date, appended["out"].type());
    ASSERT_GTE(appended["out"].Date().toMillisSinceEpoch(), 1'000'000'000'000LL);
}

// Date.now() must remain real after resetRealm() replaces the child SpiderMonkey Realm.
// The idle-bridge revival path hits resetRealm() on every request.
TEST(WasmtimeBridge, DateNow_SurvivesRealmReset) {
    WasmtimeScriptEngine engine;
    auto ctx = engine.createWasmEngineContext();
    wasm::MozJSWasmBridge::Options opts;
    opts.linearMemoryLimitMB = static_cast<uint32_t>(gWasmtimeStoreMemoryLimitMB.load());
    auto bridge = std::make_unique<wasm::MozJSWasmBridge>(ctx, opts);
    ASSERT_TRUE(bridge->initialize());

    auto handle = bridge->createFunction("function() { return Date.now(); }");
    auto result1 =
        bridge->invokeFunction(handle, wasm::wasm_helpers::convertBsonToWcVal(BSONObj{}));
    ASSERT_OK(result1.getStatus());
    const auto t1 = result1.getValue()["__returnValue"].numberLong();
    ASSERT_GTE(t1, 1'000'000'000'000LL)
        << "Date.now() returned " << t1 << " before resetRealm — clock wasn't wired at all.";

    // Reset the realm — the failing CI path.
    bridge->resetRealm();

    // Handle is now invalid; compile a fresh one in the new realm.
    auto handle2 = bridge->createFunction("function() { return Date.now(); }");
    auto result2 =
        bridge->invokeFunction(handle2, wasm::wasm_helpers::convertBsonToWcVal(BSONObj{}));
    ASSERT_OK(result2.getStatus());
    const auto t2 = result2.getValue()["__returnValue"].numberLong();
    ASSERT_GTE(t2, t1) << "Date.now() after resetRealm returned " << t2
                       << "; expected real wall clock (>= " << t1
                       << "). resetRealm() is dropping the WASI "
                          "clock binding when the child realm is "
                          "rebuilt — this is the hoist_computation "
                          "regression.";

    bridge->shutdown();
}

// Companion: two consecutive Date.now() calls must be able to differ, so a
// spin-loop that waits "until the clock ticks" eventually exits.
TEST(WasmtimeScope, DateNow_AdvancesOverShortInterval) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ASSERT(scope);

    // Spin until Date.now() changes, with a generous 5-second bound so a frozen
    // clock fails quickly rather than hanging the unit-test suite.
    ScriptingFunction fn = scope->createFunction(
        "var t0 = Date.now();"
        "var deadline = t0 + 5000;"
        "while (Date.now() === t0 && Date.now() < deadline) {}"
        "return Date.now() - t0;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    const double delta = scope->getNumber("__returnValue");
    ASSERT_GT(delta, 0.0) << "Date.now() did not advance over a 5-second window; "
                             "wall clock is frozen in the WASM bridge.";
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

/* TODO SERVER-127482: Re-enable once mozjs regex handling is fixed.
   getRegEx on a regex literal whose pattern contains a literal '/'.
TEST(WasmtimeScope, GetRegEx_SlashInPattern) {
    WasmtimeScriptEngine engine;
    std::unique_ptr<Scope> scope(engine.createScopeForCurrentThread(boost::none));
    ScriptingFunction fn = scope->createFunction("return /hello\\/world/gi;");
    ASSERT_EQ(0, scope->invoke(fn, nullptr, nullptr, 0));
    JSRegEx re = scope->getRegEx("__returnValue");
    ASSERT_EQ(std::string("hello\\/world"), re.pattern);
    ASSERT_EQ(std::string("gi"), re.flags);
}
*/
