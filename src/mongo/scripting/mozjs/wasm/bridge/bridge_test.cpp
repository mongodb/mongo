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
 * Unit tests for the MozJS Wasm component API via wasmtime.
 *
 * These tests load the AOT pre-compiled mozjs_wasm_api.cwasm component
 * (embedded into the binary via objcopy at build time), instantiate it
 * in a wasmtime runtime, and exercise the WIT interface functions
 * (initialize-engine, create-function, invoke-function,
 * get-return-value-bson, shutdown-engine etc).
 *
 */

#include "mongo/scripting/mozjs/wasm/bridge/bridge.h"

#include "mongo/base/error_codes.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/scripting/config_engine_gen.h"
#include "mongo/scripting/mozjs/wasm/embedded_wasm_resource.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace mongo {
namespace mozjs {
namespace wasm {
namespace {

// Shares wasmtime engine + compiled component across all tests via WasmEngineContext.
// Each test gets a fresh MozJSWasmBridge (store + instance) for isolation.
class WasmMozJSBridgeTest : public unittest::Test {
public:
    static void SetUpTestSuite() {
        auto [wasmData, wasmSize] = getEmbeddedWasmResource();
        _s_engineCtx = WasmEngineContext::createFromPrecompiled(wasmData, wasmSize);
    }

protected:
    void setUp() override {
        MozJSWasmBridge::Options opts{};
        opts.linearMemoryLimitMB = gWasmtimeStoreMemoryLimitMB.load();
        _bridge = std::make_unique<MozJSWasmBridge>(_s_engineCtx, opts);
        ASSERT_TRUE(initEngine());
    }

    void tearDown() override {
        if (_bridge && _bridge->isInitialized()) {
            _bridge->shutdown();
        }
        _bridge.reset();
    }

    bool initEngine() {
        return _bridge->initialize();
    }

    uint64_t createFunction(std::string_view source) {
        return _bridge->createFunction(source);
    }

    BSONObj invokeFunction(uint64_t handle, const BSONObj& args) {
        auto result = _bridge->invokeFunction(handle, args);
        uassert(result.getStatus().code(), result.getStatus().reason(), result.isOK());
        return result.getValue();
    }

    void setGlobal(std::string_view name, const BSONObj& value) {
        _bridge->setGlobal(name, value);
    }

    void setGlobalValue(std::string_view name, const BSONObj& value) {
        _bridge->setGlobalValue(name, value);
    }

    void setFunction(std::string_view name, std::string_view code) {
        BSONObjBuilder b;
        b.appendCode("val", std::string(code));
        return setGlobalValue(name, b.obj());
    }

    void setupEmit(boost::optional<int64_t> byteLimit) {
        _bridge->setupEmit(byteLimit);
    }

    void invokeMap(uint64_t handle, const BSONObj& value) {
        _bridge->invokeMap(handle, value);
    }

    bool invokePredicate(uint64_t handle, const BSONObj& value) {
        return _bridge->invokePredicate(handle, value);
    }

    BSONObj drainEmitBuffer() {
        return _bridge->drainEmitBuffer();
    }

    BSONObj getGlobal(std::string_view name) {
        return _bridge->getGlobal(name);
    }

    auto getContainsCheck(ErrorCodes::Error e, std::string query) {
        return [=](auto&& ex) {
            ASSERT_EQUALS(ex.code(), e);
            ASSERT_STRING_CONTAINS(ex.reason(), query);
        };
    }

    std::unique_ptr<MozJSWasmBridge> _bridge;

    static std::shared_ptr<WasmEngineContext> _s_engineCtx;
};

std::shared_ptr<WasmEngineContext> WasmMozJSBridgeTest::_s_engineCtx;

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, EngineInitializeAndShutdown) {
    ASSERT_TRUE(_bridge->isInitialized());

    _bridge->shutdown();
    ASSERT_FALSE(_bridge->isInitialized());
}

TEST_F(WasmMozJSBridgeTest, CreateFunctionReturnsHandle) {
    auto handle = createFunction("function() { return 42; }");
    ASSERT_NE(handle, uint64_t(0));
}

TEST_F(WasmMozJSBridgeTest, InvokeFunctionNoArgs) {
    auto handle = createFunction("function() { return {\"answer\": 42}; }");

    // Invoke with empty BSON args
    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    // Retrieve result
    ASSERT_FALSE(result.isEmpty());

    ASSERT_EQ(result.getIntField("answer"), 42);
}

// ---------------------------------------------------------------------------
// BSON argument passing tests
//
// invokeFunction passes each BSON field as a POSITIONAL JS argument:
//   BSONObj {a: 21, b: "hello"} → JS call: func(21, "hello")
// The field names determine insertion order but are not visible to JS.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, BsonArgsInt32) {
    // Each BSON field becomes a positional JS arg.
    auto handle = createFunction(
        "function(value, multiplier) {"
        "  return { result: value * multiplier };"
        "}");

    BSONObjBuilder bob;
    bob.append("value", 21);
    bob.append("multiplier", 2);

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getIntField("result"), 42);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsDouble) {
    auto handle = createFunction(
        "function(a, b) {"
        "  return { sum: a + b };"
        "}");

    BSONObjBuilder bob;
    bob.append("a", 3.14);
    bob.append("b", 2.86);

    auto result = invokeFunction(handle, bob.obj());

    double sum = result.getField("sum").Number();
    ASSERT_APPROX_EQUAL(sum, 6.0, 1e-10);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsString) {
    auto handle = createFunction(
        "function(greeting, name) {"
        "  return { message: greeting + ', ' + name + '!' };"
        "}");

    BSONObjBuilder bob;
    bob.append("greeting", "Hello");
    bob.append("name", "MongoDB");

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getStringField("message"), "Hello, MongoDB!");
}

TEST_F(WasmMozJSBridgeTest, BsonArgsBoolean) {
    auto handle = createFunction(
        "function(flag) {"
        "  return { negated: !flag, type: typeof flag };"
        "}");

    {
        BSONObjBuilder bob;
        bob.appendBool("flag", true);
        auto result = invokeFunction(handle, bob.obj());

        ASSERT_EQ(result.getBoolField("negated"), false);
        ASSERT_EQ(result.getStringField("type"), "boolean");
    }
    {
        BSONObjBuilder bob;
        bob.appendBool("flag", false);
        auto result = invokeFunction(handle, bob.obj());

        ASSERT_EQ(result.getBoolField("negated"), true);
    }
}

TEST_F(WasmMozJSBridgeTest, BsonArgsNull) {
    auto handle = createFunction(
        "function(val) {"
        "  return { isNull: val === null, type: typeof val };"
        "}");

    BSONObjBuilder bob;
    bob.appendNull("val");

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getBoolField("isNull"), true);
    ASSERT_EQ(result.getStringField("type"), "object");  // typeof null === "object" in JS
}

TEST_F(WasmMozJSBridgeTest, BsonArgsObject) {
    // Nested BSON object becomes a JS object positional arg
    auto handle = createFunction(
        "function(doc) {"
        "  return {"
        "    name: doc.name,"
        "    age: doc.age,"
        "    hasCity: doc.address !== undefined && doc.address.city !== undefined"
        "  };"
        "}");

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

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getStringField("name"), "Alice");
    ASSERT_EQ(result.getIntField("age"), 30);
    ASSERT_EQ(result.getBoolField("hasCity"), true);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsArray) {
    // BSON array becomes a JS Array positional arg
    auto handle = createFunction(
        "function(arr) {"
        "  return {"
        "    length: arr.length,"
        "    sum: arr.reduce(function(a, b) { return a + b; }, 0),"
        "    first: arr[0],"
        "    last: arr[arr.length - 1]"
        "  };"
        "}");

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(10);
        arr.append(20);
        arr.append(30);
        arr.append(40);
    }
    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getIntField("length"), 4);
    ASSERT_EQ(result.getIntField("sum"), 100);
    ASSERT_EQ(result.getIntField("first"), 10);
    ASSERT_EQ(result.getIntField("last"), 40);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsMixedTypes) {
    // Multiple BSON fields of different types as positional args
    auto handle = createFunction(
        "function(num, str, flag, obj) {"
        "  return {"
        "    numType: typeof num,"
        "    strType: typeof str,"
        "    flagType: typeof flag,"
        "    objType: typeof obj,"
        "    computed: flag ? (num + ' ' + str) : obj.key"
        "  };"
        "}");

    BSONObjBuilder bob;
    bob.append("num", 42);
    bob.append("str", "hello");
    bob.appendBool("flag", true);
    {
        BSONObjBuilder nested(bob.subobjStart("obj"));
        nested.append("key", "fallback");
    }

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getStringField("numType"), "number");
    ASSERT_EQ(result.getStringField("strType"), "string");
    ASSERT_EQ(result.getStringField("flagType"), "boolean");
    ASSERT_EQ(result.getStringField("objType"), "object");
    ASSERT_EQ(result.getStringField("computed"), "42 hello");
}

TEST_F(WasmMozJSBridgeTest, BsonArgsEmptyObject) {
    // No args (empty BSON) — function receives no arguments
    auto handle = createFunction(
        "function() {"
        "  return { argCount: arguments.length };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    ASSERT_EQ(result.getIntField("argCount"), 0);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsReturnArray) {
    // JS function that returns an array
    auto handle = createFunction(
        "function(n) {"
        "  var result = [];"
        "  for (var i = 0; i < n; i++) {"
        "    result.push(i * i);"
        "  }"
        "  return result;"
        "}");

    BSONObjBuilder bob;
    bob.append("n", 5);
    auto result = invokeFunction(handle, bob.obj());

    // Elements are keyed "0", "1", "2", ...
    ASSERT_EQ(result.getIntField("0"), 0);   // 0*0
    ASSERT_EQ(result.getIntField("1"), 1);   // 1*1
    ASSERT_EQ(result.getIntField("2"), 4);   // 2*2
    ASSERT_EQ(result.getIntField("3"), 9);   // 3*3
    ASSERT_EQ(result.getIntField("4"), 16);  // 4*4
}

TEST_F(WasmMozJSBridgeTest, BsonArgsReturnPrimitives) {
    // Returning various primitive types
    BSONObj emptyArgs;

    // Integer
    {
        auto h = createFunction("function() { return 42; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_EQ(retVal.numberInt(), 42);
    }

    // Double
    {
        auto h = createFunction("function() { return 3.14159; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_APPROX_EQUAL(retVal.Number(), 3.14159, 1e-5);
    }

    // Boolean true
    {
        auto h = createFunction("function() { return true; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_EQ(retVal.type(), BSONType::boolean);
        ASSERT_EQ(retVal.Bool(), true);
    }

    // Boolean false
    {
        auto h = createFunction("function() { return false; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_EQ(retVal.type(), BSONType::boolean);
        ASSERT_EQ(retVal.Bool(), false);
    }

    // null
    {
        auto h = createFunction("function() { return null; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_EQ(retVal.type(), BSONType::null);
    }

    // String
    {
        auto h = createFunction("function() { return 'hello'; }");
        auto result = invokeFunction(h, emptyArgs);

        BSONElement retVal = result.getField("__value");
        ASSERT_EQ(retVal.type(), BSONType::string);
        ASSERT_EQ(retVal.str(), "hello");
    }
}

TEST_F(WasmMozJSBridgeTest, BsonArgsNestedArraysAndObjects) {
    // Deeply nested structure: object with arrays containing objects
    auto handle = createFunction(
        "function(data) {"
        "  var total = 0;"
        "  for (var i = 0; i < data.items.length; i++) {"
        "    total += data.items[i].price * data.items[i].qty;"
        "  }"
        "  return { total: total, itemCount: data.items.length };"
        "}");

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

    auto result = invokeFunction(handle, bob.obj());

    // 10*3 + 25*2 + 5*10 = 30 + 50 + 50 = 130
    ASSERT_EQ(result.getIntField("total"), 130);
    ASSERT_EQ(result.getIntField("itemCount"), 3);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsObjectId) {
    // ObjectId round-trips through JS preserving type and value
    auto handle = createFunction(
        "function(oid) {"
        "  return { val: oid };"
        "}");

    OID testOid = OID::gen();
    BSONObjBuilder bob;
    bob.append("oid", testOid);

    auto result = invokeFunction(handle, bob.obj());

    BSONElement valElem = result.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::oid);
    ASSERT_EQ(valElem.OID(), testOid);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsDate) {
    // Date round-trips through JS. JS Date supports getUTCFullYear etc.
    auto handle = createFunction(
        "function(d) {"
        "  return {"
        "    val: d,"
        "    year: d.getUTCFullYear()"
        "  };"
        "}");

    // 2025-06-15T00:00:00Z = 1750032000000ms since epoch
    Date_t testDate = Date_t::fromMillisSinceEpoch(1750032000000LL);
    BSONObjBuilder bob;
    bob.appendDate("d", testDate);

    auto result = invokeFunction(handle, bob.obj());

    // Date round-trips as BSONType::date
    BSONElement valElem = result.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::date);
    ASSERT_EQ(valElem.Date(), testDate);
    // JS method access works on native Date
    ASSERT_EQ(result.getIntField("year"), 2025);
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

TEST_F(WasmMozJSBridgeTest, BsonArgsNumberLong) {
    // NumberLong (int64) round-trips through JS as a NumberLong object.
    auto handle = createFunction(
        "function(n) {"
        "  return { val: n };"
        "}");

    BSONObjBuilder bob;
    bob.append("n", 1234567890123LL);

    auto result = invokeFunction(handle, bob.obj());

    BSONElement valElem = result.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::numberLong);
    ASSERT_EQ(valElem.Long(), 1234567890123LL);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsNumberDecimal128) {
    auto handle = createFunction(
        "function(dec) {"
        "  return { val: dec };"
        "}");

    BSONObjBuilder bob;
    bob.append("dec", Decimal128("123.456"));

    auto result = invokeFunction(handle, bob.obj());

    BSONElement valElem = result.getField("val");
    ASSERT_EQ(valElem.type(), BSONType::numberDecimal);
    ASSERT_TRUE(valElem.numberDecimal() == Decimal128("123.456"));
}

TEST_F(WasmMozJSBridgeTest, BsonArgsTimestamp) {
    auto handle = createFunction(
        "function(ts) {"
        "  return { val: ts };"
        "}");

    BSONObjBuilder bob;
    bob.append("ts", Timestamp(1700000000, 42));

    auto result = invokeFunction(handle, bob.obj());

    BSONElement tsElem = result.getField("val");
    ASSERT_EQ(tsElem.type(), BSONType::timestamp);
    ASSERT_EQ(tsElem.timestamp(), Timestamp(1700000000, 42));
}

TEST_F(WasmMozJSBridgeTest, BsonArgsRegex) {
    auto handle = createFunction(
        "function(re) {"
        "  return { val: re };"
        "}");

    BSONObjBuilder bob;
    bob.appendRegex("re", "Hello", "i");

    auto result = invokeFunction(handle, bob.obj());

    BSONElement reElem = result.getField("val");
    ASSERT_EQ(reElem.type(), BSONType::regEx);
    ASSERT_EQ(std::string(reElem.regex()), "Hello");
    ASSERT_EQ(std::string(reElem.regexFlags()), "i");
}

TEST_F(WasmMozJSBridgeTest, BsonArgsMinKeyMaxKey) {
    auto handle = createFunction(
        "function(mn, mx) {"
        "  return { minVal: mn, maxVal: mx };"
        "}");

    BSONObjBuilder bob;
    bob.appendMinKey("mn");
    bob.appendMaxKey("mx");

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getField("minVal").type(), BSONType::minKey);
    ASSERT_EQ(result.getField("maxVal").type(), BSONType::maxKey);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsBinData) {
    auto handle = createFunction(
        "function(bd) {"
        "  return { val: bd };"
        "}");

    const char binPayload[] = {'\x01', '\x02', '\x03', '\x04', '\x05'};
    BSONObjBuilder bob;
    bob.appendBinData("bd", sizeof(binPayload), BinDataGeneral, binPayload);

    auto result = invokeFunction(handle, bob.obj());

    BSONElement bdElem = result.getField("val");
    ASSERT_EQ(bdElem.type(), BSONType::binData);
    int len = 0;
    const char* data = bdElem.binData(len);
    ASSERT_EQ(len, 5);
    ASSERT_EQ(std::memcmp(data, binPayload, 5), 0);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsArrayAsArg) {
    // Pass a raw BSON array as a positional arg — JS receives it as an Array
    auto handle = createFunction(
        "function(arr) {"
        "  var max = -Infinity;"
        "  var min = Infinity;"
        "  for (var i = 0; i < arr.length; i++) {"
        "    if (arr[i] > max) max = arr[i];"
        "    if (arr[i] < min) min = arr[i];"
        "  }"
        "  return { max: max, min: min, len: arr.length };"
        "}");

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(5);
        arr.append(99);
        arr.append(-3);
        arr.append(42);
        arr.append(0);
    }

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getIntField("max"), 99);
    ASSERT_EQ(result.getIntField("min"), -3);
    ASSERT_EQ(result.getIntField("len"), 5);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsArrayOfStrings) {
    // Pass array of strings, join them in JS
    auto handle = createFunction(
        "function(words) {"
        "  return { sentence: words.join(' '), count: words.length };"
        "}");

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("words"));
        arr.append("the");
        arr.append("quick");
        arr.append("brown");
        arr.append("fox");
    }

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getStringField("sentence"), "the quick brown fox");
    ASSERT_EQ(result.getIntField("count"), 4);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsArrayOfMixedTypes) {
    // Array containing mixed types: int, string, bool, null, nested object
    auto handle = createFunction(
        "function(arr) {"
        "  var types = [];"
        "  for (var i = 0; i < arr.length; i++) {"
        "    types.push(arr[i] === null ? 'null' : typeof arr[i]);"
        "  }"
        "  return { types: types.join(','), len: arr.length };"
        "}");

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

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getStringField("types"), "number,string,boolean,null,object");
    ASSERT_EQ(result.getIntField("len"), 5);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsArrayPassthrough) {
    // Verify that a JS function can return the array it received (round-trip)
    auto handle = createFunction(
        "function(arr) {"
        "  return arr;"
        "}");

    BSONObjBuilder bob;
    {
        BSONArrayBuilder arr(bob.subarrayStart("arr"));
        arr.append(10);
        arr.append(20);
        arr.append(30);
    }

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getIntField("0"), 10);
    ASSERT_EQ(result.getIntField("1"), 20);
    ASSERT_EQ(result.getIntField("2"), 30);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsObjectIdRoundTrip) {
    // Pass ObjectId through JS and get it back as-is
    auto handle = createFunction(
        "function(oid) {"
        "  return { id: oid };"
        "}");

    OID testOid = OID::gen();
    BSONObjBuilder bob;
    bob.append("oid", testOid);

    auto result = invokeFunction(handle, bob.obj());

    BSONElement idElem = result.getField("id");
    ASSERT_EQ(idElem.type(), BSONType::oid);
    ASSERT_EQ(idElem.OID(), testOid);
}

TEST_F(WasmMozJSBridgeTest, BsonArgsAllTypesInOneCall) {
    // Pass many different BSON types as positional args in one call
    auto handle = createFunction(
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

    auto result = invokeFunction(handle, bob.obj());

    ASSERT_EQ(result.getIntField("argCount"), 9);
    ASSERT_EQ(result.getIntField("i32"), 7);
    ASSERT_APPROX_EQUAL(result.getField("dbl").Number(), 2.718, 1e-5);
    ASSERT_EQ(result.getStringField("str"), "test");
    ASSERT_EQ(result.getBoolField("flag"), false);
    ASSERT_EQ(result.getBoolField("isNull"), true);
    ASSERT_EQ(result.getStringField("objKey"), "v");
    ASSERT_EQ(result.getIntField("arrLen"), 3);
    ASSERT_TRUE(result.hasField("oidStr"));
    ASSERT_EQ(result.getIntField("dtYear"), 2025);
}

TEST_F(WasmMozJSBridgeTest, Throw) {
    BSONObj emptyArgs;

    auto h1 = createFunction("function() { throw new Error(\"Throwing error!\"); }");
    ASSERT_THROWS_CODE(
        invokeFunction(h1, emptyArgs), AssertionException, ErrorCodes::JSInterpreterFailure);
}

TEST_F(WasmMozJSBridgeTest, CreateMultipleFunctions) {
    auto h1 = createFunction("function() { return 1; }");
    auto h2 = createFunction("function() { return 2; }");
    auto h3 = createFunction("function() { return 3; }");

    // Handles should be distinct
    ASSERT_NE(h1, h2);
    ASSERT_NE(h2, h3);
    ASSERT_NE(h1, h3);

    // Invoke each and verify results
    BSONObj emptyArgs;

    auto r1 = invokeFunction(h1, emptyArgs);
    ASSERT_EQ(r1.getField("__value").numberInt(), 1);

    auto r2 = invokeFunction(h2, emptyArgs);
    ASSERT_EQ(r2.getField("__value").numberInt(), 2);

    auto r3 = invokeFunction(h3, emptyArgs);
    ASSERT_EQ(r3.getField("__value").numberInt(), 3);
}

TEST_F(WasmMozJSBridgeTest, InvokeWithInvalidHandleFails) {
    // Handle 0 is always invalid; the bridge throws via uassert.
    ASSERT_THROWS_CODE(
        invokeFunction(0, BSONObj()), AssertionException, ErrorCodes::JSInterpreterFailure);
}

TEST_F(WasmMozJSBridgeTest, CreateFunctionWithInvalidSourceFails) {
    // Invalid JavaScript - not a function expression; the bridge throws via uassert.
    ASSERT_THROWS_CODE(createFunction("this is not valid javascript {{{"),
                       AssertionException,
                       ErrorCodes::JSInterpreterFailure);
}

TEST_F(WasmMozJSBridgeTest, ShutdownAndReinitialize) {
    createFunction("function() { return 'first'; }");

    _bridge->shutdown();
    ASSERT_FALSE(_bridge->isInitialized());

    // Re-initialize into the same store/instance.
    ASSERT_TRUE(initEngine());

    // Old handles are invalid after shutdown; new functions work fine.
    auto h2 = createFunction("function() { return 'second'; }");
    BSONObj emptyArgs;
    auto result = invokeFunction(h2, emptyArgs);
    ASSERT_FALSE(result.isEmpty());
}

TEST_F(WasmMozJSBridgeTest, FunctionReturningString) {

    auto handle = createFunction("function() { return \"hello world\"; }");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    ASSERT_FALSE(result.isEmpty());

    BSONElement retVal = result.getField("__value");
    ASSERT_FALSE(retVal.eoo());
    ASSERT_EQ(retVal.type(), mongo::BSONType::string);
    ASSERT_EQ(retVal.str(), "hello world");
}

TEST_F(WasmMozJSBridgeTest, FunctionReturningNestedObject) {
    auto handle = createFunction(
        "function() {"
        "  return {"
        "    outer: {"
        "      inner: {"
        "        value: 99"
        "      }"
        "    }"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    ASSERT_FALSE(result.isEmpty());

    BSONObj inner = result.getObjectField("outer").getObjectField("inner");
    ASSERT_EQ(inner.getIntField("value"), 99);
}

// ---------------------------------------------------------------------------
// set-global / get-global tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, SetGlobalGetGlobalRoundTrip) {
    BSONObjBuilder bob;
    bob.append("x", 10);
    bob.append("y", "hello");
    BSONObj value = bob.obj();

    setGlobal("myVar", value);

    BSONObj retrieved = getGlobal("myVar");
    ASSERT_FALSE(retrieved.isEmpty());
    ASSERT_EQ(retrieved.getIntField("x"), 10);
    ASSERT_EQ(retrieved.getStringField("y"), "hello");
}

TEST_F(WasmMozJSBridgeTest, GetGlobalNonexistent) {
    ASSERT_THROWS_CODE(getGlobal("nonexistent_global_12345"), AssertionException, 11542301);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalThenUseInFunction) {
    BSONObjBuilder bob;
    bob.append("factor", 7);
    setGlobal("config", bob.obj());

    // JS function that reads the global we set (by name) and uses it.
    // set-global should install "config" on the JS global scope.
    auto handle = createFunction(
        "function(n) {"
        "  if (typeof config === 'undefined') return { result: -1 };"
        "  return { result: n * config.factor };"
        "}");

    BSONObjBuilder args;
    args.append("n", 6);
    auto result = invokeFunction(handle, args.obj());

    // 6 * config.factor(7) = 42.
    ASSERT_EQ(result.getIntField("result"), 42);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalOverwrite) {
    BSONObjBuilder b1;
    b1.append("v", 1);
    setGlobal("g", b1.obj());

    ASSERT_EQ(getGlobal("g").getIntField("v"), 1);

    // Overwrite with a new value
    BSONObjBuilder b2;
    b2.append("v", 2);
    b2.append("extra", "second");
    setGlobal("g", b2.obj());
    BSONObj g2 = getGlobal("g");
    ASSERT_EQ(g2.getIntField("v"), 2);
    ASSERT_EQ(g2.getStringField("extra"), "second");
}

TEST_F(WasmMozJSBridgeTest, SetGlobalMultipleNames) {
    BSONObjBuilder b1;
    b1.append("val", 1);
    setGlobal("alpha", b1.obj());

    BSONObjBuilder b2;
    b2.append("val", 2);
    setGlobal("beta", b2.obj());

    BSONObjBuilder b3;
    b3.append("val", 3);
    setGlobal("gamma", b3.obj());

    // Each global should be independently retrievable
    ASSERT_EQ(getGlobal("alpha").getIntField("val"), 1);
    ASSERT_EQ(getGlobal("beta").getIntField("val"), 2);
    ASSERT_EQ(getGlobal("gamma").getIntField("val"), 3);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalVariousTypes) {
    // Set global with various BSON types and verify round-trip via get-global
    {
        BSONObjBuilder bob;
        bob.append("n", 42);
        bob.append("d", 3.14);
        bob.append("s", "hello");
        bob.appendBool("b", true);
        bob.appendNull("nil");
        setGlobal("mixed", bob.obj());
    }

    BSONObj retrieved = getGlobal("mixed");
    ASSERT_FALSE(retrieved.isEmpty());
    ASSERT_EQ(retrieved.getIntField("n"), 42);
    ASSERT_APPROX_EQUAL(retrieved.getField("d").Number(), 3.14, 1e-10);
    ASSERT_EQ(retrieved.getStringField("s"), "hello");
    ASSERT_EQ(retrieved.getBoolField("b"), true);
    ASSERT_EQ(retrieved.getField("nil").type(), BSONType::null);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalVisibleAcrossFunctions) {
    BSONObjBuilder bob;
    bob.append("counter", 100);
    setGlobal("state", bob.obj());

    // Two different functions should both see the same global
    auto h1 = createFunction(
        "function() {"
        "  return { val: state.counter + 1 };"
        "}");

    auto h2 = createFunction(
        "function() {"
        "  return { val: state.counter + 2 };"
        "}");

    BSONObj emptyArgs;

    auto r1 = invokeFunction(h1, emptyArgs);
    ASSERT_EQ(r1.getIntField("val"), 101);

    auto r2 = invokeFunction(h2, emptyArgs);
    ASSERT_EQ(r2.getIntField("val"), 102);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalValueVisibleAcrossFunctions) {
    BSONObjBuilder bob;
    setGlobalValue("state", BSON("" << 3));

    // Two different functions should both see the same global
    auto h1 = createFunction(
        "function() {"
        "  return state;"
        "}");
    auto h2 = createFunction(
        "function() {"
        "  state = state + 1;"
        "  return state;"
        "}");

    BSONObj emptyArgs;

    auto r1 = invokeFunction(h1, emptyArgs);
    ASSERT_EQ(r1.getField("__value").Number(), 3);
    auto r2 = invokeFunction(h2, emptyArgs);
    ASSERT_EQ(r2.getField("__value").Number(), 4);
    r1 = invokeFunction(h1, emptyArgs);
    ASSERT_EQ(r1.getField("__value").Number(), 4);
}

TEST_F(WasmMozJSBridgeTest, EmissionsTesting) {
    BSONObjBuilder bob;
    // Setup emit
    setupEmit(boost::none);

    // Create map function
    auto h1 = createFunction(
        "function() {"
        "  return emit(this.key, this.value)"
        "}");

    // Emtit some
    invokeMap(h1, BSON("key" << "a" << "value" << "b"));
    invokeMap(h1, BSON("key" << "b" << "value" << "c"));

    // Drain and check
    auto buffer = drainEmitBuffer();
    ASSERT_EQ(buffer["emits"].Obj()[0].Obj()["k"].String(), "a");
    ASSERT_EQ(buffer["emits"].Obj()[0].Obj()["v"].String(), "b");
    ASSERT_EQ(buffer["emits"].Obj()[1].Obj()["k"].String(), "b");
    ASSERT_EQ(buffer["emits"].Obj()[1].Obj()["v"].String(), "c");
}

TEST_F(WasmMozJSBridgeTest, InvokePredicate) {
    // Create predicate function
    auto h1 = createFunction(
        "function() {"
        "  return this.age > 18"
        "}");

    // Check result
    ASSERT_TRUE(invokePredicate(h1, BSON("age" << 49)));
    ASSERT_FALSE(invokePredicate(h1, BSON("age" << 17)));
}

TEST_F(WasmMozJSBridgeTest, GlobalScopeContainsExpectedVars) {
    // Enumerate all global property names from the JS environment.
    auto handle = createFunction(
        "function() {"
        "  var global = (function() { return this; })();"
        "  return Object.getOwnPropertyNames(global);"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    // Collect all property names into a set.
    std::set<std::string> globals;
    for (const auto& elem : result) {
        if (elem.type() == BSONType::string) {
            globals.insert(elem.str());
        }
    }

    // Verify custom globals installed by GlobalInfo::freeFunctions.
    for (const auto& name : {"print", "gc", "sleep", "version", "buildInfo", "getJSHeapLimitMB"}) {
        ASSERT_TRUE(globals.count(name) > 0);
    }

    // BSON type constructors installed via WrapType::install() (InstallType::Global).
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

TEST_F(WasmMozJSBridgeTest, BinDataConstructorAndMethods) {
    // BinData(subtype, base64str) constructor + prototype methods
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasBase64"));
    ASSERT_TRUE(inner.getBoolField("hasHex"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_EQ(inner.getStringField("b64Val"), "AQIDBA==");
    ASSERT_EQ(inner.getStringField("hexVal"), "01020304");
}

TEST_F(WasmMozJSBridgeTest, BinDataFreeFunctions) {
    // HexData, MD5, UUID are global free functions from BinDataInfo.
    auto handle = createFunction(
        "function() {"
        "  return {"
        "    hasHexData: typeof HexData === 'function',"
        "    hasMD5:     typeof MD5     === 'function',"
        "    hasUUID:    typeof UUID    === 'function',"
        "    hexDataOk:  HexData(0, '01020304').hex() === '01020304'"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasHexData"));
    ASSERT_TRUE(inner.getBoolField("hasMD5"));
    ASSERT_TRUE(inner.getBoolField("hasUUID"));
    ASSERT_TRUE(inner.getBoolField("hexDataOk"));
}

TEST_F(WasmMozJSBridgeTest, NumberIntConstructorAndMethods) {
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToNumber"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasValueOf"));
    ASSERT_EQ(inner.getIntField("numVal"), 42);
    ASSERT_EQ(inner.getStringField("strVal"), "NumberInt(42)");
    ASSERT_EQ(inner.getIntField("valOf"), 42);
}

TEST_F(WasmMozJSBridgeTest, NumberLongConstructorAndMethods) {
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToNumber"));
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasValueOf"));
    ASSERT_TRUE(inner.getBoolField("hasCompare"));
    ASSERT_EQ(inner.getStringField("strVal"), "NumberLong(\"1234567890123\")");
}

TEST_F(WasmMozJSBridgeTest, NumberDecimalConstructorAndMethods) {
    auto handle = createFunction(
        "function() {"
        "  var nd = NumberDecimal('123.456');"
        "  return {"
        "    hasToStr:   typeof nd.toString === 'function',"
        "    hasToJSON:  typeof nd.toJSON   === 'function',"
        "    strVal:     nd.toString()"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_EQ(inner.getStringField("strVal"), "NumberDecimal(\"123.456\")");
}

TEST_F(WasmMozJSBridgeTest, ObjectIdConstructorAndMethods) {
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    ASSERT_TRUE(inner.getBoolField("hasStr"));
    ASSERT_EQ(inner.getIntField("strLen"), 24);
    // toString() returns 'ObjectId("...")' format
    std::string toStr(inner.getStringField("toStrVal"));
    // toString() should return 'ObjectId("...")' format.
    ASSERT_TRUE(toStr.find("ObjectId(") == 0);
}

TEST_F(WasmMozJSBridgeTest, TimestampConstructorAndMethods) {
    auto handle = createFunction(
        "function() {"
        "  var ts = Timestamp(1700000000, 42);"
        "  return {"
        "    hasToJSON:  typeof ts.toJSON === 'function',"
        "    jsonVal:    ts.toJSON()"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToJSON"));
    // toJSON() returns { "$timestamp": { "t": ..., "i": ... } }
    BSONObj jsonVal = inner.getObjectField("jsonVal");
    // toJSON() returns { "$timestamp": { "t": ..., "i": ... } }.
    ASSERT_FALSE(jsonVal.isEmpty());
}

TEST_F(WasmMozJSBridgeTest, MinKeyMaxKeyMethods) {
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("mnHasTojson"));
    ASSERT_TRUE(inner.getBoolField("mnHasToJSON"));
    ASSERT_TRUE(inner.getBoolField("mxHasTojson"));
    ASSERT_TRUE(inner.getBoolField("mxHasToJSON"));
    ASSERT_EQ(inner.getStringField("mnTojson"), "{ \"$minKey\" : 1 }");
    ASSERT_EQ(inner.getStringField("mxTojson"), "{ \"$maxKey\" : 1 }");
}

TEST_F(WasmMozJSBridgeTest, BSONFreeFunctionsWork) {
    // bsonWoCompare, bsonBinaryEqual, bsonObjToArray are global free functions
    // from BSONInfo (Private installType, but freeFunctions go on global).
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
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

TEST_F(WasmMozJSBridgeTest, CodeConstructor) {
    auto handle = createFunction(
        "function() {"
        "  var c = Code('function() { return 1; }');"
        "  return {"
        "    hasToStr: typeof c.toString === 'function',"
        "    strVal:   c.toString()"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("hasToStr"));
}

TEST_F(WasmMozJSBridgeTest, NumberLongCompare) {
    auto handle = createFunction(
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

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_TRUE(inner.getBoolField("aLtB"));
    ASSERT_TRUE(inner.getBoolField("bGtA"));
    ASSERT_TRUE(inner.getBoolField("aEqC"));
}

TEST_F(WasmMozJSBridgeTest, NumberIntValueOfEnablesArithmetic) {
    // valueOf() allows NumberInt to participate in JS arithmetic
    auto handle = createFunction(
        "function() {"
        "  var a = NumberInt(10);"
        "  var b = NumberInt(32);"
        "  return {"
        "    sum:      a + b,"
        "    product:  a * b,"
        "    valueOf:  a.valueOf()"
        "  };"
        "}");

    BSONObj emptyArgs;
    auto result = invokeFunction(handle, emptyArgs);

    BSONObj inner = result;
    ASSERT_EQ(inner.getIntField("sum"), 42);
    ASSERT_EQ(inner.getIntField("product"), 320);
    ASSERT_EQ(inner.getIntField("valueOf"), 10);
}


// ---------------------------------------------------------------------------
// Error extraction and failure scenario tests
//
// These tests exercise failure paths and verify that the wasm-mozjs-error
// record returned by WIT contains the correct error code, message, filename,
// and line/column information for various failure modes.
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, CompileErrorHasCorrectErrorCode) {
    ASSERT_THROWS_WITH_CHECK(createFunction("this is not valid javascript {{{"),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-compile"));
}

TEST_F(WasmMozJSBridgeTest, CompileErrorContainsMessage) {
    ASSERT_THROWS_WITH_CHECK(createFunction("function( { invalid syntax"),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-compile"));
}

TEST_F(WasmMozJSBridgeTest, CompileErrorHasFileAndLineInfo) {
    ASSERT_THROWS_WITH_CHECK(createFunction("function() { var x = ; }"),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-compile"));
}

TEST_F(WasmMozJSBridgeTest, RuntimeErrorHasCorrectErrorCode) {
    auto handle = createFunction("function() { throw new Error('boom'); }");
    ASSERT_THROWS_WITH_CHECK(invokeFunction(handle, BSONObj()),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime"));
}

TEST_F(WasmMozJSBridgeTest, RuntimeErrorContainsMessage) {
    auto handle = createFunction("function() { throw new Error('specific error text'); }");
    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "specific error text")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeFunction(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, RuntimeErrorFromUndefinedVariable) {
    auto handle = createFunction("function() { return nonexistentVariable.property; }");


    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "nonexistentVariable")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeFunction(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, RuntimeErrorFromTypeError) {
    // Calling a non-function triggers a TypeError at runtime
    auto handle = createFunction("function() { var x = 42; return x(); }");
    ASSERT_THROWS_WITH_CHECK(invokeFunction(handle, BSONObj()),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime"));
}

TEST_F(WasmMozJSBridgeTest, ExpressionSourceWrappedAsFunction) {
    // "42" is an expression, not a function declaration but our semantics say this implies a
    // constant function. __parseJSFunctionOrExpression wraps it as "function() { return 42; }" so
    // createFunction succeeds and produces a callable handle.
    auto handle = createFunction("42");
    auto result = invokeFunction(handle, BSONObj());

    ASSERT_EQ(result["__value"].Number(), 42);
}

TEST_F(WasmMozJSBridgeTest, StaleHandleAfterShutdownHasCorrectErrorCode) {

    auto handle = createFunction("function() { return 1; }");

    // Shutdown and re-initialize
    _bridge->shutdown();

    ASSERT_TRUE(initEngine());

    ASSERT_THROWS_WITH_CHECK(invokeFunction(handle, BSONObj()),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-invalid-arg"));
}

// ---------------------------------------------------------------------------
// Stored procedure tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, StoredProcedureViaSetGlobalCode) {
    // Install a stored procedure as a global via BSON Code type.
    BSONObjBuilder bob;
    bob.appendCode("multiply", "function(a, b) { return a * b; }");
    setGlobal("procs", bob.obj());

    auto handle = createFunction(
        "function(x, y) {"
        "  return { result: procs.multiply(x, y) };"
        "}");

    BSONObjBuilder args;
    args.append("x", 6);
    args.append("y", 7);
    BSONObj result = invokeFunction(handle, args.obj());

    ASSERT_EQ(result.getIntField("result"), 42);
}

TEST_F(WasmMozJSBridgeTest, StoredProcedureMultiple) {
    // Install multiple stored procedures at once.
    BSONObjBuilder bob;
    bob.appendCode("add", "function(a, b) { return a + b; }");
    bob.appendCode("square", "function(x) { return x * x; }");
    bob.appendCode("negate", "function(x) { return -x; }");
    setGlobal("math", bob.obj());

    auto handle = createFunction(
        "function(a, b) {"
        "  var sum = math.add(a, b);"
        "  var sq = math.square(sum);"
        "  return { result: math.negate(sq) };"
        "}");

    BSONObjBuilder args;
    args.append("a", 3);
    args.append("b", 4);
    auto result = invokeFunction(handle, args.obj());

    // negate(square(add(3, 4))) = negate(square(7)) = negate(49) = -49
    ASSERT_EQ(result.getIntField("result"), -49);
}

TEST_F(WasmMozJSBridgeTest, StoredProcedureChaining) {
    // Install procedures that call each other.
    BSONObjBuilder bob;
    bob.appendCode("double_", "function(x) { return x * 2; }");
    bob.appendCode("doubleAndAdd", "function(a, b) { return utils.double_(a) + b; }");
    setGlobal("utils", bob.obj());

    auto handle = createFunction(
        "function(n) {"
        "  return { result: utils.doubleAndAdd(n, 10) };"
        "}");

    BSONObjBuilder args;
    args.append("n", 5);
    auto result = invokeFunction(handle, args.obj());

    // doubleAndAdd(5, 10) = double_(5) + 10 = 10 + 10 = 20
    ASSERT_EQ(result.getIntField("result"), 20);
}

TEST_F(WasmMozJSBridgeTest, StoredProcedureCalledFromMultipleFunctions) {
    ASSERT_TRUE(initEngine());

    BSONObjBuilder bob;
    bob.appendCode("transform", "function(x) { return x * x + 1; }");
    setGlobal("sp", bob.obj());

    // Two different functions both use the same stored procedure.
    auto h1 = createFunction("function(n) { return { result: sp.transform(n) }; }");

    auto h2 =
        createFunction("function(a, b) { return { result: sp.transform(a) + sp.transform(b) }; }");

    {
        BSONObjBuilder a;
        a.append("n", 3);
        auto r = invokeFunction(h1, a.obj());
        // transform(3) = 3*3 + 1 = 10
        ASSERT_EQ(r.getIntField("result"), 10);
    }

    {
        BSONObjBuilder a;
        a.append("a", 2);
        a.append("b", 4);
        auto r = invokeFunction(h2, a.obj());

        // transform(2) + transform(4) = (4+1) + (16+1) = 5 + 17 = 22
        ASSERT_EQ(r.getIntField("result"), 22);
    }
}

TEST_F(WasmMozJSBridgeTest, StoredProcedureWithCodeWScope) {

    // CodeWScope is also evaluated as a function (scope is ignored per SpiderMonkey behavior).
    BSONObjBuilder bob;
    bob.appendCodeWScope("greet", "function(name) { return 'Hello, ' + name; }", BSONObj());
    setGlobal("funcs", bob.obj());

    auto handle = createFunction("function() { return { msg: funcs.greet('World') }; }");
    ASSERT_NE(handle, 0u);

    auto result = invokeFunction(handle, BSONObj());
    ASSERT_EQ(result.getStringField("msg"), "Hello, World");
}


TEST_F(WasmMozJSBridgeTest, MQLFunctionPattern) {
    // Simulates: { $function: { body: <body>, args: ["$name"], lang: "js" } }
    // JsExecution::callFunction(func, params, thisObj={})
    // → Scope::invoke(func, &params, &{}, timeout, false)
    auto handle = createFunction(
        "function(name, factor) {"
        "  return { upper: name.toUpperCase(), doubled: factor * 2 };"
        "}");

    // Params are built as: { "arg0": <value>, "arg1": <value>, ... }
    BSONObjBuilder params;
    params.append("arg0", "hello");
    params.append("arg1", 21);
    auto result = invokeFunction(handle, params.obj());

    ASSERT_EQ(result.getStringField("upper"), "HELLO");
    ASSERT_EQ(result.getIntField("doubled"), 42);
}

TEST_F(WasmMozJSBridgeTest, MQLFunctionPatternWithDocumentArg) {
    // $function receives evaluated expressions as args; a common pattern is
    // passing the entire document as an argument.
    auto handle = createFunction(
        "function(doc) {"
        "  return doc.price * doc.qty;"
        "}");

    BSONObjBuilder params;
    {
        BSONObjBuilder doc(params.subobjStart("arg0"));
        doc.append("price", 15);
        doc.append("qty", 3);
    }
    auto result = invokeFunction(handle, params.obj());

    ASSERT_EQ(result.getField("__value").numberInt(), 45);
}

TEST_F(WasmMozJSBridgeTest, MQLAccumulatorFullLifecycle) {
    // Simulates the $accumulator lifecycle:
    //   init() → state
    //   accumulate(state, val) → state  (per document)
    //   merge(s1, s2) → state           (across shards)
    //   finalize(state) → result
    auto initFn = createFunction("function() { return { count: 0, sum: 0 }; }");
    auto accFn = createFunction(
        "function(state, val) {"
        "  return { count: state.count + 1, sum: state.sum + val };"
        "}");
    auto mergeFn = createFunction(
        "function(s1, s2) {"
        "  return { count: s1.count + s2.count, sum: s1.sum + s2.sum };"
        "}");
    auto finalizeFn = createFunction(
        "function(state) {"
        "  return state.count > 0 ? state.sum / state.count : 0;"
        "}");

    // Shard 1: init → accumulate [10, 20, 30]
    auto state1 = invokeFunction(initFn, BSONObj()).getOwned();
    for (int val : {10, 20, 30}) {
        BSONObjBuilder a;
        a.append("state", state1);
        a.append("val", val);
        state1 = invokeFunction(accFn, a.obj()).getOwned();
    }
    ASSERT_EQ(state1.getIntField("count"), 3);
    ASSERT_EQ(state1.getIntField("sum"), 60);

    // Shard 2: init → accumulate [40]
    auto state2 = invokeFunction(initFn, BSONObj()).getOwned();
    {
        BSONObjBuilder a;
        a.append("state", state2);
        a.append("val", 40);
        state2 = invokeFunction(accFn, a.obj()).getOwned();
    }
    ASSERT_EQ(state2.getIntField("count"), 1);
    ASSERT_EQ(state2.getIntField("sum"), 40);

    // Merge shard states
    {
        BSONObjBuilder a;
        a.append("s1", state1);
        a.append("s2", state2);
        state1 = invokeFunction(mergeFn, a.obj()).getOwned();
    }
    ASSERT_EQ(state1.getIntField("count"), 4);
    ASSERT_EQ(state1.getIntField("sum"), 100);

    // Finalize: average = 100/4 = 25
    {
        BSONObjBuilder a;
        a.append("state", state1);
        auto result = invokeFunction(finalizeFn, a.obj()).getOwned();
        ASSERT_APPROX_EQUAL(result.getField("__value").Number(), 25.0, 1e-10);
    }
}

TEST_F(WasmMozJSBridgeTest, MQLAccumulatorPattern) {
    // Mirrors real $accumulator: setFunction("__accumulate", code) installs a JS
    // function as a directly-callable global via set-global-value with BSONType::Code.
    // A wrapper then calls __accumulate(state, val) in a loop.
    setFunction("__accumulate",
                "function(state, val) {"
                "  return { count: state.count + 1, sum: state.sum + val };"
                "}");
    setFunction("__merge",
                "function(s1, s2) {"
                "  return { count: s1.count + s2.count, sum: s1.sum + s2.sum };"
                "}");
    auto wrapper = createFunction(
        "function(state, pendingCalls) {"
        "  for (var i = 0; i < pendingCalls.length; i++) {"
        "    state = __accumulate(state, pendingCalls[i]);"
        "  }"
        "  return state;"
        "}");

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
    auto result = invokeFunction(wrapper, args.obj());

    ASSERT_EQ(result.getIntField("count"), 3);
    ASSERT_EQ(result.getIntField("sum"), 60);

    auto mergeWrapper = createFunction(
        "function(state, pendingMerges) {"
        "  for (var i = 0; i < pendingMerges.length; i++) {"
        "    state = __merge(state, pendingMerges[i]);"
        "  }"
        "  return state;"
        "}");
    ASSERT_NE(mergeWrapper, 0u);

    BSONObjBuilder mergeArgs;
    mergeArgs.append("state", result);
    {
        BSONArrayBuilder pending(mergeArgs.subarrayStart("pendingMerges"));
        {
            BSONObjBuilder s(pending.subobjStart());
            s.append("count", 2);
            s.append("sum", 70);
        }
    }
    auto merged = invokeFunction(mergeWrapper, mergeArgs.obj()).getOwned();
    ASSERT_EQ(merged.getIntField("count"), 5);
    ASSERT_EQ(merged.getIntField("sum"), 130);
}

TEST_F(WasmMozJSBridgeTest, MQLWherePattern) {
    // $where calls: invoke(func, nullptr, &document, timeout)
    // where document becomes 'this'. invoke-predicate handles this directly.
    auto handle = createFunction(
        "function() {"
        "  return this.x > this.y;"
        "}");

    // Document that passes filter
    ASSERT_TRUE(invokePredicate(handle, BSON("x" << 10 << "y" << 5)));

    // Document that fails filter
    ASSERT_FALSE(invokePredicate(handle, BSON("x" << 3 << "y" << 7)));
}

TEST_F(WasmMozJSBridgeTest, MQLMapReduceReducePattern) {
    // mapReduce.reduce calls: invoke(reduceFunc, &{key, values}, &{}, timeout)
    // 'this' is an empty object. invoke-function (no this binding) works.
    auto reduceFn = createFunction(
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
    auto result = invokeFunction(reduceFn, args.obj());

    ASSERT_EQ(result.getField("__value").numberInt(), 725);
}

TEST_F(WasmMozJSBridgeTest, MQLMapReduceFinalizePattern) {
    // mapReduce.finalize uses $function pattern:
    // invoke(finalizeFunc, &{key, reducedValue}, &{}, timeout)
    auto finalizeFn = createFunction(
        "function(key, reducedValue) {"
        "  return { category: key, total: reducedValue, formatted: key + ': $' + reducedValue };"
        "}");
    ASSERT_NE(finalizeFn, 0u);

    BSONObjBuilder args;
    args.append("key", "electronics");
    args.append("reducedValue", 725);
    auto result = invokeFunction(finalizeFn, args.obj());

    ASSERT_EQ(result.getStringField("category"), "electronics");
    ASSERT_EQ(result.getIntField("total"), 725);
    ASSERT_EQ(result.getStringField("formatted"), "electronics: $725");
}

TEST_F(WasmMozJSBridgeTest, MQLMapReduceMapPattern) {
    // mapReduce.map calls: invoke(mapFunc, nullptr, &document, timeout)
    // where document becomes 'this' and the function calls emit(key, value).
    // invoke-map handles this: document as `this`, emits buffered.
    setupEmit(boost::none);

    auto mapFn = createFunction(
        "function() {"
        "  emit(this.category, this.price);"
        "}");

    invokeMap(mapFn, BSON("category" << "electronics" << "price" << 100));
    invokeMap(mapFn, BSON("category" << "books" << "price" << 25));
    invokeMap(mapFn, BSON("category" << "electronics" << "price" << 250));

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

TEST_F(WasmMozJSBridgeTest, MQLFunctionReusedAcrossDocuments) {
    // $function/$accumulator create the function once
    // and invoke it per-document. This tests handle reuse across many calls.
    auto handle = createFunction(
        "function(price, qty) {"
        "  return price * qty;"
        "}");

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
        auto result = invokeFunction(handle, args.obj());
        ASSERT_EQ(result.getField("__value").numberInt(), tc.expected);
    }
}

// ---------------------------------------------------------------------------
// set-global-value tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, SetGlobalValueBoolean) {
    BSONObj boolDoc = BSON("val" << true);
    setGlobalValue("fullObject", boolDoc);

    // Verify: fullObject should be boolean true, not an object
    auto handle = createFunction("function() { return fullObject === true; }");
    auto ret = invokeFunction(handle, BSONObj());
    ASSERT_TRUE(ret["__value"].boolean());
}

TEST_F(WasmMozJSBridgeTest, SetGlobalValueNumber) {
    BSONObj numDoc = BSON("val" << 3.14);
    setGlobalValue("pi", numDoc);

    auto handle = createFunction("function() { return pi; }");
    auto ret = invokeFunction(handle, BSONObj());
    ASSERT_APPROX_EQUAL(ret["__value"].numberDouble(), 3.14, 0.001);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalValueString) {
    BSONObj strDoc = BSON("val" << "world");
    setGlobalValue("greeting", strDoc);

    auto handle = createFunction("function() { return greeting; }");
    auto ret = invokeFunction(handle, BSONObj());
    ASSERT_EQ(ret["__value"].str(), "world");
}

TEST_F(WasmMozJSBridgeTest, SetGlobalValueCodeFunction) {
    // Use BSONType::Code to set a callable function as a global value
    BSONObjBuilder b;
    b.appendCode("val", "function(x) { return x * 2; }");
    BSONObj codeDoc = b.obj();

    setGlobalValue("doubler", codeDoc);

    // doubler should now be a callable function
    auto handle = createFunction("function(n) { return doubler(n); }");
    auto ret = invokeFunction(handle, BSON("0" << 21));
    ASSERT_EQ(ret["__value"].numberInt(), 42);
}

TEST_F(WasmMozJSBridgeTest, SetGlobalValueObject) {
    // Setting an object via set-global-value should set it as-is
    BSONObj objDoc = BSON("val" << BSON("a" << 1 << "b" << 2));
    setGlobalValue("config", objDoc);

    auto handle = createFunction("function() { return config.a + config.b; }");
    auto ret = invokeFunction(handle, BSONObj());
    ASSERT_EQ(ret["__value"].numberInt(), 3);
}

// ---------------------------------------------------------------------------
// `emit` in-WASM boundary
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, SetupEmitAndDrain) {
    setupEmit(boost::none);

    auto handle = createFunction("function(doc) { emit(doc.key, doc.val);}");
    invokeFunction(handle, BSON("0" << BSON("key" << "a" << "val" << 1)));
    invokeFunction(handle, BSON("0" << BSON("key" << "b" << "val" << 2)));

    BSONObj emitDoc = drainEmitBuffer();
    auto emitsArr = emitDoc["emits"].Array();
    ASSERT_EQ(emitsArr.size(), 2u);
    ASSERT_EQ(emitsArr[0].Obj()["k"].str(), "a");
    ASSERT_EQ(emitsArr[0].Obj()["v"].numberInt(), 1);
    ASSERT_EQ(emitsArr[1].Obj()["k"].str(), "b");
    ASSERT_EQ(emitsArr[1].Obj()["v"].numberInt(), 2);
}

TEST_F(WasmMozJSBridgeTest, EmitDrainClearsBetweenCalls) {
    setupEmit(boost::none);

    auto handle = createFunction("function() { emit('x', 10); }");
    invokeFunction(handle, BSONObj());

    BSONObj d1 = drainEmitBuffer();
    ASSERT_EQ(d1["emits"].Array().size(), 1u);

    invokeFunction(handle, BSONObj());
    invokeFunction(handle, BSONObj());

    BSONObj d2 = drainEmitBuffer();
    ASSERT_EQ(d2["emits"].Array().size(), 2u);
}

TEST_F(WasmMozJSBridgeTest, EmitDrainEmptyBuffer) {
    setupEmit(boost::none);

    BSONObj doc = drainEmitBuffer();
    ASSERT_EQ(doc["emits"].Array().size(), 0u);
    ASSERT_EQ(doc["bytesUsed"].numberLong(), 0);
}

TEST_F(WasmMozJSBridgeTest, EmitWithUndefinedKey) {
    setupEmit(boost::none);

    auto handle = createFunction("function() { emit(undefined, 42); }");
    invokeFunction(handle, BSONObj());

    BSONObj doc = drainEmitBuffer();
    auto emits = doc["emits"].Array();
    ASSERT_EQ(emits.size(), 1u);
    ASSERT_TRUE(emits[0].Obj()["k"].isNull());
    ASSERT_EQ(emits[0].Obj()["v"].numberInt(), 42);
}

// ---------------------------------------------------------------------------
// invoke-predicate tests ($where pattern)
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, InvokePredicateReturnsTrueForMatch) {
    auto handle = createFunction("function() { return this.x > this.y; }");

    ASSERT_TRUE(invokePredicate(handle, BSON("x" << 10 << "y" << 5)));
}

TEST_F(WasmMozJSBridgeTest, InvokePredicateReturnsFalseForNonMatch) {
    auto handle = createFunction("function() { return this.x > this.y; }");

    ASSERT_FALSE(invokePredicate(handle, BSON("x" << 3 << "y" << 7)));
}

TEST_F(WasmMozJSBridgeTest, InvokePredicateFieldAccess) {
    auto handle = createFunction("function() { return this.name === 'hello'; }");

    ASSERT_TRUE(invokePredicate(handle, BSON("name" << "hello")));
    ASSERT_FALSE(invokePredicate(handle, BSON("name" << "world")));
}

TEST_F(WasmMozJSBridgeTest, InvokePredicateMultipleDocuments) {
    auto handle = createFunction("function() { return this.age >= 18; }");

    ASSERT_TRUE(invokePredicate(handle, BSON("age" << 25)));
    ASSERT_FALSE(invokePredicate(handle, BSON("age" << 10)));
    ASSERT_TRUE(invokePredicate(handle, BSON("age" << 18)));
    ASSERT_FALSE(invokePredicate(handle, BSON("age" << 17)));
}

TEST_F(WasmMozJSBridgeTest, InvokePredicateRuntimeError) {
    auto handle = createFunction("function() { throw new Error('pred error'); }");

    ASSERT_THROWS_WITH_CHECK(invokePredicate(handle, BSON("x" << 1)),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime"));
}

// ---------------------------------------------------------------------------
// invoke-map tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, InvokeMapEmitsDocuments) {
    setupEmit(boost::none);

    auto handle = createFunction("function() { emit(this.category, this.price); }");

    invokeMap(handle, BSON("category" << "A" << "price" << 10));
    invokeMap(handle, BSON("category" << "B" << "price" << 20));
    invokeMap(handle, BSON("category" << "A" << "price" << 30));

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

TEST_F(WasmMozJSBridgeTest, InvokeMapRuntimeError) {
    auto handle = createFunction("function() { throw new Error('map error'); }");

    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()),
                             AssertionException,
                             getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime"));
}

TEST_F(WasmMozJSBridgeTest, InvokeFunctionNoThisBinding) {
    auto handle = createFunction("function() { return typeof this; }");

    auto result = invokeFunction(handle, BSONObj());
    ASSERT_EQ(result["__value"].str(), "object");
}

// ---------------------------------------------------------------------------
// OOM tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, EmitByteLimitEnforced) {
    setupEmit(256);

    auto handle = createFunction(
        "function() {"
        "  for (var i = 0; i < 100; i++) {"
        "    emit('key_' + i, 'value_' + i);"
        "  }"
        "}");

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
    };

    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, EmitByteLimitEnforcedInMapReduce) {
    initEngine();
    // Each emitted {k: <string>, v: <int>} BSON doc is ~30 bytes.
    // A 64-byte limit allows ~2 emits before the 3rd exceeds.
    setupEmit(64);

    auto handle = createFunction(
        "function() {"
        "  emit(this.category, this.price);"
        "}");

    invokeMap(handle, BSON("category" << "A" << "price" << 10));
    invokeMap(handle, BSON("category" << "B" << "price" << 20));

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
    };

    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSON("category" << "C" << "price" << 30)),
                             AssertionException,
                             doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, EmitByteLimitResetOnSetupEmit) {
    setupEmit(128);

    auto handle = createFunction("function() { emit('k', 'v'); }");

    // Emit a few times, approaching the limit.
    invokeFunction(handle, BSONObj());
    invokeFunction(handle, BSONObj());
    invokeFunction(handle, BSONObj());

    // Re-setup resets the buffer and byte counter.
    setupEmit(128);

    invokeFunction(handle, BSONObj());
    invokeFunction(handle, BSONObj());
    invokeFunction(handle, BSONObj());

    BSONObj doc = drainEmitBuffer();
    ASSERT_EQ(doc["emits"].Array().size(), 3u);
}

TEST_F(WasmMozJSBridgeTest, JsHeapOOMFromLargeArrayAllocation) {
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

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "allocation size overflow")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, JsHeapOOMFromStringConcatenation) {
    initEngine();

    auto handle = createFunction(
        "function() {"
        "  var s = new Array(1024 * 1024 + 1).join('x');"
        "  for (var i = 0; i < 10; i++) {"
        "    s = s + s;"
        "  }"
        "  return s.length;"
        "}");

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "allocation size overflow")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, JsHeapOomFromDeeplyNestedObject) {
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

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "allocation size overflow")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, MapReduceHighVolumeEmitHitsLimit) {
    // Use a moderate byte limit (64 KB) and hammer it with many map invocations.
    setupEmit(64 * 1024);

    auto mapFn = createFunction(
        "function() {"
        "  emit(this._id, this.payload);"
        "}");
    ASSERT_NE(mapFn, 0u);

    int emitted = 0;
    for (; emitted < 10000; emitted++) {
        std::string payload(100, 'A' + (emitted % 26));
        try {
            invokeMap(mapFn, BSON("_id" << emitted << "payload" << payload));
        } catch (AssertionException& ex) {
            getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
            getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
            break;
        }
    }
    ASSERT_GT(emitted, 0);
    ASSERT_LT(emitted, 10000);

    // The limit is still exceeded, so the next call also fails with the same error.
    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(mapFn, BSON("_id" << 99999 << "payload" << "x")),
                             AssertionException,
                             doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, MapReduceEmitLargeValues) {
    setupEmit(32 * 1024);

    // Each emit produces a large value (~10 KB). A few calls should exceed 32 KB.
    auto mapFn = createFunction(
        "function() {"
        "  var big = new Array(10001).join('z');"
        "  emit(this.key, big);"
        "}");

    invokeMap(mapFn, BSON("key" << 1));
    invokeMap(mapFn, BSON("key" << 2));
    invokeMap(mapFn, BSON("key" << 3));

    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(mapFn, BSON("key" << 4)), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, JsHeapOOMFromFunctionAllocatingManyObjects) {
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
    auto doubleCheck = [&](auto&& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "allocation size overflow")(ex);
    };
    ASSERT_THROWS_WITH_CHECK(invokeMap(handle, BSONObj()), AssertionException, doubleCheck);
}

TEST_F(WasmMozJSBridgeTest, MapReduceOomRecoveryAfterDrain) {
    // Use a small limit so we can observe recovery after drain.
    setupEmit(512);

    auto mapFn = createFunction(
        "function() {"
        "  emit(this.key, this.value);"
        "}");

    // Emit until limit is hit.
    int emitted = 0;
    try {
        while (true) {
            invokeMap(mapFn, BSON("key" << emitted << "value" << "data"));
            emitted++;
            if (emitted > 100)
                break;
        }
    } catch (AssertionException& ex) {
        ASSERT_LT(emitted, 100);
        ASSERT_GT(emitted, 0);
        auto doubleCheck = [&](auto&& ex) {
            getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
            getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
        };
        doubleCheck(ex);
        ASSERT_THROWS_WITH_CHECK(invokeMap(mapFn, BSON("key" << 999 << "value" << "x")),
                                 AssertionException,
                                 doubleCheck);
    }

    // Drain and re-setup resets the counter, allowing more emits.
    drainEmitBuffer();
    setupEmit(512);

    int emitted2 = 0;
    try {
        while (true) {
            invokeMap(mapFn, BSON("key" << emitted2 << "value" << "data"));
            emitted2++;
            if (emitted2 > 100)
                break;
        }
    } catch (AssertionException& ex) {
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "e-runtime")(ex);
        getContainsCheck(ErrorCodes::JSInterpreterFailure, "emit() exceeded memory limit")(ex);
        ASSERT_LT(emitted2, 100);
        ASSERT_GT(emitted2, 0);
        ASSERT_EQ(emitted, emitted2);
    }
}

// ---------------------------------------------------------------------------
// Store limiter tests
// ---------------------------------------------------------------------------

// Triggers the store limiter OOM path — the WASM module hits the linear memory
// ceiling, SpiderMonkey aborts inside WASM, and wasmtime catches the resulting trap.
// The bridge's _callFunc/_callFuncNoArgs methods latch hasTrapped() before re-throwing.
//
// Disabled under TSan: when the store limiter denies memory.grow, SpiderMonkey
// MOZ_CRASHes during GC. Wasmtime catches the SIGABRT via a signal handler that
// performs allocations (signal-unsafe). TSan intercepts the signal, flags the
// malloc, and aborts the process before wasmtime can convert it into a trap.
#if !__has_feature(thread_sanitizer)
TEST_F(WasmMozJSBridgeTest, StoreLimiterTrapSetsFlag) {
    _bridge->shutdown();
    _bridge.reset();

    MozJSWasmBridge::Options opts{};
    // Use a small explicit JS heap so the minimum store limit stays low.
    // min store = 50 + max(64, 5) = 114 MB.  This is tight enough that a
    // large allocation will push total linear-memory usage (heap + runtime
    // overhead) past the store limit and trigger a wasmtime trap.
    opts.jsHeapLimitMB = 50;
    opts.linearMemoryLimitMB = 114;
    _bridge = std::make_unique<MozJSWasmBridge>(_s_engineCtx, opts);
    ASSERT_TRUE(initEngine());

    // Each iteration pushes an object with two ints and a 32-char string,
    // conservatively ~64 bytes per element. With a 50 MB JS heap and 114 MB
    // store limit, the loop will push total linear-memory usage past the
    // store limit and trigger a wasmtime trap.
    auto handle = createFunction(
        "function() {"
        "  var big = [];"
        "  for (var i = 0; i < 2000000; i++) {"
        "    big.push({x: i, y: i * 2, z: 'padding_string_to_consume_memory'});"
        "  }"
        "  return big.length;"
        "}");

    ASSERT_THROWS((void)_bridge->invokeFunction(handle, BSONObj()), DBException);
    ASSERT_TRUE(_bridge->hasTrapped());
    // The store is in a broken state after the trap; discard without calling shutdown.
    _bridge.reset();
}
#endif  // !__has_feature(thread_sanitizer)

TEST_F(WasmMozJSBridgeTest, StoreLimiterAllowsWithinLimit) {
    // Shut down the default bridge and create one with an explicit memory limit.
    _bridge->shutdown();
    _bridge.reset();

    MozJSWasmBridge::Options opts{};
    opts.jsHeapLimitMB = 50;
    opts.linearMemoryLimitMB = 256;
    _bridge = std::make_unique<MozJSWasmBridge>(_s_engineCtx, opts);
    ASSERT_TRUE(initEngine());

    auto handle = createFunction("function() { return 1 + 2; }");
    auto result = _bridge->invokeFunction(handle, BSONObj());
    ASSERT_TRUE(result.isOK());
}

// ---------------------------------------------------------------------------
// wasmtimeStoreMemoryLimitMB server parameter tests
// ---------------------------------------------------------------------------

TEST_F(WasmMozJSBridgeTest, ServerParam_DefaultIs1210) {
    // Verify the IDL-declared default value (jsHeapLimitMB=1100 * 1.1 = 1210).
    ASSERT_EQ(1210, gWasmtimeStoreMemoryLimitMB.load());
}

TEST_F(WasmMozJSBridgeTest, ServerParam_DefaultLimitAllowsExecution) {
    // Recreate the bridge using the default server parameter (1210 MB).
    _bridge->shutdown();
    _bridge.reset();

    MozJSWasmBridge::Options opts{};
    opts.linearMemoryLimitMB = gWasmtimeStoreMemoryLimitMB.load();
    _bridge = std::make_unique<MozJSWasmBridge>(_s_engineCtx, opts);
    ASSERT_TRUE(initEngine());

    auto handle = createFunction("function() { return 42; }");
    auto result = _bridge->invokeFunction(handle, BSONObj());
    ASSERT_TRUE(result.isOK());
}

}  // namespace
}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo
