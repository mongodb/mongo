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

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/oid.h"
#include "mongo/bson/timestamp.h"
#include "mongo/platform/decimal128.h"
#include "mongo/unittest/unittest.h"

#include <cstdint>
#include <cstring>
#include <set>
#include <string>
#include <vector>

// Symbols produced by objcopy from the AOT-compiled mozjs_wasm_api.cwasm.
// See the embed_mozjs_wasm_obj genrule in BUILD.bazel.
extern "C" {
extern const uint8_t _binary_mozjs_wasm_api_cwasm_start[];
extern const uint8_t _binary_mozjs_wasm_api_cwasm_end[];
}

namespace mongo {
namespace mozjs {
namespace wasm {
namespace {

// Shares wasmtime engine + compiled component across all tests via WasmEngineContext.
// Each test gets a fresh MozJSWasmBridge (store + instance) for isolation.
class WasmMozJSBridgeTest : public unittest::Test {
public:
    static void SetUpTestSuite() {
        size_t size = static_cast<size_t>(_binary_mozjs_wasm_api_cwasm_end -
                                          _binary_mozjs_wasm_api_cwasm_start);
        _s_engineCtx =
            WasmEngineContext::createFromPrecompiled(_binary_mozjs_wasm_api_cwasm_start, size);
    }

protected:
    void setUp() override {
        _bridge = std::make_unique<MozJSWasmBridge>(_s_engineCtx);
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
    ASSERT_THROWS_CODE(invokeFunction(h1, emptyArgs), AssertionException, 11542314);
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
    ASSERT_THROWS_CODE(invokeFunction(0, BSONObj()), AssertionException, 11542314);
}

TEST_F(WasmMozJSBridgeTest, CreateFunctionWithInvalidSourceFails) {
    // Invalid JavaScript - not a function expression; the bridge throws via uassert.
    ASSERT_THROWS_CODE(
        createFunction("this is not valid javascript {{{"), AssertionException, 11542311);
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

}  // namespace
}  // namespace wasm
}  // namespace mozjs
}  // namespace mongo
