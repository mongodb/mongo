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

/**
 * Unit tests for the MozJS WASM component API via wasmtime.
 *
 * These tests load the compiled mozjs_wasm_api.wasm component,
 * instantiate it in a wasmtime runtime, and exercise the WIT
 * interface functions (initialize-engine, create-function,
 * invoke-function, invoke-predicate, invoke-map,
 * get-return-value-bson, shutdown-engine).
 *
 */

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <wasmtime.hh>

#include <wasmtime/component.hh>
#include <wasmtime/component/val.h>

namespace wt = wasmtime;
namespace wc = wasmtime::component;

namespace mongo {
namespace mozjs {
namespace wasm {
namespace {

// TODO SERVER-115423: Replace raw usages of the Wasmtime API with the
// MozJS Wasm Bridge implementation.
static std::vector<uint8_t> readWasmFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) {
        return {};
    }
    auto pos = f.tellg();
    if (pos < 0) {
        return {};
    }
    auto size = static_cast<size_t>(pos);
    f.seekg(0);
    std::vector<uint8_t> buf(size);
    if (!f.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(size))) {
        return {};
    }
    return buf;
}

static wc::List makeListU8(const uint8_t* data, size_t len) {
    wasmtime_component_vallist_t raw;
    wasmtime_component_vallist_new_uninit(&raw, len);
    for (size_t i = 0; i < len; i++) {
        raw.data[i].kind = WASMTIME_COMPONENT_U8;
        raw.data[i].of.u8 = data[i];
    }
    return wc::List(std::move(raw));
}

static wc::List makeListU8(std::string_view s) {
    return makeListU8(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// Create a component Val for WIT `string` type (distinct from list<u8>).
static wc::Val makeString(std::string_view s) {
    wasmtime_component_val_t raw;
    raw.kind = WASMTIME_COMPONENT_STRING;
    wasm_byte_vec_new(&raw.of.string, s.size(), s.data());
    return wc::Val(std::move(raw));
}

static wc::List makeListU8(const BSONObj& obj) {
    return makeListU8(reinterpret_cast<const uint8_t*>(obj.objdata()),
                      static_cast<size_t>(obj.objsize()));
}

static std::vector<uint8_t> extractListU8(const wc::Val& v) {
    std::vector<uint8_t> out;
    if (!v.is_list())
        return out;
    const wc::List& list = v.get_list();
    out.reserve(list.size());
    for (const wc::Val& elem : list) {
        if (elem.is_u8()) {
            out.push_back(elem.get_u8());
        }
    }
    return out;
}

static std::optional<wc::Func> getMozjsFunc(wc::Instance& instance,
                                            wt::Store::Context ctx,
                                            std::string_view ifaceName,
                                            std::string_view funcName) {
    auto ifaceIdx = instance.get_export_index(ctx, nullptr, ifaceName);
    if (!ifaceIdx)
        return std::nullopt;
    auto funcIdx = instance.get_export_index(ctx, &*ifaceIdx, funcName);
    if (!funcIdx)
        return std::nullopt;
    return instance.get_func(ctx, *funcIdx);
}

template <typename... Args>
static bool callFunc(
    wc::Func& func, wt::Store::Context ctx, wc::Val* results, size_t numResults, Args&&... args) {
    std::array<wc::Val, sizeof...(Args)> argsArr = {std::forward<Args>(args)...};
    wt::Span<const wc::Val> argsSpan(argsArr.data(), argsArr.size());
    wt::Span<wc::Val> resultsSpan(results, numResults);
    auto callResult = func.call(ctx, argsSpan, resultsSpan);
    if (!callResult)
        return false;
    auto postResult = func.post_return(ctx);
    return static_cast<bool>(postResult);
}

static bool callFuncNoArgs(wc::Func& func,
                           wt::Store::Context ctx,
                           wc::Val* results,
                           size_t numResults) {
    const wc::Val* emptyPtr = nullptr;
    wt::Span<const wc::Val> empty(emptyPtr, size_t{0});
    wt::Span<wc::Val> resultsSpan(results, numResults);
    auto callResult = func.call(ctx, empty, resultsSpan);
    if (!callResult)
        return false;
    auto postResult = func.post_return(ctx);
    return static_cast<bool>(postResult);
}

static bool isResultOk(const wc::Val& result) {
    return result.is_result() && result.get_result().is_ok();
}

struct WasmError {
    std::string code;
    std::string msg;
    std::string filename;
    std::string stack;
    uint32_t line = 0;
    uint32_t column = 0;
};

static std::string extractOptString(const wc::Val& optionVal) {
    const wc::Val* inner = optionVal.get_option().value();
    if (inner && inner->is_string())
        return std::string(inner->get_string());
    return {};
}

// WIT record fields are always in declaration order (mozjs.wit):
//   0: code (err-code enum)
//   1: msg (option<string>)
//   2: filename (option<string>)
//   3: stack (option<string>)
//   4: line (u32)
//   5: column (u32)
static std::optional<WasmError> extractError(const wc::Val& result) {
    if (!result.is_result())
        return std::nullopt;
    const auto& witResult = result.get_result();
    if (witResult.is_ok())
        return std::nullopt;
    const wc::Val* payload = witResult.payload();
    if (!payload || !payload->is_record())
        return std::nullopt;

    const wc::Record& rec = payload->get_record();
    auto* f = rec.begin();

    WasmError error;
    error.code = std::string(f[0].value().get_enum());
    error.msg = extractOptString(f[1].value());
    error.filename = extractOptString(f[2].value());
    error.stack = extractOptString(f[3].value());
    error.line = f[4].value().get_u32();
    error.column = f[5].value().get_u32();
    return error;
}

static std::string resolveWasmPath() {
    // 1. Explicit env var
    if (const char* envPath = std::getenv("WASM_MODULE_PATH")) {
        return envPath;
    }
    // 2. Bazel TEST_SRCDIR runfiles
    if (const char* srcdir = std::getenv("TEST_SRCDIR")) {
        for (const char* candidate : {
                 "/_main/src/mongo/scripting/mozjs/wasm/mozjs_wasm_api.wasm",
                 "/_main~_repo_rules~mozjs_wasm/file/mozjs_wasm_api.wasm",
             }) {
            std::string p = std::string(srcdir) + candidate;
            std::ifstream check(p);
            if (check.good())
                return p;
        }
    }
    // 3. Bazel-bin output directory (when built separately with --config=wasi)
    {
        // Try relative path that works when run from repo root via bazel test
        for (const char* candidate : {
                 "bazel-bin/src/mongo/scripting/mozjs/wasm/mozjs_wasm_api.wasm",
                 "src/mongo/scripting/mozjs/wasm/mozjs_wasm_api.wasm",
             }) {
            std::ifstream check(candidate);
            if (check.good())
                return candidate;
        }
    }
    // 4. Last attempt
    return "src/mongo/scripting/mozjs/wasm/mozjs_wasm_api.wasm";
}

// Shares wasmtime engine + compiled component across all tests.
// Only the store (and thus instance) are recreated per test to provide isolation.
class WasmMozJSTest : public unittest::Test {
public:
    // One-time setup: compile the WASM component (expensive, ~40s).
    static void SetUpTestSuite() {
        s_wasmPath = resolveWasmPath();
        auto wasmBytes = readWasmFile(s_wasmPath);
        if (wasmBytes.empty()) {
            return;
        }

        wt::Config config;
        config.wasm_component_model(true);

        s_engine.emplace(std::move(config));

        wt::Span<uint8_t> wasmSpan(wasmBytes.data(), wasmBytes.size());
        auto componentResult = wc::Component::compile(*s_engine, wasmSpan);
        if (!componentResult) {
            return;
        }
        s_component.emplace(componentResult.ok());
        s_suiteReady = true;
    }

    static void TearDownTestSuite() {
        s_component.reset();
        s_engine.reset();
        s_suiteReady = false;
    }

protected:
    // Per-test setup: create a fresh store and instance.
    void setUp() override {
        ASSERT_TRUE(s_suiteReady);

        _store.emplace(*s_engine);
        auto ctx = _store->context();

        wt::WasiConfig wasiConfig;
        wasiConfig.inherit_stdout();
        wasiConfig.inherit_stderr();
        auto wasiResult = ctx.set_wasi(std::move(wasiConfig));
        // WASI config must be set successfully.
        ASSERT_TRUE(!!wasiResult);

        wc::Linker linker(*s_engine);
        auto wasip2Result = linker.add_wasip2();
        // wasip2 must be added to the linker.
        ASSERT_TRUE(!!wasip2Result);

        auto instanceResult = linker.instantiate(ctx, *s_component);
        // Component instantiation must succeed.
        ASSERT_TRUE(!!instanceResult);
        _instance.emplace(instanceResult.ok());
        _ready = true;
    }

    void tearDown() override {
        if (_ready && _engineInitialized) {
            auto shutdownFunc = getFunc("shutdown-engine");
            if (shutdownFunc) {
                wc::Val result(wc::WitResult::ok(std::nullopt));
                callFuncNoArgs(*shutdownFunc, ctx(), &result, 1);
            }
        }
    }

    wt::Store::Context ctx() {
        return _store->context();
    }

    std::optional<wc::Func> getFunc(std::string_view funcName) {
        return getMozjsFunc(*_instance, ctx(), "mongo:mozjs/mozjs", funcName);
    }

    // Initialize the MozJS engine inside WASM. Returns true on success.
    bool initEngine() {
        auto initFunc = getFunc("initialize-engine");
        ASSERT_TRUE(initFunc.has_value());
        wc::Val result(wc::WitResult::ok(std::nullopt));
        ASSERT_TRUE(callFuncNoArgs(*initFunc, ctx(), &result, 1));
        bool ok = isResultOk(result);
        if (ok)
            _engineInitialized = true;
        return ok;
    }

    // Create a JS function. Returns function handle on success, 0 on failure.
    uint64_t createFunction(std::string_view source) {
        auto createFunc = getFunc("create-function");
        ASSERT_TRUE(createFunc.has_value());
        wc::Val srcArg(makeListU8(source));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg)))
            return 0;
        if (!isResultOk(result))
            return 0;
        const wc::Val* payload = result.get_result().payload();
        if (!payload || !payload->is_u64())
            return 0;
        return payload->get_u64();
    }

    // Invoke a function with BSON args. Returns true on success.
    bool invokeFunction(uint64_t handle, const BSONObj& args) {
        auto invokeFunc = getFunc("invoke-function");
        ASSERT_TRUE(invokeFunc.has_value());
        wc::Val arg0(handle);
        wc::Val arg1(makeListU8(args));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)))
            return false;
        return isResultOk(result);
    }

    // Invoke a function expecting failure. Returns the WasmError.
    WasmError invokeFunctionError(uint64_t handle, const BSONObj& args) {
        auto invokeFunc = getFunc("invoke-function");
        ASSERT_TRUE(invokeFunc.has_value());
        wc::Val arg0(handle);
        wc::Val arg1(makeListU8(args));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));
        auto err = extractError(result);
        ASSERT_TRUE(err.has_value()) << "expected error but call succeeded";
        return *err;
    }

    // Invoke a map function expecting failure. Returns the WasmError.
    WasmError invokeMapError(uint64_t handle, const BSONObj& document) {
        auto func = getFunc("invoke-map");
        ASSERT_TRUE(func.has_value());
        wc::Val arg0(handle);
        wc::Val arg1(makeListU8(document));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        callFunc(*func, ctx(), &result, 1, std::move(arg0), std::move(arg1));
        auto err = extractError(result);
        ASSERT_TRUE(err.has_value()) << "expected error but call succeeded";
        return *err;
    }

    // Invoke a predicate: document becomes `this`, returns the bool result.
    // Asserts that the call succeeds.
    bool invokePredicate(uint64_t handle, const BSONObj& document) {
        auto func = getFunc("invoke-predicate");
        ASSERT_TRUE(func.has_value());
        wc::Val arg0(handle);
        wc::Val arg1(makeListU8(document));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        ASSERT_TRUE(callFunc(*func, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
        ASSERT_TRUE(isResultOk(result));
        const wc::Val* payload = result.get_result().payload();
        ASSERT_TRUE(payload != nullptr && payload->is_bool());
        return payload->get_bool();
    }

    // Invoke a map function: document becomes `this`, emits buffered internally.
    bool invokeMap(uint64_t handle, const BSONObj& document) {
        auto func = getFunc("invoke-map");
        ASSERT_TRUE(func.has_value());
        wc::Val arg0(handle);
        wc::Val arg1(makeListU8(document));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*func, ctx(), &result, 1, std::move(arg0), std::move(arg1)))
            return false;
        return isResultOk(result);
    }

    // Set up the emit() built-in for mapReduce. Resets the emit buffer.
    bool setupEmit() {
        auto func = getFunc("setup-emit");
        ASSERT_TRUE(func.has_value());
        wc::Val arg0{wc::WitOption(std::nullopt)};
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*func, ctx(), &result, 1, std::move(arg0)))
            return false;
        return isResultOk(result);
    }

    // Set up emit() with an explicit byte limit.
    bool setupEmitWithLimit(int64_t byteLimit) {
        auto func = getFunc("setup-emit");
        ASSERT_TRUE(func.has_value());
        wc::Val arg0{wc::WitOption(wc::Val(byteLimit))};
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*func, ctx(), &result, 1, std::move(arg0)))
            return false;
        return isResultOk(result);
    }

    // Drain the emit buffer: returns accumulated {k,v} pairs as BSON.
    BSONObj drainEmitBuffer() {
        auto func = getFunc("drain-emit-buffer");
        ASSERT_TRUE(func.has_value());
        wc::Val result(wc::WitResult::ok(std::nullopt));
        ASSERT_TRUE(callFuncNoArgs(*func, ctx(), &result, 1));
        ASSERT_TRUE(isResultOk(result));
        auto bytes = extractListU8(*result.get_result().payload());
        return BSONObj(reinterpret_cast<const char*>(bytes.data())).getOwned();
    }

    // Get the last return value as BSON.
    BSONObj getReturnValueBson() {
        auto getFunc = this->getFunc("get-return-value-bson");
        ASSERT_TRUE(getFunc.has_value());
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFuncNoArgs(*getFunc, ctx(), &result, 1))
            return BSONObj();
        if (!isResultOk(result))
            return BSONObj();
        const wc::Val* payload = result.get_result().payload();
        if (!payload)
            return BSONObj();
        auto bytes = extractListU8(*payload);
        if (bytes.size() < 5)
            return BSONObj();
        // Copy into owned buffer for BSONObj
        auto buf = SharedBuffer::allocate(bytes.size());
        std::memcpy(buf.get(), bytes.data(), bytes.size());
        return BSONObj(std::move(buf));
    }

    // Set a named global variable from a BSON-encoded value. Returns true on success.
    bool setGlobal(std::string_view name, const BSONObj& value) {
        auto setFunc = getFunc("set-global");
        ASSERT_TRUE(setFunc.has_value());
        wc::Val nameArg = makeString(name);
        wc::Val valueArg(makeListU8(value));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*setFunc, ctx(), &result, 1, std::move(nameArg), std::move(valueArg)))
            return false;
        return isResultOk(result);
    }

    // Set a named global to a single BSON element's JS value (like Scope::setFunction).
    bool setGlobalValue(std::string_view name, const BSONObj& singleElementDoc) {
        auto func = getFunc("set-global-value");
        ASSERT_TRUE(func.has_value());
        wc::Val nameArg = makeString(name);
        wc::Val valueArg(makeListU8(singleElementDoc));
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*func, ctx(), &result, 1, std::move(nameArg), std::move(valueArg)))
            return false;
        return isResultOk(result);
    }

    // Convenience: set a named global to a JS function from source code.
    bool setFunction(std::string_view name, std::string_view code) {
        BSONObjBuilder b;
        b.appendCode("val", std::string(code));
        return setGlobalValue(name, b.obj());
    }

    // Get a named global variable as BSON. Returns empty BSONObj on failure.
    BSONObj getGlobal(std::string_view name) {
        auto getGlobalFunc = getFunc("get-global");
        ASSERT_TRUE(getGlobalFunc.has_value());
        wc::Val nameArg = makeString(name);
        wc::Val result(wc::WitResult::ok(std::nullopt));
        if (!callFunc(*getGlobalFunc, ctx(), &result, 1, std::move(nameArg)))
            return BSONObj();
        if (!isResultOk(result))
            return BSONObj();
        const wc::Val* payload = result.get_result().payload();
        if (!payload)
            return BSONObj();
        auto bytes = extractListU8(*payload);
        if (bytes.size() < 5)
            return BSONObj();
        auto buf = SharedBuffer::allocate(bytes.size());
        std::memcpy(buf.get(), bytes.data(), bytes.size());
        return BSONObj(std::move(buf));
    }

private:
    // Per-test state
    bool _ready = false;
    bool _engineInitialized = false;

    std::optional<wt::Store> _store;
    std::optional<wc::Instance> _instance;

    // Shared across all tests (expensive to create)
    static std::string s_wasmPath;
    static bool s_suiteReady;
    static std::optional<wt::Engine> s_engine;
    static std::optional<wc::Component> s_component;
};

// Static member definitions
std::string WasmMozJSTest::s_wasmPath;
bool WasmMozJSTest::s_suiteReady = false;
std::optional<wt::Engine> WasmMozJSTest::s_engine;
std::optional<wc::Component> WasmMozJSTest::s_component;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, EngineInitializeAndShutdown) {
    ASSERT_TRUE(initEngine());

    // Shutdown
    auto shutdownFunc = getFunc("shutdown-engine");
    ASSERT_TRUE(shutdownFunc.has_value());
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFuncNoArgs(*shutdownFunc, ctx(), &result, 1));
    ASSERT_TRUE(isResultOk(result));
}

TEST_F(WasmMozJSTest, CreateFunctionReturnsHandle) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { return 42; }");
    ASSERT_NE(handle, 0u);
}

TEST_F(WasmMozJSTest, InvokeFunctionNoArgs) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { return {\"answer\": 42}; }");
    ASSERT_NE(handle, 0u);

    // Invoke with empty BSON args
    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    // Retrieve result
    BSONObj result = getReturnValueBson();
    ASSERT_FALSE(result.isEmpty());

    // The result is wrapped: { __returnValue: { answer: 42 } }
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_FALSE(retVal.eoo());
    ASSERT_TRUE(retVal.isABSONObj());

    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getIntField("answer"), 42);
}

// ---------------------------------------------------------------------------
// BSON argument passing tests
//
// invokeFunction passes each BSON field as a POSITIONAL JS argument:
//   BSONObj {a: 21, b: "hello"} → JS call: func(21, "hello")
// The field names determine insertion order but are not visible to JS.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, BsonArgsInt32) {
    ASSERT_TRUE(initEngine());

    // Each BSON field becomes a positional JS arg.
    uint64_t handle = createFunction(
        "function(value, multiplier) {"
        "  return { result: value * multiplier };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("value", 21);
    bob.append("multiplier", 2);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_FALSE(retVal.eoo());
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getIntField("result"), 42);
}

TEST_F(WasmMozJSTest, BsonArgsDouble) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(a, b) {"
        "  return { sum: a + b };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("a", 3.14);
    bob.append("b", 2.86);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    double sum = retVal.Obj().getField("sum").Number();
    ASSERT_APPROX_EQUAL(sum, 6.0, 1e-10);
}

TEST_F(WasmMozJSTest, BsonArgsString) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(greeting, name) {"
        "  return { message: greeting + ', ' + name + '!' };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("greeting", "Hello");
    bob.append("name", "MongoDB");

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getStringField("message"), "Hello, MongoDB!");
}

TEST_F(WasmMozJSTest, BsonArgsBoolean) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(flag) {"
        "  return { negated: !flag, type: typeof flag };"
        "}");
    ASSERT_NE(handle, 0u);

    {
        BSONObjBuilder bob;
        bob.appendBool("flag", true);
        ASSERT_TRUE(invokeFunction(handle, bob.obj()));

        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_TRUE(retVal.isABSONObj());
        BSONObj inner = retVal.Obj();
        ASSERT_EQ(inner.getBoolField("negated"), false);
        ASSERT_EQ(inner.getStringField("type"), "boolean");
    }
    {
        BSONObjBuilder bob;
        bob.appendBool("flag", false);
        ASSERT_TRUE(invokeFunction(handle, bob.obj()));

        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        BSONObj inner = retVal.Obj();
        ASSERT_EQ(inner.getBoolField("negated"), true);
    }
}

TEST_F(WasmMozJSTest, BsonArgsNull) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(val) {"
        "  return { isNull: val === null, type: typeof val };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.appendNull("val");

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getBoolField("isNull"), true);
    ASSERT_EQ(inner.getStringField("type"), "object");  // typeof null === "object" in JS
}

TEST_F(WasmMozJSTest, BsonArgsObject) {
    ASSERT_TRUE(initEngine());

    // Nested BSON object becomes a JS object positional arg
    uint64_t handle = createFunction(
        "function(doc) {"
        "  return {"
        "    name: doc.name,"
        "    age: doc.age,"
        "    hasCity: doc.address !== undefined && doc.address.city !== undefined"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONObjBuilder nested(bob.subobjStart("doc"));
        nested.append("name", "Alice");
        nested.append("age", 30);
        {
            BSONObjBuilder addr(nested.subobjStart("address"));
            addr.append("city", "NYC");
            addr.append("zip", "10001");
        }
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getStringField("name"), "Alice");
    ASSERT_EQ(inner.getIntField("age"), 30);
    ASSERT_EQ(inner.getBoolField("hasCity"), true);
}

TEST_F(WasmMozJSTest, BsonArgsArray) {
    ASSERT_TRUE(initEngine());

    // BSON array becomes a JS Array positional arg
    uint64_t handle = createFunction(
        "function(arr) {"
        "  return {"
        "    length: arr.length,"
        "    sum: arr.reduce(function(a, b) { return a + b; }, 0),"
        "    first: arr[0],"
        "    last: arr[arr.length - 1]"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(10);
        arr.append(20);
        arr.append(30);
        arr.append(40);
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getIntField("length"), 4);
    ASSERT_EQ(inner.getIntField("sum"), 100);
    ASSERT_EQ(inner.getIntField("first"), 10);
    ASSERT_EQ(inner.getIntField("last"), 40);
}

TEST_F(WasmMozJSTest, BsonArgsMixedTypes) {
    ASSERT_TRUE(initEngine());

    // Multiple BSON fields of different types as positional args
    uint64_t handle = createFunction(
        "function(num, str, flag, obj) {"
        "  return {"
        "    numType: typeof num,"
        "    strType: typeof str,"
        "    flagType: typeof flag,"
        "    objType: typeof obj,"
        "    computed: flag ? (num + ' ' + str) : obj.key"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("num", 42);
    bob.append("str", "hello");
    bob.appendBool("flag", true);
    {
        BSONObjBuilder nested(bob.subobjStart("obj"));
        nested.append("key", "fallback");
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getStringField("numType"), "number");
    ASSERT_EQ(inner.getStringField("strType"), "string");
    ASSERT_EQ(inner.getStringField("flagType"), "boolean");
    ASSERT_EQ(inner.getStringField("objType"), "object");
    ASSERT_EQ(inner.getStringField("computed"), "42 hello");
}

TEST_F(WasmMozJSTest, BsonArgsEmptyObject) {
    ASSERT_TRUE(initEngine());

    // No args (empty BSON) — function receives no arguments
    uint64_t handle = createFunction(
        "function() {"
        "  return { argCount: arguments.length };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getIntField("argCount"), 0);
}

TEST_F(WasmMozJSTest, BsonArgsReturnArray) {
    ASSERT_TRUE(initEngine());

    // JS function that returns an array
    uint64_t handle = createFunction(
        "function(n) {"
        "  var result = [];"
        "  for (var i = 0; i < n; i++) {"
        "    result.push(i * i);"
        "  }"
        "  return result;"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("n", 5);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_FALSE(retVal.eoo());
    // Arrays serialize as BSON arrays (which are BSONType::array)
    ASSERT_EQ(retVal.type(), BSONType::array);
    BSONObj arr = retVal.Obj();
    // Elements are keyed "0", "1", "2", ...
    ASSERT_EQ(arr.getIntField("0"), 0);   // 0*0
    ASSERT_EQ(arr.getIntField("1"), 1);   // 1*1
    ASSERT_EQ(arr.getIntField("2"), 4);   // 2*2
    ASSERT_EQ(arr.getIntField("3"), 9);   // 3*3
    ASSERT_EQ(arr.getIntField("4"), 16);  // 4*4
}

TEST_F(WasmMozJSTest, BsonArgsReturnPrimitives) {
    ASSERT_TRUE(initEngine());

    // Returning various primitive types
    BSONObj emptyArgs;

    // Integer
    {
        uint64_t h = createFunction("function() { return 42; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_EQ(retVal.numberInt(), 42);
    }

    // Double
    {
        uint64_t h = createFunction("function() { return 3.14159; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_APPROX_EQUAL(retVal.Number(), 3.14159, 1e-5);
    }

    // Boolean true
    {
        uint64_t h = createFunction("function() { return true; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_EQ(retVal.type(), BSONType::boolean);
        ASSERT_EQ(retVal.Bool(), true);
    }

    // Boolean false
    {
        uint64_t h = createFunction("function() { return false; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_EQ(retVal.type(), BSONType::boolean);
        ASSERT_EQ(retVal.Bool(), false);
    }

    // null
    {
        uint64_t h = createFunction("function() { return null; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_EQ(retVal.type(), BSONType::null);
    }

    // String
    {
        uint64_t h = createFunction("function() { return 'hello'; }");
        ASSERT_NE(h, 0u);
        ASSERT_TRUE(invokeFunction(h, emptyArgs));
        BSONObj result = getReturnValueBson();
        BSONElement retVal = result.getField("__returnValue");
        ASSERT_EQ(retVal.type(), BSONType::string);
        ASSERT_EQ(retVal.str(), "hello");
    }
}

TEST_F(WasmMozJSTest, BsonArgsNestedArraysAndObjects) {
    ASSERT_TRUE(initEngine());

    // Deeply nested structure: object with arrays containing objects
    uint64_t handle = createFunction(
        "function(data) {"
        "  var total = 0;"
        "  for (var i = 0; i < data.items.length; i++) {"
        "    total += data.items[i].price * data.items[i].qty;"
        "  }"
        "  return { total: total, itemCount: data.items.length };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONObjBuilder data(bob.subobjStart("data"));
        {
            BSONArrayBuilder items(data.subarrayStart("items"));
            {
                BSONObjBuilder item(items.subobjStart());
                item.append("price", 10);
                item.append("qty", 3);
            }
            {
                BSONObjBuilder item(items.subobjStart());
                item.append("price", 25);
                item.append("qty", 2);
            }
            {
                BSONObjBuilder item(items.subobjStart());
                item.append("price", 5);
                item.append("qty", 10);
            }
        }
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    // 10*3 + 25*2 + 5*10 = 30 + 50 + 50 = 130
    ASSERT_EQ(inner.getIntField("total"), 130);
    ASSERT_EQ(inner.getIntField("itemCount"), 3);
}

TEST_F(WasmMozJSTest, BsonArgsObjectId) {
    ASSERT_TRUE(initEngine());

    // ObjectId round-trips through JS preserving type and value
    uint64_t handle = createFunction(
        "function(oid) {"
        "  return { val: oid };"
        "}");
    ASSERT_NE(handle, 0u);

    OID testOid = OID::gen();
    BSONObjBuilder bob;
    bob.append("oid", testOid);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement valElem = inner.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::oid);
    ASSERT_EQ(valElem.OID(), testOid);
}

TEST_F(WasmMozJSTest, BsonArgsDate) {
    ASSERT_TRUE(initEngine());

    // Date round-trips through JS. JS Date supports getUTCFullYear etc.
    uint64_t handle = createFunction(
        "function(d) {"
        "  return {"
        "    val: d,"
        "    year: d.getUTCFullYear()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    // 2025-06-15T00:00:00Z = 1750032000000ms since epoch
    Date_t testDate = Date_t::fromMillisSinceEpoch(1750032000000LL);
    BSONObjBuilder bob;
    bob.appendDate("d", testDate);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    // Date round-trips as BSONType::date
    BSONElement valElem = inner.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::date);
    ASSERT_EQ(valElem.Date(), testDate);
    // JS method access works on native Date
    ASSERT_EQ(inner.getIntField("year"), 2025);
}

// ---------------------------------------------------------------------------
// Extended BSON type tests: NumberLong, Decimal128, Timestamp, Regex,
// MinKey/MaxKey, and BinData.
//
// These types use custom SpiderMonkey prototypes (NumberLongInfo,
// NumberDecimalInfo, TimestampInfo, etc.) which are now installed on the
// global via MozJSPrototypeInstaller::installBSONTypes(). trackedNewInt64
// is also implemented, so all these types round-trip correctly through JS.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, BsonArgsNumberLong) {
    ASSERT_TRUE(initEngine());

    // NumberLong (int64) round-trips through JS as a NumberLong object.
    uint64_t handle = createFunction(
        "function(n) {"
        "  return { val: n };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("n", 1234567890123LL);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement valElem = inner.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::numberLong);
    ASSERT_EQ(valElem.Long(), 1234567890123LL);
}

TEST_F(WasmMozJSTest, BsonArgsNumberDecimal128) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(dec) {"
        "  return { val: dec };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("dec", Decimal128("123.456"));

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement valElem = inner.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::numberDecimal);
    ASSERT_TRUE(valElem.numberDecimal() == Decimal128("123.456"));
}

TEST_F(WasmMozJSTest, BsonArgsTimestamp) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(ts) {"
        "  return { val: ts };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("ts", Timestamp(1700000000, 42));

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement tsElem = inner.getField("val");
    ASSERT_EQ(tsElem.type(), BSONType::timestamp);
    ASSERT_EQ(tsElem.timestamp(), Timestamp(1700000000, 42));
}

TEST_F(WasmMozJSTest, BsonArgsRegex) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(re) {"
        "  return { val: re };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.appendRegex("re", "Hello", "i");

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement reElem = inner.getField("val");
    ASSERT_EQ(reElem.type(), BSONType::regEx);
    ASSERT_EQ(std::string(reElem.regex()), "Hello");
    ASSERT_EQ(std::string(reElem.regexFlags()), "i");
}

TEST_F(WasmMozJSTest, BsonArgsMinKeyMaxKey) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(mn, mx) {"
        "  return { minVal: mn, maxVal: mx };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.appendMinKey("mn");
    bob.appendMaxKey("mx");

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_EQ(inner.getField("minVal").type(), BSONType::minKey);
    ASSERT_EQ(inner.getField("maxVal").type(), BSONType::maxKey);
}

TEST_F(WasmMozJSTest, BsonArgsBinData) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(bd) {"
        "  return { val: bd };"
        "}");
    ASSERT_NE(handle, 0u);

    const char binPayload[] = {'\x01', '\x02', '\x03', '\x04', '\x05'};
    BSONObjBuilder bob;
    bob.appendBinData("bd", sizeof(binPayload), BinDataGeneral, binPayload);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));
    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    BSONElement bdElem = inner.getField("val");
    ASSERT_EQ(bdElem.type(), BSONType::binData);
    int len = 0;
    const char* data = bdElem.binData(len);
    ASSERT_EQ(len, 5);
    ASSERT_EQ(std::memcmp(data, binPayload, 5), 0);
}

TEST_F(WasmMozJSTest, BsonArgsArrayAsArg) {
    ASSERT_TRUE(initEngine());

    // Pass a raw BSON array as a positional arg — JS receives it as an Array
    uint64_t handle = createFunction(
        "function(arr) {"
        "  var max = -Infinity;"
        "  var min = Infinity;"
        "  for (var i = 0; i < arr.length; i++) {"
        "    if (arr[i] > max) max = arr[i];"
        "    if (arr[i] < min) min = arr[i];"
        "  }"
        "  return { max: max, min: min, len: arr.length };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(5);
        arr.append(99);
        arr.append(-3);
        arr.append(42);
        arr.append(0);
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getIntField("max"), 99);
    ASSERT_EQ(inner.getIntField("min"), -3);
    ASSERT_EQ(inner.getIntField("len"), 5);
}

TEST_F(WasmMozJSTest, BsonArgsArrayOfStrings) {
    ASSERT_TRUE(initEngine());

    // Pass array of strings, join them in JS
    uint64_t handle = createFunction(
        "function(words) {"
        "  return { sentence: words.join(' '), count: words.length };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("words"));
        arr.append("the");
        arr.append("quick");
        arr.append("brown");
        arr.append("fox");
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getStringField("sentence"), "the quick brown fox");
    ASSERT_EQ(inner.getIntField("count"), 4);
}

TEST_F(WasmMozJSTest, BsonArgsArrayOfMixedTypes) {
    ASSERT_TRUE(initEngine());

    // Array containing mixed types: int, string, bool, null, nested object
    uint64_t handle = createFunction(
        "function(arr) {"
        "  var types = [];"
        "  for (var i = 0; i < arr.length; i++) {"
        "    types.push(arr[i] === null ? 'null' : typeof arr[i]);"
        "  }"
        "  return { types: types.join(','), len: arr.length };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(42);
        arr.append("hello");
        arr.append(true);
        arr.appendNull();
        {
            BSONObjBuilder nested(arr.subobjStart());
            nested.append("key", "val");
        }
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getStringField("types"), "number,string,boolean,null,object");
    ASSERT_EQ(inner.getIntField("len"), 5);
}

TEST_F(WasmMozJSTest, BsonArgsArrayPassthrough) {
    ASSERT_TRUE(initEngine());

    // Verify that a JS function can return the array it received (round-trip)
    uint64_t handle = createFunction(
        "function(arr) {"
        "  return arr;"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(10);
        arr.append(20);
        arr.append(30);
    }

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_EQ(retVal.type(), BSONType::array);
    BSONObj arr = retVal.Obj();
    ASSERT_EQ(arr.getIntField("0"), 10);
    ASSERT_EQ(arr.getIntField("1"), 20);
    ASSERT_EQ(arr.getIntField("2"), 30);
}

TEST_F(WasmMozJSTest, BsonArgsObjectIdRoundTrip) {
    ASSERT_TRUE(initEngine());

    // Pass ObjectId through JS and get it back as-is
    uint64_t handle = createFunction(
        "function(oid) {"
        "  return { id: oid };"
        "}");
    ASSERT_NE(handle, 0u);

    OID testOid = OID::gen();
    BSONObjBuilder bob;
    bob.append("oid", testOid);

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj retObj = retVal.Obj();
    BSONElement idElem = retObj.getField("id");
    ASSERT_EQ(idElem.type(), BSONType::oid);
    ASSERT_EQ(idElem.OID(), testOid);
}

TEST_F(WasmMozJSTest, BsonArgsAllTypesInOneCall) {
    ASSERT_TRUE(initEngine());

    // Pass many different BSON types as positional args in one call
    uint64_t handle = createFunction(
        "function(i32, dbl, str, flag, nullVal, obj, arr, oid, dt) {"
        "  return {"
        "    argCount: arguments.length,"
        "    i32: i32,"
        "    dbl: dbl,"
        "    str: str,"
        "    flag: flag,"
        "    isNull: nullVal === null,"
        "    objKey: obj.k,"
        "    arrLen: arr.length,"
        "    oidStr: oid.str,"
        "    dtYear: dt.getUTCFullYear()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder bob;
    bob.append("i32", 7);
    bob.append("dbl", 2.718);
    bob.append("str", "test");
    bob.appendBool("flag", false);
    bob.appendNull("nullVal");
    {
        BSONObjBuilder nested(bob.subobjStart("obj"));
        nested.append("k", "v");
    }
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(1);
        arr.append(2);
        arr.append(3);
    }
    bob.append("oid", OID::gen());
    bob.appendDate("dt", Date_t::fromMillisSinceEpoch(1750032000000LL));  // 2025-06-15

    ASSERT_TRUE(invokeFunction(handle, bob.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj();
    ASSERT_EQ(inner.getIntField("argCount"), 9);
    ASSERT_EQ(inner.getIntField("i32"), 7);
    ASSERT_APPROX_EQUAL(inner.getField("dbl").Number(), 2.718, 1e-5);
    ASSERT_EQ(inner.getStringField("str"), "test");
    ASSERT_EQ(inner.getBoolField("flag"), false);
    ASSERT_EQ(inner.getBoolField("isNull"), true);
    ASSERT_EQ(inner.getStringField("objKey"), "v");
    ASSERT_EQ(inner.getIntField("arrLen"), 3);
    ASSERT_TRUE(inner.hasField("oidStr"));
    ASSERT_EQ(inner.getIntField("dtYear"), 2025);
}

TEST_F(WasmMozJSTest, CreateMultipleFunctions) {
    ASSERT_TRUE(initEngine());

    uint64_t h1 = createFunction("function() { return 1; }");
    uint64_t h2 = createFunction("function() { return 2; }");
    uint64_t h3 = createFunction("function() { return 3; }");

    ASSERT_NE(h1, 0u);
    ASSERT_NE(h2, 0u);
    ASSERT_NE(h3, 0u);

    // Handles should be distinct
    ASSERT_NE(h1, h2);
    ASSERT_NE(h2, h3);
    ASSERT_NE(h1, h3);

    // Invoke each and verify results
    BSONObj emptyArgs;

    ASSERT_TRUE(invokeFunction(h1, emptyArgs));
    BSONObj r1 = getReturnValueBson();
    ASSERT_EQ(r1.getField("__returnValue").numberInt(), 1);

    ASSERT_TRUE(invokeFunction(h2, emptyArgs));
    BSONObj r2 = getReturnValueBson();
    ASSERT_EQ(r2.getField("__returnValue").numberInt(), 2);

    ASSERT_TRUE(invokeFunction(h3, emptyArgs));
    BSONObj r3 = getReturnValueBson();
    ASSERT_EQ(r3.getField("__returnValue").numberInt(), 3);
}

TEST_F(WasmMozJSTest, InvokeWithInvalidHandleFails) {
    ASSERT_TRUE(initEngine());

    // Invoke with handle=0 (invalid)
    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(uint64_t(0));
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    // Should return an error result
    ASSERT_TRUE(result.is_result());
    // Invalid handle should produce an error result.
    ASSERT_FALSE(result.get_result().is_ok());
}

TEST_F(WasmMozJSTest, CreateFunctionWithInvalidSourceFails) {
    ASSERT_TRUE(initEngine());

    // Invalid JavaScript - not a function expression
    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    wc::Val srcArg(makeListU8("this is not valid javascript {{{"));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    ASSERT_TRUE(result.is_result());
    // Invalid JS source should produce an error result.
    ASSERT_FALSE(result.get_result().is_ok());
}

// ---------------------------------------------------------------------------
// Error extraction and failure scenario tests
//
// These tests exercise failure paths and verify that the wasm-mozjs-error
// record returned by WIT contains the correct error code, message, filename,
// and line/column information for various failure modes.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, CompileErrorHasCorrectErrorCode) {
    ASSERT_TRUE(initEngine());

    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    wc::Val srcArg(makeListU8("this is not valid javascript {{{"));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    ASSERT_TRUE(result.is_result());
    ASSERT_FALSE(result.get_result().is_ok());

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-compile");
}

TEST_F(WasmMozJSTest, CompileErrorContainsMessage) {
    ASSERT_TRUE(initEngine());

    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    wc::Val srcArg(makeListU8("function( { invalid syntax"));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-compile");
    ASSERT_FALSE(error->msg.empty());
}

TEST_F(WasmMozJSTest, CompileErrorHasFileAndLineInfo) {
    ASSERT_TRUE(initEngine());

    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    wc::Val srcArg(makeListU8("function() { var x = ; }"));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-compile");
    ASSERT_FALSE(error->filename.empty());
    ASSERT_GT(error->line, 0u);
    ASSERT_GT(error->column, 0u);
}

TEST_F(WasmMozJSTest, RuntimeErrorHasCorrectErrorCode) {
    ASSERT_TRUE(initEngine());

    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    // Valid function that throws at runtime
    uint64_t handle = createFunction("function() { throw new Error('boom'); }");
    ASSERT_NE(handle, 0u);

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    ASSERT_TRUE(result.is_result());
    ASSERT_FALSE(result.get_result().is_ok());

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-runtime");
}

TEST_F(WasmMozJSTest, RuntimeErrorContainsMessage) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { throw new Error('specific error text'); }");
    ASSERT_NE(handle, 0u);

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-runtime");
    ASSERT_TRUE(error->msg.find("specific error text") != std::string::npos);
}

TEST_F(WasmMozJSTest, RuntimeErrorFromUndefinedVariable) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { return nonexistentVariable.property; }");
    ASSERT_NE(handle, 0u);

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-runtime");
    ASSERT_TRUE(error->msg.find("nonexistentVariable") != std::string::npos);
}

TEST_F(WasmMozJSTest, RuntimeErrorFromTypeError) {
    ASSERT_TRUE(initEngine());

    // Calling a non-function triggers a TypeError at runtime
    uint64_t handle = createFunction("function() { var x = 42; return x(); }");
    ASSERT_NE(handle, 0u);

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-runtime");
    ASSERT_FALSE(error->msg.empty());
}

TEST_F(WasmMozJSTest, TypeErrorFromNonFunctionSource) {
    ASSERT_TRUE(initEngine());

    // "42" evaluates to a number, not a function
    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    wc::Val srcArg(makeListU8("42"));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-type");
}

TEST_F(WasmMozJSTest, InvalidUtf8InCreateFunction) {
    ASSERT_TRUE(initEngine());

    auto createFunc = getFunc("create-function");
    ASSERT_TRUE(createFunc.has_value());

    // 0xFE 0xFF are never valid in UTF-8.
    std::string bad = "function() { return '\xFE\xFF'; }";
    wc::Val srcArg(makeListU8(bad));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*createFunc, ctx(), &result, 1, std::move(srcArg));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_FALSE(error->msg.empty()) << "expected an error message for invalid UTF-8";
}

TEST_F(WasmMozJSTest, InvalidHandleHasCorrectErrorCode) {
    ASSERT_TRUE(initEngine());

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(uint64_t(0));
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-invalid-arg");
}

TEST_F(WasmMozJSTest, StaleHandleAfterShutdownHasCorrectErrorCode) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { return 1; }");
    ASSERT_NE(handle, 0u);

    // Shutdown and re-initialize
    auto shutdownFunc = getFunc("shutdown-engine");
    ASSERT_TRUE(shutdownFunc.has_value());
    wc::Val shutdownResult(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFuncNoArgs(*shutdownFunc, ctx(), &shutdownResult, 1));
    ASSERT_TRUE(isResultOk(shutdownResult));

    ASSERT_TRUE(initEngine());

    // Old handle should be invalid after re-init
    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-invalid-arg");
}

TEST_F(WasmMozJSTest, OutOfRangeHandleHasCorrectErrorCode) {
    ASSERT_TRUE(initEngine());

    auto invokeFunc = getFunc("invoke-function");
    ASSERT_TRUE(invokeFunc.has_value());

    // Handle with a very large index that's never been allocated
    wc::Val arg0(uint64_t(0xFFFFFFFF));
    wc::Val arg1(makeListU8(BSONObj()));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    callFunc(*invokeFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-invalid-arg");
}

TEST_F(WasmMozJSTest, ShutdownAndReinitialize) {
    // First init
    ASSERT_TRUE(initEngine());

    uint64_t h1 = createFunction("function() { return 'first'; }");
    ASSERT_NE(h1, 0u);

    // Shutdown
    auto shutdownFunc = getFunc("shutdown-engine");
    ASSERT_TRUE(shutdownFunc.has_value());
    wc::Val shutdownResult(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFuncNoArgs(*shutdownFunc, ctx(), &shutdownResult, 1));
    ASSERT_TRUE(isResultOk(shutdownResult));

    // Re-initialize
    ASSERT_TRUE(initEngine());

    // Create a new function (old handles are invalid after shutdown)
    uint64_t h2 = createFunction("function() { return 'second'; }");
    ASSERT_NE(h2, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(h2, emptyArgs));
    BSONObj result = getReturnValueBson();
    ASSERT_FALSE(result.isEmpty());
}

TEST_F(WasmMozJSTest, FunctionReturningString) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction("function() { return \"hello world\"; }");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    ASSERT_FALSE(result.isEmpty());

    BSONElement retVal = result.getField("__returnValue");
    ASSERT_FALSE(retVal.eoo());
    ASSERT_EQ(retVal.type(), mongo::BSONType::string);
    ASSERT_EQ(retVal.str(), "hello world");
}

TEST_F(WasmMozJSTest, FunctionReturningNestedObject) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  return {"
        "    outer: {"
        "      inner: {"
        "        value: 99"
        "      }"
        "    }"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    ASSERT_FALSE(result.isEmpty());

    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    BSONObj inner = retVal.Obj().getObjectField("outer").getObjectField("inner");
    ASSERT_EQ(inner.getIntField("value"), 99);
}

// ---------------------------------------------------------------------------
// set-global / get-global tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, SetGlobalGetGlobalRoundTrip) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder bob;
    bob.append("x", 10);
    bob.append("y", "hello");
    BSONObj value = bob.obj();

    ASSERT_TRUE(setGlobal("myVar", value));

    BSONObj retrieved = getGlobal("myVar");
    ASSERT_FALSE(retrieved.isEmpty());
    ASSERT_EQ(retrieved.getIntField("x"), 10);
    ASSERT_EQ(retrieved.getStringField("y"), "hello");
}

TEST_F(WasmMozJSTest, GetGlobalNonexistent) {
    ASSERT_TRUE(initEngine());

    // get-global for a name that was never set should return empty (error).
    // If get-global is not yet implemented, it also returns empty - either way
    // the assertion holds.
    BSONObj retrieved = getGlobal("nonexistent_global_12345");
    // get-global for a nonexistent name should return an error (empty).
    ASSERT_TRUE(retrieved.isEmpty());
}

TEST_F(WasmMozJSTest, SetGlobalThenUseInFunction) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder bob;
    bob.append("factor", 7);
    ASSERT_TRUE(setGlobal("config", bob.obj()));

    // JS function that reads the global we set (by name) and uses it.
    // set-global should install "config" on the JS global scope.
    uint64_t handle = createFunction(
        "function(n) {"
        "  if (typeof config === 'undefined') return { result: -1 };"
        "  return { result: n * config.factor };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder args;
    args.append("n", 6);
    ASSERT_TRUE(invokeFunction(handle, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    // 6 * config.factor(7) = 42.
    ASSERT_EQ(retVal.Obj().getIntField("result"), 42);
}

TEST_F(WasmMozJSTest, SetGlobalOverwrite) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder b1;
    b1.append("v", 1);
    ASSERT_TRUE(setGlobal("g", b1.obj()));
    ASSERT_EQ(getGlobal("g").getIntField("v"), 1);

    // Overwrite with a new value
    BSONObjBuilder b2;
    b2.append("v", 2);
    b2.append("extra", "second");
    ASSERT_TRUE(setGlobal("g", b2.obj()));
    BSONObj g2 = getGlobal("g");
    ASSERT_EQ(g2.getIntField("v"), 2);
    ASSERT_EQ(g2.getStringField("extra"), "second");
}

TEST_F(WasmMozJSTest, SetGlobalMultipleNames) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder b1;
    b1.append("val", 1);
    ASSERT_TRUE(setGlobal("alpha", b1.obj()));

    BSONObjBuilder b2;
    b2.append("val", 2);
    ASSERT_TRUE(setGlobal("beta", b2.obj()));

    BSONObjBuilder b3;
    b3.append("val", 3);
    ASSERT_TRUE(setGlobal("gamma", b3.obj()));

    // Each global should be independently retrievable
    ASSERT_EQ(getGlobal("alpha").getIntField("val"), 1);
    ASSERT_EQ(getGlobal("beta").getIntField("val"), 2);
    ASSERT_EQ(getGlobal("gamma").getIntField("val"), 3);
}

TEST_F(WasmMozJSTest, SetGlobalVariousTypes) {
    ASSERT_TRUE(initEngine());

    // Set global with various BSON types and verify round-trip via get-global
    {
        BSONObjBuilder bob;
        bob.append("n", 42);
        bob.append("d", 3.14);
        bob.append("s", "hello");
        bob.appendBool("b", true);
        bob.appendNull("nil");
        ASSERT_TRUE(setGlobal("mixed", bob.obj()));
    }

    BSONObj retrieved = getGlobal("mixed");
    ASSERT_FALSE(retrieved.isEmpty());
    ASSERT_EQ(retrieved.getIntField("n"), 42);
    ASSERT_APPROX_EQUAL(retrieved.getField("d").Number(), 3.14, 1e-10);
    ASSERT_EQ(retrieved.getStringField("s"), "hello");
    ASSERT_EQ(retrieved.getBoolField("b"), true);
    ASSERT_EQ(retrieved.getField("nil").type(), BSONType::null);
}

TEST_F(WasmMozJSTest, SetGlobalVisibleAcrossFunctions) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder bob;
    bob.append("counter", 100);
    ASSERT_TRUE(setGlobal("state", bob.obj()));

    // Two different functions should both see the same global
    uint64_t h1 = createFunction(
        "function() {"
        "  return { val: state.counter + 1 };"
        "}");
    ASSERT_NE(h1, 0u);

    uint64_t h2 = createFunction(
        "function() {"
        "  return { val: state.counter + 2 };"
        "}");
    ASSERT_NE(h2, 0u);

    BSONObj emptyArgs;

    ASSERT_TRUE(invokeFunction(h1, emptyArgs));
    BSONObj r1 = getReturnValueBson();
    ASSERT_EQ(r1.getField("__returnValue").Obj().getIntField("val"), 101);

    ASSERT_TRUE(invokeFunction(h2, emptyArgs));
    BSONObj r2 = getReturnValueBson();
    ASSERT_EQ(r2.getField("__returnValue").Obj().getIntField("val"), 102);
}

// ---------------------------------------------------------------------------
// Stored procedure tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, StoredProcedureViaSetGlobalCode) {
    ASSERT_TRUE(initEngine());

    // Install a stored procedure as a global via BSON Code type.
    BSONObjBuilder bob;
    bob.appendCode("multiply", "function(a, b) { return a * b; }");
    ASSERT_TRUE(setGlobal("procs", bob.obj()));

    uint64_t handle = createFunction(
        "function(x, y) {"
        "  return { result: procs.multiply(x, y) };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder args;
    args.append("x", 6);
    args.append("y", 7);
    ASSERT_TRUE(invokeFunction(handle, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getIntField("result"), 42);
}

TEST_F(WasmMozJSTest, StoredProcedureMultiple) {
    ASSERT_TRUE(initEngine());

    // Install multiple stored procedures at once.
    BSONObjBuilder bob;
    bob.appendCode("add", "function(a, b) { return a + b; }");
    bob.appendCode("square", "function(x) { return x * x; }");
    bob.appendCode("negate", "function(x) { return -x; }");
    ASSERT_TRUE(setGlobal("math", bob.obj()));

    uint64_t handle = createFunction(
        "function(a, b) {"
        "  var sum = math.add(a, b);"
        "  var sq = math.square(sum);"
        "  return { result: math.negate(sq) };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder args;
    args.append("a", 3);
    args.append("b", 4);
    ASSERT_TRUE(invokeFunction(handle, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    // negate(square(add(3, 4))) = negate(square(7)) = negate(49) = -49
    ASSERT_EQ(retVal.Obj().getIntField("result"), -49);
}

TEST_F(WasmMozJSTest, StoredProcedureChaining) {
    ASSERT_TRUE(initEngine());

    // Install procedures that call each other.
    BSONObjBuilder bob;
    bob.appendCode("double_", "function(x) { return x * 2; }");
    bob.appendCode("doubleAndAdd", "function(a, b) { return utils.double_(a) + b; }");
    ASSERT_TRUE(setGlobal("utils", bob.obj()));

    uint64_t handle = createFunction(
        "function(n) {"
        "  return { result: utils.doubleAndAdd(n, 10) };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder args;
    args.append("n", 5);
    ASSERT_TRUE(invokeFunction(handle, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    // doubleAndAdd(5, 10) = double_(5) + 10 = 10 + 10 = 20
    ASSERT_EQ(retVal.Obj().getIntField("result"), 20);
}

TEST_F(WasmMozJSTest, StoredProcedureCalledFromMultipleFunctions) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder bob;
    bob.appendCode("transform", "function(x) { return x * x + 1; }");
    ASSERT_TRUE(setGlobal("sp", bob.obj()));

    // Two different functions both use the same stored procedure.
    uint64_t h1 = createFunction("function(n) { return { result: sp.transform(n) }; }");
    ASSERT_NE(h1, 0u);

    uint64_t h2 =
        createFunction("function(a, b) { return { result: sp.transform(a) + sp.transform(b) }; }");
    ASSERT_NE(h2, 0u);

    {
        BSONObjBuilder a;
        a.append("n", 3);
        ASSERT_TRUE(invokeFunction(h1, a.obj()));
        BSONObj r = getReturnValueBson();
        // transform(3) = 3*3 + 1 = 10
        ASSERT_EQ(r.getField("__returnValue").Obj().getIntField("result"), 10);
    }

    {
        BSONObjBuilder a;
        a.append("a", 2);
        a.append("b", 4);
        ASSERT_TRUE(invokeFunction(h2, a.obj()));
        BSONObj r = getReturnValueBson();
        // transform(2) + transform(4) = (4+1) + (16+1) = 5 + 17 = 22
        ASSERT_EQ(r.getField("__returnValue").Obj().getIntField("result"), 22);
    }
}

TEST_F(WasmMozJSTest, StoredProcedureWithCodeWScope) {
    ASSERT_TRUE(initEngine());

    // CodeWScope is also evaluated as a function (scope is ignored per SpiderMonkey behavior).
    BSONObjBuilder bob;
    bob.appendCodeWScope("greet", "function(name) { return 'Hello, ' + name; }", BSONObj());
    ASSERT_TRUE(setGlobal("funcs", bob.obj()));

    uint64_t handle = createFunction("function() { return { msg: funcs.greet('World') }; }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getStringField("msg"), "Hello, World");
}

TEST_F(WasmMozJSTest, GlobalScopeContainsExpectedVars) {
    ASSERT_TRUE(initEngine());

    // Enumerate all global property names from the JS environment.
    uint64_t handle = createFunction(
        "function() {"
        "  var global = (function() { return this; })();"
        "  return Object.getOwnPropertyNames(global);"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_EQ(retVal.type(), BSONType::array);

    // Collect all property names into a set.
    BSONObj arr = retVal.Obj();
    std::set<std::string> globals;
    for (const auto& elem : arr) {
        if (elem.type() == BSONType::string) {
            globals.insert(elem.str());
        }
    }

    // Verify custom globals installed by GlobalInfo::freeFunctions.
    for (const auto& name : {"print", "gc", "sleep", "version", "buildInfo", "getJSHeapLimitMB"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }

    // Verify BSON type constructors installed via WrapType::install() (InstallType::Global).
    for (const auto& name : {"BinData",
                             "Code",
                             "DBPointer",
                             "DBRef",
                             "NumberDecimal",
                             "NumberInt",
                             "NumberLong",
                             "ObjectId",
                             "MaxKey",
                             "MinKey",
                             "Timestamp"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }

    // Verify BinDataInfo::freeFunctions (installed as globals by WrapType).
    for (const auto& name : {"HexData", "MD5", "UUID"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }

    // Verify BSONInfo::freeFunctions (Private type, but freeFunctions still go on global).
    for (const auto& name :
         {"bsonWoCompare", "bsonUnorderedFieldsCompare", "bsonBinaryEqual", "bsonObjToArray"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }

    // Verify standard JS built-ins are present.
    for (const auto& name : {"Object",
                             "Array",
                             "Math",
                             "JSON",
                             "Date",
                             "RegExp",
                             "String",
                             "Number",
                             "Boolean",
                             "Error",
                             "Map",
                             "Set",
                             "Promise"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }
}

// ---------------------------------------------------------------------------
// Prototype method tests
//
// Each BSON type installed by MozJSPrototypeInstaller exposes a constructor
// and/or prototype methods.  These tests verify that the constructors create
// valid instances and that the expected methods exist and return sensible
// values.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, BinDataConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    // BinData(subtype, base64str) constructor + prototype methods
    uint64_t handle = createFunction(
        "function() {"
        "  var bd = BinData(0, 'AQIDBA==');"  // bytes [1,2,3,4]
        "  return {"
        "    hasBase64:  typeof bd.base64  === 'function',"
        "    hasHex:     typeof bd.hex     === 'function',"
        "    hasToStr:   typeof bd.toString === 'function',"
        "    hasToJSON:  typeof bd.toJSON  === 'function',"
        "    b64Val:     bd.base64(),"
        "    hexVal:     bd.hex()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasBase64"));
    ASSERT_TRUE(inner.getBoolField("hasHex"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_EQ(inner.getStringField("b64Val"), "AQIDBA==");
    ASSERT_EQ(inner.getStringField("hexVal"), "01020304");
}

TEST_F(WasmMozJSTest, BinDataFreeFunctions) {
    ASSERT_TRUE(initEngine());

    // HexData, MD5, UUID are global free functions from BinDataInfo.
    uint64_t handle = createFunction(
        "function() {"
        "  return {"
        "    hasHexData: typeof HexData === 'function',"
        "    hasMD5:     typeof MD5     === 'function',"
        "    hasUUID:    typeof UUID    === 'function',"
        "    hexDataOk:  HexData(0, '01020304').hex() === '01020304'"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasHexData"));
    ASSERT_TRUE(inner.getBoolField("hasMD5"));
    ASSERT_TRUE(inner.getBoolField("hasUUID"));
    ASSERT_TRUE(inner.getBoolField("hexDataOk"));
}

TEST_F(WasmMozJSTest, NumberIntConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var ni = NumberInt(42);"
        "  return {"
        "    hasToNumber: typeof ni.toNumber === 'function',"
        "    hasToStr:    typeof ni.toString === 'function',"
        "    hasToJSON:   typeof ni.toJSON   === 'function',"
        "    hasValueOf:  typeof ni.valueOf  === 'function',"
        "    numVal:      ni.toNumber(),"
        "    strVal:      ni.toString(),"
        "    valOf:       ni.valueOf()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToNumber"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasValueOf"));
    ASSERT_EQ(inner.getIntField("numVal"), 42);
    ASSERT_EQ(inner.getStringField("strVal"), "NumberInt(42)");
    ASSERT_EQ(inner.getIntField("valOf"), 42);
}

TEST_F(WasmMozJSTest, NumberLongConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var nl = NumberLong('1234567890123');"
        "  return {"
        "    hasToNumber: typeof nl.toNumber === 'function',"
        "    hasToStr:    typeof nl.toString === 'function',"
        "    hasToJSON:   typeof nl.toJSON   === 'function',"
        "    hasValueOf:  typeof nl.valueOf  === 'function',"
        "    hasCompare:  typeof nl.compare  === 'function',"
        "    strVal:      nl.toString(),"
        "    floatApprox: nl.floatApprox"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToNumber"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasValueOf"));
    ASSERT_TRUE(inner.getBoolField("hasCompare"));
    ASSERT_EQ(inner.getStringField("strVal"), "NumberLong(\"1234567890123\")");
}

TEST_F(WasmMozJSTest, NumberDecimalConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var nd = NumberDecimal('123.456');"
        "  return {"
        "    hasToStr:   typeof nd.toString === 'function',"
        "    hasToJSON:  typeof nd.toJSON   === 'function',"
        "    strVal:     nd.toString()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_EQ(inner.getStringField("strVal"), "NumberDecimal(\"123.456\")");
}

TEST_F(WasmMozJSTest, ObjectIdConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var oid = ObjectId();"
        "  return {"
        "    hasToStr:   typeof oid.toString === 'function',"
        "    hasToJSON:  typeof oid.toJSON   === 'function',"
        "    hasStr:     typeof oid.str      === 'string',"
        "    strLen:     oid.str.length,"  // ObjectId hex string is 24 chars
        "    toStrVal:   oid.toString()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasStr"));
    ASSERT_EQ(inner.getIntField("strLen"), 24);
    // toString() returns 'ObjectId("...")' format
    std::string toStr(inner.getStringField("toStrVal"));
    // toString() should return 'ObjectId("...")' format.
    ASSERT_TRUE(toStr.find("ObjectId(") == 0);
}

TEST_F(WasmMozJSTest, TimestampConstructorAndMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var ts = Timestamp(1700000000, 42);"
        "  return {"
        "    hasToJSON:  typeof ts.toJSON === 'function',"
        "    jsonVal:    ts.toJSON()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    // toJSON() returns { "$timestamp": { "t": ..., "i": ... } }
    BSONObj jsonVal = inner.getObjectField("jsonVal");
    // toJSON() returns { "$timestamp": { "t": ..., "i": ... } }.
    ASSERT_FALSE(jsonVal.isEmpty());
}

TEST_F(WasmMozJSTest, MinKeyMaxKeyMethods) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var mn = MinKey();"
        "  var mx = MaxKey();"
        "  return {"
        "    mnHasTojson:  typeof mn.tojson  === 'function',"
        "    mnHasToJSON:  typeof mn.toJSON  === 'function',"
        "    mxHasTojson:  typeof mx.tojson  === 'function',"
        "    mxHasToJSON:  typeof mx.toJSON  === 'function',"
        "    mnTojson:     mn.tojson(),"
        "    mxTojson:     mx.tojson()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("mnHasTojson"));
    ASSERT_TRUE(inner.getBoolField("mnHasToJSON"));
    ASSERT_TRUE(inner.getBoolField("mxHasTojson"));
    ASSERT_TRUE(inner.getBoolField("mxHasToJSON"));
    ASSERT_EQ(inner.getStringField("mnTojson"), "{ \"$minKey\" : 1 }");
    ASSERT_EQ(inner.getStringField("mxTojson"), "{ \"$maxKey\" : 1 }");
}

TEST_F(WasmMozJSTest, BSONFreeFunctionsWork) {
    ASSERT_TRUE(initEngine());

    // bsonWoCompare, bsonBinaryEqual, bsonObjToArray are global free functions
    // from BSONInfo (Private installType, but freeFunctions go on global).
    uint64_t handle = createFunction(
        "function() {"
        "  var a = {x: 1, y: 2};"
        "  var b = {x: 1, y: 2};"
        "  var c = {x: 2, y: 1};"
        "  return {"
        "    hasBsonWoCompare:  typeof bsonWoCompare === 'function',"
        "    hasBsonBinaryEqual: typeof bsonBinaryEqual === 'function',"
        "    hasBsonObjToArray: typeof bsonObjToArray === 'function',"
        "    hasBsonUnorderedFieldsCompare: typeof bsonUnorderedFieldsCompare === 'function',"
        "    eqCompare: bsonWoCompare(a, b) === 0,"
        "    neCompare: bsonWoCompare(a, c) !== 0,"
        "    binaryEq:  bsonBinaryEqual(a, b),"
        "    binaryNeq: !bsonBinaryEqual(a, c),"
        "    arrLen:    bsonObjToArray(a).length"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasBsonWoCompare"));
    ASSERT_TRUE(inner.getBoolField("hasBsonBinaryEqual"));
    ASSERT_TRUE(inner.getBoolField("hasBsonObjToArray"));
    ASSERT_TRUE(inner.getBoolField("hasBsonUnorderedFieldsCompare"));
    ASSERT_TRUE(inner.getBoolField("eqCompare"));
    ASSERT_TRUE(inner.getBoolField("neCompare"));
    ASSERT_TRUE(inner.getBoolField("binaryEq"));
    ASSERT_TRUE(inner.getBoolField("binaryNeq"));
    ASSERT_EQ(inner.getIntField("arrLen"), 2);
}

TEST_F(WasmMozJSTest, CodeConstructor) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var c = Code('function() { return 1; }');"
        "  return {"
        "    hasToStr: typeof c.toString === 'function',"
        "    strVal:   c.toString()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
}

TEST_F(WasmMozJSTest, NumberLongCompare) {
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  var a = NumberLong('100');"
        "  var b = NumberLong('200');"
        "  var c = NumberLong('100');"
        "  return {"
        "    aLtB:  a.compare(b) < 0,"
        "    bGtA:  b.compare(a) > 0,"
        "    aEqC:  a.compare(c) === 0"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_TRUE(inner.getBoolField("aLtB"));
    ASSERT_TRUE(inner.getBoolField("bGtA"));
    ASSERT_TRUE(inner.getBoolField("aEqC"));
}

TEST_F(WasmMozJSTest, NumberIntValueOfEnablesArithmetic) {
    ASSERT_TRUE(initEngine());

    // valueOf() allows NumberInt to participate in JS arithmetic
    uint64_t handle = createFunction(
        "function() {"
        "  var a = NumberInt(10);"
        "  var b = NumberInt(32);"
        "  return {"
        "    sum:      a + b,"
        "    product:  a * b,"
        "    valueOf:  a.valueOf()"
        "  };"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObj emptyArgs;
    ASSERT_TRUE(invokeFunction(handle, emptyArgs));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_EQ(inner.getIntField("sum"), 42);
    ASSERT_EQ(inner.getIntField("product"), 320);
    ASSERT_EQ(inner.getIntField("valueOf"), 10);
}

TEST_F(WasmMozJSTest, MQLFunctionPattern) {
    // Simulates: { $function: { body: <body>, args: ["$name"], lang: "js" } }
    // JsExecution::callFunction(func, params, thisObj={})
    // → Scope::invoke(func, &params, &{}, timeout, false)
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(name, factor) {"
        "  return { upper: name.toUpperCase(), doubled: factor * 2 };"
        "}");
    ASSERT_NE(handle, 0u);

    // Params are built as: { "arg0": <value>, "arg1": <value>, ... }
    BSONObjBuilder params;
    params.append("arg0", "hello");
    params.append("arg1", 21);
    ASSERT_TRUE(invokeFunction(handle, params.obj()));

    BSONObj result = getReturnValueBson();
    BSONElement retVal = result.getField("__returnValue");
    ASSERT_TRUE(retVal.isABSONObj());
    ASSERT_EQ(retVal.Obj().getStringField("upper"), "HELLO");
    ASSERT_EQ(retVal.Obj().getIntField("doubled"), 42);
}

TEST_F(WasmMozJSTest, MQLFunctionPatternWithDocumentArg) {
    // $function receives evaluated expressions as args; a common pattern is
    // passing the entire document as an argument.
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(doc) {"
        "  return doc.price * doc.qty;"
        "}");
    ASSERT_NE(handle, 0u);

    BSONObjBuilder params;
    {
        BSONObjBuilder doc(params.subobjStart("arg0"));
        doc.append("price", 15);
        doc.append("qty", 3);
    }
    ASSERT_TRUE(invokeFunction(handle, params.obj()));

    BSONObj result = getReturnValueBson();
    ASSERT_EQ(result.getField("__returnValue").numberInt(), 45);
}

TEST_F(WasmMozJSTest, MQLAccumulatorFullLifecycle) {
    // Simulates the $accumulator lifecycle:
    //   init() → state
    //   accumulate(state, val) → state  (per document)
    //   merge(s1, s2) → state           (across shards)
    //   finalize(state) → result
    ASSERT_TRUE(initEngine());

    uint64_t initFn = createFunction("function() { return { count: 0, sum: 0 }; }");
    uint64_t accFn = createFunction(
        "function(state, val) {"
        "  return { count: state.count + 1, sum: state.sum + val };"
        "}");
    uint64_t mergeFn = createFunction(
        "function(s1, s2) {"
        "  return { count: s1.count + s2.count, sum: s1.sum + s2.sum };"
        "}");
    uint64_t finalizeFn = createFunction(
        "function(state) {"
        "  return state.count > 0 ? state.sum / state.count : 0;"
        "}");

    ASSERT_NE(initFn, 0u);
    ASSERT_NE(accFn, 0u);
    ASSERT_NE(mergeFn, 0u);
    ASSERT_NE(finalizeFn, 0u);

    // Shard 1: init → accumulate [10, 20, 30]
    ASSERT_TRUE(invokeFunction(initFn, BSONObj()));
    BSONObj state1 = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    for (int val : {10, 20, 30}) {
        BSONObjBuilder a;
        a.append("state", state1);
        a.append("val", val);
        ASSERT_TRUE(invokeFunction(accFn, a.obj()));
        state1 = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    }
    ASSERT_EQ(state1.getIntField("count"), 3);
    ASSERT_EQ(state1.getIntField("sum"), 60);

    // Shard 2: init → accumulate [40]
    ASSERT_TRUE(invokeFunction(initFn, BSONObj()));
    BSONObj state2 = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    {
        BSONObjBuilder a;
        a.append("state", state2);
        a.append("val", 40);
        ASSERT_TRUE(invokeFunction(accFn, a.obj()));
        state2 = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    }
    ASSERT_EQ(state2.getIntField("count"), 1);
    ASSERT_EQ(state2.getIntField("sum"), 40);

    // Merge shard states
    {
        BSONObjBuilder a;
        a.append("s1", state1);
        a.append("s2", state2);
        ASSERT_TRUE(invokeFunction(mergeFn, a.obj()));
        state1 = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    }
    ASSERT_EQ(state1.getIntField("count"), 4);
    ASSERT_EQ(state1.getIntField("sum"), 100);

    // Finalize: average = 100/4 = 25
    {
        BSONObjBuilder a;
        a.append("state", state1);
        ASSERT_TRUE(invokeFunction(finalizeFn, a.obj()));
        BSONObj result = getReturnValueBson();
        ASSERT_APPROX_EQUAL(result.getField("__returnValue").Number(), 25.0, 1e-10);
    }
}

TEST_F(WasmMozJSTest, MQLAccumulatorPattern) {
    // Mirrors real $accumulator: setFunction("__accumulate", code) installs a JS
    // function as a directly-callable global via set-global-value with BSONType::Code.
    // A wrapper then calls __accumulate(state, val) in a loop.
    ASSERT_TRUE(initEngine());

    ASSERT_TRUE(setFunction("__accumulate",
                            "function(state, val) {"
                            "  return { count: state.count + 1, sum: state.sum + val };"
                            "}"));
    ASSERT_TRUE(setFunction("__merge",
                            "function(s1, s2) {"
                            "  return { count: s1.count + s2.count, sum: s1.sum + s2.sum };"
                            "}"));

    uint64_t wrapper = createFunction(
        "function(state, pendingCalls) {"
        "  for (var i = 0; i < pendingCalls.length; i++) {"
        "    state = __accumulate(state, pendingCalls[i]);"
        "  }"
        "  return state;"
        "}");
    ASSERT_NE(wrapper, 0u);

    BSONObjBuilder args;
    {
        BSONObjBuilder state(args.subobjStart("state"));
        state.append("count", 0);
        state.append("sum", 0);
    }
    {
        BSONArrayBuilder pending(args.subarrayStart("pendingCalls"));
        pending.append(10);
        pending.append(20);
        pending.append(30);
    }
    ASSERT_TRUE(invokeFunction(wrapper, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_EQ(inner.getIntField("count"), 3);
    ASSERT_EQ(inner.getIntField("sum"), 60);

    uint64_t mergeWrapper = createFunction(
        "function(state, pendingMerges) {"
        "  for (var i = 0; i < pendingMerges.length; i++) {"
        "    state = __merge(state, pendingMerges[i]);"
        "  }"
        "  return state;"
        "}");
    ASSERT_NE(mergeWrapper, 0u);

    BSONObjBuilder mergeArgs;
    mergeArgs.append("state", inner);
    {
        BSONArrayBuilder pending(mergeArgs.subarrayStart("pendingMerges"));
        {
            BSONObjBuilder s(pending.subobjStart());
            s.append("count", 2);
            s.append("sum", 70);
        }
    }
    ASSERT_TRUE(invokeFunction(mergeWrapper, mergeArgs.obj()));

    BSONObj merged = getReturnValueBson().getField("__returnValue").Obj().getOwned();
    ASSERT_EQ(merged.getIntField("count"), 5);
    ASSERT_EQ(merged.getIntField("sum"), 130);
}

TEST_F(WasmMozJSTest, MQLWherePattern) {
    // $where calls: invoke(func, nullptr, &document, timeout)
    // where document becomes 'this'. invoke-predicate handles this directly.
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function() {"
        "  return this.x > this.y;"
        "}");
    ASSERT_NE(handle, 0u);

    // Document that passes filter
    ASSERT_TRUE(invokePredicate(handle, BSON("x" << 10 << "y" << 5)));

    // Document that fails filter
    ASSERT_FALSE(invokePredicate(handle, BSON("x" << 3 << "y" << 7)));
}

TEST_F(WasmMozJSTest, MQLMapReduceReducePattern) {
    // mapReduce.reduce calls: invoke(reduceFunc, &{key, values}, &{}, timeout)
    // 'this' is an empty object. invoke-function (no this binding) works.
    ASSERT_TRUE(initEngine());

    uint64_t reduceFn = createFunction(
        "function(key, values) {"
        "  var total = 0;"
        "  for (var i = 0; i < values.length; i++) {"
        "    total += values[i];"
        "  }"
        "  return total;"
        "}");
    ASSERT_NE(reduceFn, 0u);

    BSONObjBuilder args;
    args.append("key", "electronics");
    {
        BSONArrayBuilder values(args.subarrayStart("values"));
        values.append(100);
        values.append(250);
        values.append(75);
        values.append(300);
    }
    ASSERT_TRUE(invokeFunction(reduceFn, args.obj()));

    BSONObj result = getReturnValueBson();
    ASSERT_EQ(result.getField("__returnValue").numberInt(), 725);
}

TEST_F(WasmMozJSTest, MQLMapReduceFinalizePattern) {
    // mapReduce.finalize uses $function pattern:
    // invoke(finalizeFunc, &{key, reducedValue}, &{}, timeout)
    ASSERT_TRUE(initEngine());

    uint64_t finalizeFn = createFunction(
        "function(key, reducedValue) {"
        "  return { category: key, total: reducedValue, formatted: key + ': $' + reducedValue };"
        "}");
    ASSERT_NE(finalizeFn, 0u);

    BSONObjBuilder args;
    args.append("key", "electronics");
    args.append("reducedValue", 725);
    ASSERT_TRUE(invokeFunction(finalizeFn, args.obj()));

    BSONObj result = getReturnValueBson();
    BSONObj inner = result.getField("__returnValue").Obj();
    ASSERT_EQ(inner.getStringField("category"), "electronics");
    ASSERT_EQ(inner.getIntField("total"), 725);
    ASSERT_EQ(inner.getStringField("formatted"), "electronics: $725");
}

TEST_F(WasmMozJSTest, MQLMapReduceMapPattern) {
    // mapReduce.map calls: invoke(mapFunc, nullptr, &document, timeout)
    // where document becomes 'this' and the function calls emit(key, value).
    // invoke-map handles this: document as `this`, emits buffered.
    ASSERT_TRUE(initEngine());
    ASSERT_TRUE(setupEmit());

    uint64_t mapFn = createFunction(
        "function() {"
        "  emit(this.category, this.price);"
        "}");
    ASSERT_NE(mapFn, 0u);

    ASSERT_TRUE(invokeMap(mapFn, BSON("category" << "electronics" << "price" << 100)));
    ASSERT_TRUE(invokeMap(mapFn, BSON("category" << "books" << "price" << 25)));
    ASSERT_TRUE(invokeMap(mapFn, BSON("category" << "electronics" << "price" << 250)));

    BSONObj emitDoc = drainEmitBuffer();
    auto emitsArr = emitDoc["emits"].Array();
    ASSERT_EQ(emitsArr.size(), 3u);
    ASSERT_EQ(emitsArr[0].Obj()["k"].str(), "electronics");
    ASSERT_EQ(emitsArr[0].Obj()["v"].numberInt(), 100);
    ASSERT_EQ(emitsArr[1].Obj()["k"].str(), "books");
    ASSERT_EQ(emitsArr[1].Obj()["v"].numberInt(), 25);
    ASSERT_EQ(emitsArr[2].Obj()["k"].str(), "electronics");
    ASSERT_EQ(emitsArr[2].Obj()["v"].numberInt(), 250);
}

TEST_F(WasmMozJSTest, MQLFunctionReusedAcrossDocuments) {
    // $function/$accumulator create the function once
    // and invoke it per-document. This tests handle reuse across many calls.
    ASSERT_TRUE(initEngine());

    uint64_t handle = createFunction(
        "function(price, qty) {"
        "  return price * qty;"
        "}");
    ASSERT_NE(handle, 0u);

    struct TestCase {
        int price;
        int qty;
        int expected;
    };
    TestCase cases[] = {
        {10, 5, 50},
        {25, 2, 50},
        {3, 100, 300},
        {0, 42, 0},
        {7, 7, 49},
    };

    for (const auto& tc : cases) {
        BSONObjBuilder args;
        args.append("price", tc.price);
        args.append("qty", tc.qty);
        ASSERT_TRUE(invokeFunction(handle, args.obj()));
        BSONObj result = getReturnValueBson();
        ASSERT_EQ(result.getField("__returnValue").numberInt(), tc.expected);
    }
}


// ---------------------------------------------------------------------------
// invoke-predicate tests ($where pattern)
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, InvokePredicateReturnsTrueForMatch) {
    initEngine();
    auto handle = createFunction("function() { return this.x > this.y; }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokePredicate(handle, BSON("x" << 10 << "y" << 5)));
}

TEST_F(WasmMozJSTest, InvokePredicateReturnsFalseForNonMatch) {
    initEngine();
    auto handle = createFunction("function() { return this.x > this.y; }");
    ASSERT_NE(handle, 0u);

    ASSERT_FALSE(invokePredicate(handle, BSON("x" << 3 << "y" << 7)));
}

TEST_F(WasmMozJSTest, InvokePredicateFieldAccess) {
    initEngine();
    auto handle = createFunction("function() { return this.name === 'hello'; }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokePredicate(handle, BSON("name" << "hello")));
    ASSERT_FALSE(invokePredicate(handle, BSON("name" << "world")));
}

TEST_F(WasmMozJSTest, InvokePredicateMultipleDocuments) {
    initEngine();
    auto handle = createFunction("function() { return this.age >= 18; }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokePredicate(handle, BSON("age" << 25)));
    ASSERT_FALSE(invokePredicate(handle, BSON("age" << 10)));
    ASSERT_TRUE(invokePredicate(handle, BSON("age" << 18)));
    ASSERT_FALSE(invokePredicate(handle, BSON("age" << 17)));
}

TEST_F(WasmMozJSTest, InvokePredicateRuntimeError) {
    initEngine();
    auto handle = createFunction("function() { throw new Error('pred error'); }");
    ASSERT_NE(handle, 0u);

    auto func = getFunc("invoke-predicate");
    ASSERT_TRUE(func.has_value());
    wc::Val arg0(handle);
    wc::Val arg1(makeListU8(BSON("x" << 1)));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*func, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_FALSE(isResultOk(result));

    auto error = extractError(result);
    ASSERT_TRUE(error.has_value());
    ASSERT_EQ(error->code, "e-runtime");
}

// ---------------------------------------------------------------------------
// invoke-map tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, InvokeMapEmitsDocuments) {
    initEngine();
    ASSERT_TRUE(setupEmit());

    auto handle = createFunction("function() { emit(this.category, this.price); }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokeMap(handle, BSON("category" << "A" << "price" << 10)));
    ASSERT_TRUE(invokeMap(handle, BSON("category" << "B" << "price" << 20)));
    ASSERT_TRUE(invokeMap(handle, BSON("category" << "A" << "price" << 30)));

    BSONObj emitDoc = drainEmitBuffer();
    auto emitsArr = emitDoc["emits"].Array();
    ASSERT_EQ(emitsArr.size(), 3u);
    ASSERT_EQ(emitsArr[0].Obj()["k"].str(), "A");
    ASSERT_EQ(emitsArr[0].Obj()["v"].numberInt(), 10);
    ASSERT_EQ(emitsArr[1].Obj()["k"].str(), "B");
    ASSERT_EQ(emitsArr[1].Obj()["v"].numberInt(), 20);
    ASSERT_EQ(emitsArr[2].Obj()["k"].str(), "A");
    ASSERT_EQ(emitsArr[2].Obj()["v"].numberInt(), 30);
}

TEST_F(WasmMozJSTest, InvokeMapRuntimeError) {
    initEngine();
    auto handle = createFunction("function() { throw new Error('map error'); }");
    ASSERT_NE(handle, 0u);

    ASSERT_FALSE(invokeMap(handle, BSON("x" << 1)));
}

TEST_F(WasmMozJSTest, InvokeFunctionNoThisBinding) {
    initEngine();
    auto handle = createFunction("function() { return typeof this; }");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj result = getReturnValueBson();
    ASSERT_EQ(result["__returnValue"].str(), "object");
}

// ---------------------------------------------------------------------------
// set-global-value tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, SetGlobalValueBoolean) {
    initEngine();
    auto setFunc = getFunc("set-global-value");
    ASSERT_TRUE(setFunc.has_value());

    BSONObj boolDoc = BSON("val" << true);
    wc::Val arg0 = makeString("fullObject");
    wc::Val arg1(makeListU8(boolDoc));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*setFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_TRUE(isResultOk(result));

    // Verify: fullObject should be boolean true, not an object
    auto handle = createFunction("function() { return fullObject === true; }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj ret = getReturnValueBson();
    ASSERT_TRUE(ret["__returnValue"].boolean());
}

TEST_F(WasmMozJSTest, SetGlobalValueNumber) {
    initEngine();
    auto setFunc = getFunc("set-global-value");
    ASSERT_TRUE(setFunc.has_value());

    BSONObj numDoc = BSON("val" << 3.14);
    wc::Val arg0 = makeString("pi");
    wc::Val arg1(makeListU8(numDoc));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*setFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_TRUE(isResultOk(result));

    auto handle = createFunction("function() { return pi; }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj ret = getReturnValueBson();
    ASSERT_APPROX_EQUAL(ret["__returnValue"].numberDouble(), 3.14, 0.001);
}

TEST_F(WasmMozJSTest, SetGlobalValueString) {
    initEngine();
    auto setFunc = getFunc("set-global-value");
    ASSERT_TRUE(setFunc.has_value());

    BSONObj strDoc = BSON("val" << "world");
    wc::Val arg0 = makeString("greeting");
    wc::Val arg1(makeListU8(strDoc));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*setFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_TRUE(isResultOk(result));

    auto handle = createFunction("function() { return greeting; }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj ret = getReturnValueBson();
    ASSERT_EQ(ret["__returnValue"].str(), "world");
}

TEST_F(WasmMozJSTest, SetGlobalValueCodeFunction) {
    initEngine();
    auto setFunc = getFunc("set-global-value");
    ASSERT_TRUE(setFunc.has_value());

    // Use BSONType::Code to set a callable function as a global value
    BSONObjBuilder b;
    b.appendCode("val", "function(x) { return x * 2; }");
    BSONObj codeDoc = b.obj();

    wc::Val arg0 = makeString("doubler");
    wc::Val arg1(makeListU8(codeDoc));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*setFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_TRUE(isResultOk(result));

    // doubler should now be a callable function
    auto handle = createFunction("function(n) { return doubler(n); }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSON("0" << 21)));
    BSONObj ret = getReturnValueBson();
    ASSERT_EQ(ret["__returnValue"].numberInt(), 42);
}

TEST_F(WasmMozJSTest, SetGlobalValueObject) {
    initEngine();
    auto setFunc = getFunc("set-global-value");
    ASSERT_TRUE(setFunc.has_value());

    // Setting an object via set-global-value should set it as-is
    BSONObj objDoc = BSON("val" << BSON("a" << 1 << "b" << 2));
    wc::Val arg0 = makeString("config");
    wc::Val arg1(makeListU8(objDoc));
    wc::Val result(wc::WitResult::ok(std::nullopt));
    ASSERT_TRUE(callFunc(*setFunc, ctx(), &result, 1, std::move(arg0), std::move(arg1)));
    ASSERT_TRUE(isResultOk(result));

    auto handle = createFunction("function() { return config.a + config.b; }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    BSONObj ret = getReturnValueBson();
    ASSERT_EQ(ret["__returnValue"].numberInt(), 3);
}

// ---------------------------------------------------------------------------
// `emit` in-WASM boundary
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, SetupEmitAndDrain) {
    initEngine();
    ASSERT_TRUE(setupEmit());

    auto handle = createFunction("function(doc) { emit(doc.key, doc.val); }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSON("0" << BSON("key" << "a" << "val" << 1))));
    ASSERT_TRUE(invokeFunction(handle, BSON("0" << BSON("key" << "b" << "val" << 2))));

    BSONObj emitDoc = drainEmitBuffer();
    auto emitsArr = emitDoc["emits"].Array();
    ASSERT_EQ(emitsArr.size(), 2u);
    ASSERT_EQ(emitsArr[0].Obj()["k"].str(), "a");
    ASSERT_EQ(emitsArr[0].Obj()["v"].numberInt(), 1);
    ASSERT_EQ(emitsArr[1].Obj()["k"].str(), "b");
    ASSERT_EQ(emitsArr[1].Obj()["v"].numberInt(), 2);
}

TEST_F(WasmMozJSTest, EmitDrainClearsBetweenCalls) {
    initEngine();
    ASSERT_TRUE(setupEmit());

    auto handle = createFunction("function() { emit('x', 10); }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));

    BSONObj d1 = drainEmitBuffer();
    ASSERT_EQ(d1["emits"].Array().size(), 1u);

    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));

    BSONObj d2 = drainEmitBuffer();
    ASSERT_EQ(d2["emits"].Array().size(), 2u);
}

TEST_F(WasmMozJSTest, EmitDrainEmptyBuffer) {
    initEngine();
    ASSERT_TRUE(setupEmit());

    BSONObj doc = drainEmitBuffer();
    ASSERT_EQ(doc["emits"].Array().size(), 0u);
    ASSERT_EQ(doc["bytesUsed"].numberLong(), 0);
}

TEST_F(WasmMozJSTest, EmitWithUndefinedKey) {
    initEngine();
    ASSERT_TRUE(setupEmit());

    auto handle = createFunction("function() { emit(undefined, 42); }");
    ASSERT_NE(handle, 0u);
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));

    BSONObj doc = drainEmitBuffer();
    auto emits = doc["emits"].Array();
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_TRUE(emits[0].Obj()["k"].isNull());
    ASSERT_EQ(emits[0].Obj()["v"].numberInt(), 42);
}

// ---------------------------------------------------------------------------
// OOM tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSTest, EmitByteLimitEnforced) {
    initEngine();
    ASSERT_TRUE(setupEmitWithLimit(256));

    auto handle = createFunction(
        "function() {"
        "  for (var i = 0; i < 100; i++) {"
        "    emit('key_' + i, 'value_' + i);"
        "  }"
        "}");
    ASSERT_NE(handle, 0u);

    auto err = invokeFunctionError(handle, BSONObj());
    ASSERT_EQ(err.code, "e-runtime");
    ASSERT_NE(err.msg.find("emit() exceeded memory limit"), std::string::npos);
}

TEST_F(WasmMozJSTest, EmitByteLimitEnforcedInMapReduce) {
    initEngine();
    // Each emitted {k: <string>, v: <int>} BSON doc is ~30 bytes.
    // A 64-byte limit allows ~2 emits before the 3rd exceeds.
    ASSERT_TRUE(setupEmitWithLimit(64));

    auto handle = createFunction(
        "function() {"
        "  emit(this.category, this.price);"
        "}");
    ASSERT_NE(handle, 0u);

    ASSERT_TRUE(invokeMap(handle, BSON("category" << "A" << "price" << 10)));
    ASSERT_TRUE(invokeMap(handle, BSON("category" << "B" << "price" << 20)));

    auto err = invokeMapError(handle, BSON("category" << "C" << "price" << 30));
    ASSERT_EQ(err.code, "e-runtime");
    ASSERT_NE(err.msg.find("emit() exceeded memory limit"), std::string::npos);
}

TEST_F(WasmMozJSTest, EmitByteLimitResetOnSetupEmit) {
    initEngine();
    ASSERT_TRUE(setupEmitWithLimit(128));

    auto handle = createFunction("function() { emit('k', 'v'); }");
    ASSERT_NE(handle, 0u);

    // Emit a few times, approaching the limit.
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));

    // Re-setup resets the buffer and byte counter.
    ASSERT_TRUE(setupEmitWithLimit(128));

    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));
    ASSERT_TRUE(invokeFunction(handle, BSONObj()));

    BSONObj doc = drainEmitBuffer();
    ASSERT_EQ(doc["emits"].Array().size(), 3u);
}

TEST_F(WasmMozJSTest, JsHeapOOMFromLargeArrayAllocation) {
    initEngine();

    // Keep multiple large strings alive simultaneously.  Doubling creates a
    // series of strings: 1 MB, 2 MB, 4 MB ... all pushed into an array so GC
    // cannot collect any of them.  Total live ≈ 2^(n+1) - 1 MB.
    auto handle = createFunction(
        "function() {"
        "  var arr = [];"
        "  var s = new Array(1024 * 1024 + 1).join('a');"
        "  for (var i = 0; i < 10; i++) {"
        "    arr.push(s);"
        "    s = s + s;"
        "  }"
        "  return arr.length;"
        "}");
    ASSERT_NE(handle, 0u);

    auto err = invokeFunctionError(handle, BSONObj());
    ASSERT_EQ(err.code, "e-runtime");
    bool hasOomMsg = err.msg.find("out of memory") != std::string::npos ||
        err.msg.find("allocation size overflow") != std::string::npos;
    ASSERT_TRUE(hasOomMsg) << "msg: " << err.msg;
}

TEST_F(WasmMozJSTest, JsHeapOOMFromStringConcatenation) {
    initEngine();

    auto handle = createFunction(
        "function() {"
        "  var s = new Array(1024 * 1024 + 1).join('x');"
        "  for (var i = 0; i < 10; i++) {"
        "    s = s + s;"
        "  }"
        "  return s.length;"
        "}");
    ASSERT_NE(handle, 0u);

    auto err = invokeFunctionError(handle, BSONObj());
    ASSERT_EQ(err.code, "e-runtime");
    bool hasOomMsg = err.msg.find("out of memory") != std::string::npos ||
        err.msg.find("allocation size overflow") != std::string::npos;
    ASSERT_TRUE(hasOomMsg) << "msg: " << err.msg;
}

TEST_F(WasmMozJSTest, JsHeapOomFromDeeplyNestedObject) {
    initEngine();

    // Build a chain of objects where each node holds an exponentially growing
    // string, preventing GC from collecting anything in the chain.
    auto handle = createFunction(
        "function() {"
        "  var s = new Array(1024 * 1024 + 1).join('b');"
        "  var obj = {val: s};"
        "  for (var i = 0; i < 10; i++) {"
        "    s = s + s;"
        "    obj = {inner: obj, val: s};"
        "  }"
        "  return obj.val.length;"
        "}");
    ASSERT_NE(handle, 0u);

    auto err = invokeFunctionError(handle, BSONObj());
    ASSERT_EQ(err.code, "e-runtime");
    bool hasOomMsg = err.msg.find("out of memory") != std::string::npos ||
        err.msg.find("allocation size overflow") != std::string::npos;
    ASSERT_TRUE(hasOomMsg) << "msg: " << err.msg;
}

TEST_F(WasmMozJSTest, MapReduceHighVolumeEmitHitsLimit) {
    initEngine();

    // Use a moderate byte limit (64 KB) and hammer it with many map invocations.
    ASSERT_TRUE(setupEmitWithLimit(64 * 1024));

    auto mapFn = createFunction(
        "function() {"
        "  emit(this._id, this.payload);"
        "}");
    ASSERT_NE(mapFn, 0u);

    int emitted = 0;
    for (; emitted < 10000; emitted++) {
        std::string payload(100, 'A' + (emitted % 26));
        if (!invokeMap(mapFn, BSON("_id" << emitted << "payload" << payload)))
            break;
    }
    ASSERT_GT(emitted, 0);
    ASSERT_LT(emitted, 10000);

    // The limit is still exceeded, so the next call also fails with the same error.
    auto err = invokeMapError(mapFn, BSON("_id" << 99999 << "payload" << "x"));
    ASSERT_EQ(err.code, "e-runtime");
    ASSERT_NE(err.msg.find("emit() exceeded memory limit"), std::string::npos);
}

TEST_F(WasmMozJSTest, MapReduceEmitLargeValues) {
    initEngine();
    ASSERT_TRUE(setupEmitWithLimit(32 * 1024));

    // Each emit produces a large value (~10 KB). A few calls should exceed 32 KB.
    auto mapFn = createFunction(
        "function() {"
        "  var big = new Array(10001).join('z');"
        "  emit(this.key, big);"
        "}");
    ASSERT_NE(mapFn, 0u);

    ASSERT_TRUE(invokeMap(mapFn, BSON("key" << 1)));
    ASSERT_TRUE(invokeMap(mapFn, BSON("key" << 2)));
    ASSERT_TRUE(invokeMap(mapFn, BSON("key" << 3)));

    auto err = invokeMapError(mapFn, BSON("key" << 4));
    ASSERT_EQ(err.code, "e-runtime");
    ASSERT_NE(err.msg.find("emit() exceeded memory limit"), std::string::npos);
}

TEST_F(WasmMozJSTest, JsHeapOOMFromFunctionAllocatingManyObjects) {
    initEngine();

    // Exponential string doubling inside a function (same pattern as
    // StringConcatenation but using a separate code path: invoke-function).
    auto handle = createFunction(
        "function() {"
        "  var s = new Array(1024 * 1024 + 1).join('z');"
        "  for (var i = 0; i < 10; i++) {"
        "    s = s + s;"
        "  }"
        "  return s.length;"
        "}");
    ASSERT_NE(handle, 0u);

    auto err = invokeFunctionError(handle, BSONObj());
    ASSERT_EQ(err.code, "e-runtime");
    bool hasOomMsg = err.msg.find("out of memory") != std::string::npos ||
        err.msg.find("allocation size overflow") != std::string::npos;
    ASSERT_TRUE(hasOomMsg) << "msg: " << err.msg;
}

TEST_F(WasmMozJSTest, MapReduceOomRecoveryAfterDrain) {
    initEngine();

    // Use a small limit so we can observe recovery after drain.
    ASSERT_TRUE(setupEmitWithLimit(512));

    auto mapFn = createFunction(
        "function() {"
        "  emit(this.key, this.value);"
        "}");
    ASSERT_NE(mapFn, 0u);

    // Emit until limit is hit.
    int emitted = 0;
    while (invokeMap(mapFn, BSON("key" << emitted << "value" << "data"))) {
        emitted++;
        if (emitted > 100)
            break;
    }
    ASSERT_LT(emitted, 100);
    ASSERT_GT(emitted, 0);

    // Verify the failure is an emit-limit error.
    auto err = invokeMapError(mapFn, BSON("key" << 999 << "value" << "x"));
    ASSERT_EQ(err.code, "e-runtime");
    ASSERT_NE(err.msg.find("emit() exceeded memory limit"), std::string::npos);

    // Drain and re-setup resets the counter, allowing more emits.
    drainEmitBuffer();
    ASSERT_TRUE(setupEmitWithLimit(512));

    int emitted2 = 0;
    while (invokeMap(mapFn, BSON("key" << emitted2 << "value" << "data"))) {
        emitted2++;
        if (emitted2 > 100)
            break;
    }
    ASSERT_LT(emitted2, 100);
    ASSERT_GT(emitted2, 0);
    ASSERT_EQ(emitted, emitted2);
}

}  // namespace
}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo
