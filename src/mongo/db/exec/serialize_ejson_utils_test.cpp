/**
 *    Copyright (C) 2025-present MongoDB, Inc.
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


#include "mongo/db/exec/serialize_ejson_utils.h"

#include "mongo/bson/bson_depth.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/unittest/unittest.h"

#define MONGO_LOGV2_DEFAULT_COMPONENT ::mongo::logv2::LogComponent::kTest

namespace mongo::exec::expression::serialize_ejson_utils {
namespace {

/// Specify if the input contains any BSON-only types or values.
enum class JsonCompatible { no, yes };

/**
 * Holds input BSON and expected Extended JSON representations.
 */
struct TestCase {
    explicit(false) TestCase(auto&& b, auto&& c, auto&& r, JsonCompatible j)
        : bson(b), canonical(c), relaxed(r), jsonCompatible(j) {}

    Value bson;
    Value canonical;
    Value relaxed;
    JsonCompatible jsonCompatible;
};

constexpr uint8_t kUuidBytes[] = {0, 0, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0, 0};

const TestCase testCases[]{
    // minKey
    {MINKEY, BSON("$minKey" << 1), BSON("$minKey" << 1), JsonCompatible::no},
    // double
    {42.42, BSON("$numberDouble" << "42.42"), 42.42, JsonCompatible::yes},
    {1e-18, BSON("$numberDouble" << "1e-18"), 1e-18, JsonCompatible::yes},
    {std::numeric_limits<double>::infinity(),
     BSON("$numberDouble" << "Infinity"),
     BSON("$numberDouble" << "Infinity"),
     JsonCompatible::no},
    {-std::numeric_limits<double>::infinity(),
     BSON("$numberDouble" << "-Infinity"),
     BSON("$numberDouble" << "-Infinity"),
     JsonCompatible::no},
    {std::numeric_limits<double>::quiet_NaN(),
     BSON("$numberDouble" << "NaN"),
     BSON("$numberDouble" << "NaN"),
     JsonCompatible::no},
    // string
    {"string"_sd, "string"_sd, "string"_sd, JsonCompatible::yes},
    // object
    {BSON("foo" << 1),
     BSON("foo" << BSON("$numberInt" << "1")),
     BSON("foo" << 1),
     JsonCompatible::yes},
    {BSON("foo" << BSON("bar" << 1)),
     BSON("foo" << BSON("bar" << BSON("$numberInt" << "1"))),
     BSON("foo" << BSON("bar" << 1)),
     JsonCompatible::yes},
    // array
    {BSON_ARRAY(1 << 2),
     BSON_ARRAY(BSON("$numberInt" << "1") << BSON("$numberInt" << "2")),
     BSON_ARRAY(1 << 2),
     JsonCompatible::yes},
    // binData
    {BSONBinData("123", 3, BinDataType::newUUID),
     BSON("$binary" << BSON("base64" << "MTIz" << "subType" << "4")),
     BSON("$binary" << BSON("base64" << "MTIz" << "subType" << "4")),
     JsonCompatible::no},
    {BSONBinData(kUuidBytes, 16, BinDataType::newUUID),
     BSON("$uuid" << "00000000-0000-4000-8000-000000000000"),
     BSON("$uuid" << "00000000-0000-4000-8000-000000000000"),
     JsonCompatible::no},
    {BSONBinData("123", 3, BinDataType::bdtCustom),
     BSON("$binary" << BSON("base64" << "MTIz" << "subType" << "80")),
     BSON("$binary" << BSON("base64" << "MTIz" << "subType" << "80")),
     JsonCompatible::no},
    // undefined
    {BSONUndefined, BSON("$undefined" << true), BSON("$undefined" << true), JsonCompatible::no},
    // oid
    {OID("57e193d7a9cc81b4027498b5"),
     BSON("$oid" << "57e193d7a9cc81b4027498b5"),
     BSON("$oid" << "57e193d7a9cc81b4027498b5"),
     JsonCompatible::no},
    // boolean
    {true, true, true, JsonCompatible::yes},
    {false, false, false, JsonCompatible::yes},
    // date
    {Date_t(),
     BSON("$date" << BSON("$numberLong" << "0")),
     BSON("$date" << "1970-01-01T00:00:00.000Z"),
     JsonCompatible::no},
    {Date_t::max(),
     BSON("$date" << BSON("$numberLong" << "9223372036854775807")),
     BSON("$date" << BSON("$numberLong" << "9223372036854775807")),
     JsonCompatible::no},
    {Date_t::min(),
     BSON("$date" << BSON("$numberLong" << "-9223372036854775808")),
     BSON("$date" << BSON("$numberLong" << "-9223372036854775808")),
     JsonCompatible::no},
    {Date_t::fromDurationSinceEpoch(stdx::chrono::years{50}),
     BSON("$date" << BSON("$numberLong" << "1577847600000")),
     BSON("$date" << "2020-01-01T03:00:00.000Z"),
     JsonCompatible::no},
    // null
    {BSONNULL, BSONNULL, BSONNULL, JsonCompatible::yes},
    // regEx
    {BSONRegEx("foo*", "ig"),
     BSON("$regularExpression" << BSON("pattern" << "foo*" << "options" << "ig")),
     BSON("$regularExpression" << BSON("pattern" << "foo*" << "options" << "ig")),
     JsonCompatible::no},
    // dbRef
    {BSONDBRef("collection", OID("57e193d7a9cc81b4027498b1")),
     BSON("$dbPointer" << BSON("$ref" << "collection" << "$id" << "57e193d7a9cc81b4027498b1")),
     BSON("$dbPointer" << BSON("$ref" << "collection" << "$id" << "57e193d7a9cc81b4027498b1")),
     JsonCompatible::no},
    // code
    {BSONCode("function() {}"),
     BSON("$code" << "function() {}"),
     BSON("$code" << "function() {}"),
     JsonCompatible::no},
    // symbol
    {BSONSymbol("symbol"),
     BSON("$symbol" << "symbol"),
     BSON("$symbol" << "symbol"),
     JsonCompatible::no},
    // codeWScope
    {BSONCodeWScope("function() {}", BSON("n" << 5)),
     BSON("$code" << "function() {}" << "$scope" << BSON("n" << BSON("$numberInt" << "5"))),
     // the $scope is always in canonical format
     BSON("$code" << "function() {}" << "$scope" << BSON("n" << BSON("$numberInt" << "5"))),
     JsonCompatible::no},
    // numberInt
    {42, BSON("$numberInt" << "42"), 42, JsonCompatible::yes},
    // timestamp
    {Timestamp(42, 1),
     BSON("$timestamp" << BSON("t" << 42 << "i" << 1)),
     BSON("$timestamp" << BSON("t" << 42 << "i" << 1)),
     JsonCompatible::no},
    // numberLong
    {42LL, BSON("$numberLong" << "42"), 42LL, JsonCompatible::yes},
    // numberDecimal
    {Decimal128(42.42),
     BSON("$numberDecimal" << "42.4200000000000"),
     BSON("$numberDecimal" << "42.4200000000000"),
     JsonCompatible::no},
    {Decimal128::kPositiveInfinity,
     BSON("$numberDecimal" << "Infinity"),
     BSON("$numberDecimal" << "Infinity"),
     JsonCompatible::no},
    {Decimal128::kNegativeInfinity,
     BSON("$numberDecimal" << "-Infinity"),
     BSON("$numberDecimal" << "-Infinity"),
     JsonCompatible::no},
    {Decimal128::kPositiveNaN,
     BSON("$numberDecimal" << "NaN"),
     BSON("$numberDecimal" << "NaN"),
     JsonCompatible::no},
    {Decimal128::kNegativeNaN,
     BSON("$numberDecimal" << "NaN"),
     BSON("$numberDecimal" << "NaN"),
     JsonCompatible::no},
    // maxKey
    {MAXKEY, BSON("$maxKey" << 1), BSON("$maxKey" << 1), JsonCompatible::no},
};

/**
 * Generate Extended JSON string using the native BSONObj method.
 * This is considered the golden truth value.
 */
std::string jsonString(const Value& v, bool relaxed) {
    BSONArrayBuilder builder;
    v.addToBsonArray(&builder);
    BSONArray bsonArr = builder.arr();
    auto format = relaxed ? JsonStringFormat::ExtendedRelaxedV2_0_0
                          : JsonStringFormat::ExtendedCanonicalV2_0_0;
    return bsonArr.firstElement().jsonString(format, false, false);
}

/**
 * Disable the generator for types where the conversion does not follow the spec.
 */
bool isGeneratorBugged(BSONType t) {
    switch (t) {
        case BSONType::dbRef:
            // ExtendedCanonicalV200Generator does not emit $dbPointer.
            return true;
        default:
            return false;
    }
}

/**
 * Test against the native BSONObj Extended JSON method.
 */
void testGenerator(const Value& v, bool relaxed) {
    auto golden = jsonString(v, relaxed);
    auto test = jsonString(serializeToExtendedJson(v, relaxed), true);
    ASSERT_EQ(golden, test) << (relaxed ? "relaxed" : "canonical") << " format";
}

TEST(SerializeExtendedJsonUtilsTest, SerializeSucceedsOnTestCases) {
    for (auto& tc : testCases) {
        auto testCanonical = serializeToExtendedJson(tc.bson, false);
        auto testRelaxed = serializeToExtendedJson(tc.bson, true);

        if (!tc.bson.missing() && !isGeneratorBugged(tc.bson.getType())) {
            testGenerator(tc.bson, false);
            testGenerator(tc.bson, true);
        }

        ASSERT_VALUE_EQ(testCanonical, tc.canonical);
        ASSERT_VALUE_EQ(testRelaxed, tc.relaxed);
    }
}

TEST(SerializeExtendedJsonUtilsTest, SerializeThrowsOnMissingValues) {
    ASSERT_THROWS_CODE(serializeToExtendedJson(Value(), false), DBException, ErrorCodes::BadValue);
    ASSERT_THROWS_CODE(serializeToExtendedJson(Value(), true), DBException, ErrorCodes::BadValue);
}

Value makeNested(Value v, int depth) {
    std::vector<Value> next{std::move(v)};
    for (int i = 0; i < depth; i++) {
        next = std::vector<Value>{Value(std::move(next))};
    }
    return Value(std::move(next));
}

TEST(SerializeExtendedJsonUtilsTest, SerializeThrowsOnNestingLimit) {
    int depthLimit = BSONDepth::getMaxAllowableDepth();
    auto nested = makeNested(Value(1), depthLimit - 2);
    // Does not throw
    serializeToExtendedJson(nested, true);
    // Throws because of added nesting level {$numberInt: "1"}.
    ASSERT_THROWS_CODE(
        serializeToExtendedJson(nested, false), DBException, ErrorCodes::ConversionFailure);
}

TEST(SerializeExtendedJsonUtilsTest, SerializeThrowsOnSizeLimit) {
    const size_t sizeLimit = BSONObjMaxUserSize;
    // Compute string length required for our object.
    size_t stringLengthToHitLimit = sizeLimit - BSON("string" << "" << "number" << 1).objsize();
    auto largeObject = BSON("string" << std::string(stringLengthToHitLimit, 'x') << "number" << 1);
    ASSERT_EQ(largeObject.objsize(), BSONObjMaxUserSize);
    auto largeValue = Value(largeObject);

    // Does not throw
    serializeToExtendedJson(largeValue, true);
    // Throws because of added type wrapper {$numberInt: "1"}.
    ASSERT_THROWS_CODE(
        serializeToExtendedJson(largeValue, false), DBException, ErrorCodes::ConversionFailure);
}

}  // namespace
}  // namespace mongo::exec::expression::serialize_ejson_utils
