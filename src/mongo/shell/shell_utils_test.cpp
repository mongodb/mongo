/**
 *    Copyright (C) 2019-present MongoDB, Inc.
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

#include "mongo/shell/shell_utils.h"

#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/bsonobj.h"
#include "mongo/bson/bsontypes.h"
#include "mongo/bson/json.h"
#include "mongo/bson/timestamp.h"
#include "mongo/shell/shell_utils_extended.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/temp_dir.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {
using shell_utils::getObjInDumpFile;
using shell_utils::getURIFromArgs;
using shell_utils::NormalizationOpts;
using shell_utils::NormalizationOptsSet;
using shell_utils::normalizeBSONObj;
using shell_utils::numObjsInDumpFile;
using shell_utils::readDumpFile;
using shell_utils::removeFile;
using shell_utils::writeBsonArrayToFile;

using namespace std::literals::string_literals;

const auto fullOpts = NormalizationOpts::kSortBSON | NormalizationOpts::kSortArrays |
    NormalizationOpts::kNormalizeNumerics;

TEST(ShellUtils, BalancedTest) {
    using shell_utils::isBalanced;
    struct {
        std::string in;
        bool isBalanced;
    } specs[] = {{"x = 5", true},
                 {"function(){}", true},
                 {"function(){\n}", true},
                 {"function(){", false},
                 {R"(x = "{";)", true},
                 {"// {", true},
                 {"// \n {", false},
                 {R"("//" {)", false},
                 {R"({x:/x\//})", true},
                 {R"({ \/// })", false},
                 {"x = 5 + y ", true},
                 {"x = ", false},
                 {"x = // hello", false},
                 {"x = 5 +", false},
                 {" x ++", true},
                 {"-- x", true},
                 {"a.", false},
                 {"a. ", false},
                 {"a.b", true},
                 // SERVER-5809 and related cases --
                 {R"(a = {s:"\""})", true},
                 {R"(db.test.save({s:"\""}))", true},
                 {R"(printjson(" \" "))", true},  //-- SERVER-8554
                 {R"(var a = "\\";)", true},
                 {R"(var a = ("\\") //")", true},
                 {R"(var a = ("\\") //\")", true},
                 {R"(var a = ("\\") //\")", true},
                 {R"(var a = ("\\") //)", true},
                 {R"(var a = ("\\"))", true},
                 {R"(var a = ("\\\""))", true},
                 {R"(var a = ("\\" //")", false},
                 {R"(var a = ("\\" //)", false},
                 {R"(var a = ("\\")", false}};
    for (const auto& spec : specs) {
        ASSERT_EQUALS(isBalanced(spec.in), spec.isBalanced);
    }
}

// Numeric arrays.
TEST(NormalizeBSONObj, NumericArray) {
    BSONObj bson = fromjson("{a: [4, 3, 2, 1, 0]}");
    std::string expected =
        "{ a: [ 0E-6176, 1.000000000000000000000000000000000, 2.000000000000000000000000000000000, "
        "3.000000000000000000000000000000000, 4.000000000000000000000000000000000 ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, NumericNestedArray) {
    BSONObj bson = fromjson("{a: [4, [1, 2, 3], 2, 1, 0]}");
    std::string expected =
        "{ a: [ 0E-6176, 1.000000000000000000000000000000000, 2.000000000000000000000000000000000, "
        "4.000000000000000000000000000000000, [ 1.000000000000000000000000000000000, "
        "2.000000000000000000000000000000000, 3.000000000000000000000000000000000 ] ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, NumericNestedArrayDiffFieldOrder) {
    BSONObj bson = fromjson("{b: [2, 1], a: [4, [1, 2, 3], 2, 1, 0]}");
    std::string expected =
        "{ a: [ 0E-6176, 1.000000000000000000000000000000000, 2.000000000000000000000000000000000, "
        "4.000000000000000000000000000000000, [ 1.000000000000000000000000000000000, "
        "2.000000000000000000000000000000000, 3.000000000000000000000000000000000 ] ], b: [ "
        "1.000000000000000000000000000000000, 2.000000000000000000000000000000000 ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, NumericNestedArrayDiffFields) {
    BSONObj a = fromjson("{a: [0, 1, 2, [3, 2, 1], 4], b: [1, 2]}");
    BSONObj b = fromjson("{a: [2, 1], b: [4, [1, 2, 3], 2, 1, 0]}");
    ASSERT_NOT_EQUALS(normalizeBSONObj(a, fullOpts).toString(),
                      normalizeBSONObj(b, fullOpts).toString());
}

TEST(NormalizeBSONObj, NumericArrayWithUnsortedNestedArrays) {
    BSONObj bson = fromjson("{a: [[9, 2, 3], [8, 0, 1], [7, 5, 6]]}");
    std::string expected =
        "{ a: [ [ 0E-6176, 1.000000000000000000000000000000000, "
        "8.000000000000000000000000000000000 ], [ 2.000000000000000000000000000000000, "
        "3.000000000000000000000000000000000, 9.000000000000000000000000000000000 ], [ "
        "5.000000000000000000000000000000000, 6.000000000000000000000000000000000, "
        "7.000000000000000000000000000000000 ] ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Mixed type numeric objects and arrays.
TEST(NormalizeBSONObj, NumericArrayDiffTypes) {
    BSONObj bson = fromjson("{a: [NumberDecimal('1.0'), NumberLong(1), NumberInt(1), 1.0]}");
    std::string expected =
        "{ a: [ 1.000000000000000000000000000000000, 1.000000000000000000000000000000000, "
        "1.000000000000000000000000000000000, 1.000000000000000000000000000000000 ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, NumericArrayDiffTypesDiffVals) {
    BSONObj bson = fromjson("{a: [NumberDecimal('1.00010'), 1.0, 1, 1.01]}");
    std::string expected =
        "{ a: [ 1.000000000000000000000000000000000, 1.000000000000000000000000000000000, "
        "1.000100000000000000000000000000000, 1.010000000000000000000000000000000 ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, NumericArrayDiffVals) {
    BSONObj a = fromjson("{a: [1.01, NumberDecimal('1.0001'), 1.0002]}");
    BSONObj b = fromjson("{a: [NumberDecimal('1.00012'), 1.0002, 1.01]}");
    ASSERT_NOT_EQUALS(normalizeBSONObj(a, fullOpts).toString(),
                      normalizeBSONObj(b, fullOpts).toString());
}

TEST(NormalizeBSONObj, NumericObjDiffTypes) {
    BSONObj bson = fromjson("{a: NumberDecimal('1.0'), b: NumberInt(1)}");
    std::string expected =
        "{ a: 1.000000000000000000000000000000000, b: 1.000000000000000000000000000000000 }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// String arrays.
TEST(NormalizeBSONObj, StringArray) {
    BSONObj bson = fromjson("{a: ['B', 'b', 'A', 'ab', 'a']}");
    std::string expected = "{ a: [ \"A\", \"B\", \"a\", \"ab\", \"b\" ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, StringArrayNested) {
    BSONObj bson = fromjson("{a: ['b', ['a', 'ab', 'A'], 'A', 'ab', 'a']}");
    std::string expected = "{ a: [ \"A\", \"a\", \"ab\", \"b\", [ \"A\", \"a\", \"ab\" ] ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Object arrays.
TEST(NormalizeBSONObj, ObjectArray) {
    BSONObj bson = fromjson("{a: [{d: 1, c: 1}, {b: 1}, {a: 1}]}");
    std::string expected =
        "{ a: [ { a: 1.000000000000000000000000000000000 }, { b: "
        "1.000000000000000000000000000000000 }, { c: 1.000000000000000000000000000000000, d: "
        "1.000000000000000000000000000000000 } ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, ObjectArrayNested) {
    BSONObj bson = fromjson("{a: [{b: 1}, {d: [{b: 1}, {d: 1, c: 1}, {a: 1}], c: 1}, {a: 1}]}");
    std::string expected =
        "{ a: [ { a: 1.000000000000000000000000000000000 }, { b: "
        "1.000000000000000000000000000000000 }, { c: 1.000000000000000000000000000000000, d: [ { "
        "a: 1.000000000000000000000000000000000 }, { b: 1.000000000000000000000000000000000 }, { "
        "c: 1.000000000000000000000000000000000, d: 1.000000000000000000000000000000000 } ] } ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, ObjectArrayWithNestedArrays) {
    BSONObj bson = fromjson("{a: {d: [{d: [5, 7, 6]}, {c: [[10, 1], [4, 5], [3, 7], 1]}]}}");
    std::string expected =
        "{ a: { d: [ { c: [ 1.000000000000000000000000000000000, [ "
        "1.000000000000000000000000000000000, 10.00000000000000000000000000000000 ], [ "
        "3.000000000000000000000000000000000, 7.000000000000000000000000000000000 ], [ "
        "4.000000000000000000000000000000000, 5.000000000000000000000000000000000 ] ] }, { d: [ "
        "5.000000000000000000000000000000000, 6.000000000000000000000000000000000, "
        "7.000000000000000000000000000000000 ] } ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, ObjectArrayWithDiffNestedArrays) {
    BSONObj a = fromjson("{a: {d: [{c: [1, [7, 3], [5, 4], [1, 10]]}, {d: [7, 6, 5]}]}}");
    BSONObj b = fromjson("{a: {d: [{d: [5, 7, 6]}, {c: [[10, 4], [1, 5], [3, 7], 1]}]}}");
    ASSERT_NOT_EQUALS(normalizeBSONObj(a, fullOpts).toString(),
                      normalizeBSONObj(b, fullOpts).toString());
}

TEST(NormalizeBSONObj, ObjectArrayWithDiffFields) {
    BSONObj bson = fromjson("{a: {d: [{c: [5, 7, 6]}, {c: [[10, 1], [4, 5], [3, 7], 1]}]}}");
    std::string expected =
        "{ a: { d: [ { c: [ 1.000000000000000000000000000000000, [ "
        "1.000000000000000000000000000000000, 10.00000000000000000000000000000000 ], [ "
        "3.000000000000000000000000000000000, 7.000000000000000000000000000000000 ], [ "
        "4.000000000000000000000000000000000, 5.000000000000000000000000000000000 ] ] }, { c: [ "
        "5.000000000000000000000000000000000, 6.000000000000000000000000000000000, "
        "7.000000000000000000000000000000000 ] } ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// BinData arrays.
TEST(NormalizeBSONObj, BinDataArray) {
    BSONBinData a = BSONBinData("abcdabcdabcdabcd", 16, BinDataType::newUUID);
    BSONBinData b = BSONBinData("abcdabcdabcdabce", 16, BinDataType::newUUID);

    BSONObj bson = BSON("a" << BSON_ARRAY(b << a));
    std::string expected =
        "{ a: [ UUID(\"61626364-6162-6364-6162-636461626364\"), "
        "UUID(\"61626364-6162-6364-6162-636461626365\") ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// ObjectId arrays.
TEST(NormalizeBSONObj, ObjectIDArray) {
    OID a = OID("112233445566778899AABBCC");
    OID b = OID("112233445566778899AABBCB");

    BSONObj bson = BSON("a" << BSON_ARRAY(a << b));
    std::string expected =
        "{ a: [ ObjectId('112233445566778899aabbcb'), ObjectId('112233445566778899aabbcc') ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Boolean arrays.
TEST(NormalizeBSONObj, BooleanArray) {
    BSONObj bson = fromjson("{a: [true, false, false]}");
    std::string expected = "{ a: [ false, false, true ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, BooleanNestedArray) {
    BSONObj bson = fromjson("{a: [true, false, [false, true]]}");
    std::string expected = "{ a: [ [ false, true ], false, true ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Date arrays.
TEST(NormalizeBSONObj, DateArray) {
    BSONObj bson = fromjson(
        "{a: [new Date(1351242000002), new Date(1351242000000), new Date(1351242000001)]}");
    std::string expected =
        "{ a: [ new Date(1351242000000), new Date(1351242000001), new Date(1351242000002) ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Timestamp arrays.
TEST(NormalizeBSONObj, TimestampArray) {
    BSONObj bson = BSON("a" << BSON_ARRAY(Timestamp(3, 4) << Timestamp(2, 3) << Timestamp(1, 2)));
    std::string expected = "{ a: [ Timestamp(1, 2), Timestamp(2, 3), Timestamp(3, 4) ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Regex arrays.
TEST(NormalizeBSONObj, RegexArray) {
    BSONObj bson = BSON(
        "a" << BSON_ARRAY(BSONRegEx("reg.ex") << BSONRegEx("a*.conn") << BSONRegEx("/regex/")));
    std::string expected = "{ a: [ //regex//, /a*.conn/, /reg.ex/ ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Mixed type arrays.
TEST(NormalizeBSONObj, MixedTypeArray) {
    BSONObj bson = fromjson("{a: [[3, 2], true, 0, {b: 5}, 'a']}");
    std::string expected =
        "{ a: [ 0E-6176, \"a\", { b: 5.000000000000000000000000000000000 }, [ "
        "2.000000000000000000000000000000000, 3.000000000000000000000000000000000 ], true ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

TEST(NormalizeBSONObj, MixedTypeArrayNested) {
    BSONObj bson = fromjson("{a: [true, ['a', [0], true], 'a', 0]}");
    std::string expected = "{ a: [ 0E-6176, \"a\", [ \"a\", [ 0E-6176 ], true ], true ] }";
    ASSERT_EQ(normalizeBSONObj(bson, fullOpts).toString(), expected);
}

// Test that NormalizationOpts are correctly parsed.
TEST(NormalizationOpts, Default) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, 1]}}");
    NormalizationOptsSet opts = NormalizationOpts::kResults;

    std::string expected = "{ a: { c: 1, b: [ 2, 1 ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, NormalizeNumerics) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, 1]}}");
    NormalizationOptsSet opts = NormalizationOpts::kNormalizeNumerics;

    std::string expected =
        "{ a: { c: 1.000000000000000000000000000000000, b: [ 2.000000000000000000000000000000000, "
        "1.000000000000000000000000000000000 ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, SortBSON) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, 1]}}");
    NormalizationOptsSet opts = NormalizationOpts::kSortBSON;

    std::string expected = "{ a: { b: [ 2, 1 ], c: 1 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, ConflateNullAndMissing) {
    BSONObj bson = fromjson("{a: null}");
    NormalizationOptsSet opts = NormalizationOpts::kConflateNullAndMissing;

    std::string expected = "{}";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, ConflateNullAndMissingNested) {
    BSONObj bson = fromjson("{a: {b: null}}");
    NormalizationOptsSet opts = NormalizationOpts::kConflateNullAndMissing;

    std::string expected = "{ a: {} }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, ConflateNullAndMissingNestedObjAndArray) {
    BSONObj bson = fromjson("{a: {c: null, b: [undefined, null, 5]}}");
    NormalizationOptsSet opts = NormalizationOpts::kConflateNullAndMissing;

    std::string expected = "{ a: { b: [ 5 ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, ConflateNullAndMissingNestedObjInArray) {
    BSONObj bson = fromjson("{a: {b: [null, undefined, {d: null}]}}");
    NormalizationOptsSet opts = NormalizationOpts::kConflateNullAndMissing;

    std::string expected = "{ a: { b: [ {} ] } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, SortBSONSortArrays) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, 1]}}");
    NormalizationOptsSet opts = NormalizationOpts::kSortBSON | NormalizationOpts::kSortArrays;

    std::string expected = "{ a: { b: [ 1, 2 ], c: 1 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, NormalizeNumericsSortBSON) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, 1]}}");
    NormalizationOptsSet opts =
        NormalizationOpts::kSortBSON | NormalizationOpts::kNormalizeNumerics;

    std::string expected =
        "{ a: { b: [ 2.000000000000000000000000000000000, 1.000000000000000000000000000000000 ], "
        "c: 1.000000000000000000000000000000000 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, NormalizeFull) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, null, 1, undefined]}}");
    NormalizationOptsSet opts = NormalizationOpts::kNormalizeNumerics |
        NormalizationOpts::kSortBSON | NormalizationOpts::kSortArrays;

    std::string expected =
        "{ a: { b: [ undefined, null, 1.000000000000000000000000000000000, "
        "2.000000000000000000000000000000000 ], "
        "c: 1.000000000000000000000000000000000 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(NormalizationOpts, NormalizeFullConflateNullAndMissing) {
    BSONObj bson = fromjson("{a: {c: 1, b: [2, null, 1, undefined]}}");
    NormalizationOptsSet opts = NormalizationOpts::kNormalizeNumerics |
        NormalizationOpts::kSortBSON | NormalizationOpts::kSortArrays |
        NormalizationOpts::kConflateNullAndMissing;

    std::string expected =
        "{ a: { b: [ 1.000000000000000000000000000000000, 2.000000000000000000000000000000000 ], "
        "c: 1.000000000000000000000000000000000 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}

TEST(DumpUtils, BasicDumpWriteRead) {
    unittest::TempDir tempDir("DumpUtils_BasicDumpWriteRead");

    BSONObj one = fromjson("{foo: 1}");
    BSONObj two = fromjson("{bar: 2}");
    BSONObj three = fromjson("{bam: 3}");
    BSONArray dump = BSON_ARRAY(one << two << three);
    std::vector<BSONObj> expected = {one, two, three};

    // Write our test array
    BSONObj result =
        writeBsonArrayToFile(BSON("a" << tempDir.path() + "/test.bson" << "b" << dump), nullptr);
    ASSERT_EQ(1, result.nFields());
    ASSERT_EQ(BSONType::undefined, result.firstElement().type());

    // Check bulk read
    result = readDumpFile(BSON("a" << tempDir.path() + "/test.bson"), nullptr);
    ASSERT_EQ(1, result.nFields());
    ASSERT_EQ(BSONType::array, result.firstElement().type());
    ASSERT_EQ(3, result.firstElement().Array().size());
    ASSERT_TRUE(one.binaryEqual(result.firstElement().Array().at(0).Obj()));
    ASSERT_TRUE(two.binaryEqual(result.firstElement().Array().at(1).Obj()));
    ASSERT_TRUE(three.binaryEqual(result.firstElement().Array().at(2).Obj()));

    // Check iterative read
    result = numObjsInDumpFile(BSON("a" << tempDir.path() + "/test.bson"), nullptr);
    ASSERT_EQ(1, result.nFields());
    ASSERT_EQ(3, result.firstElement().safeNumberInt());
    for (size_t i = 0; i < expected.size(); ++i) {
        BSONObjBuilder cmdBuilder;
        cmdBuilder.append("a", tempDir.path() + "/test.bson");
        cmdBuilder.appendNumber("b", (int)i);
        result = getObjInDumpFile(cmdBuilder.obj(), nullptr);
        ASSERT_EQ(1, result.nFields());
        ASSERT_TRUE(expected[i].binaryEqual(result.firstElement().Obj()));
    }

    // Cleanup
    result = removeFile(BSON("a" << tempDir.path() + "/test.bson"), nullptr);
    ASSERT_EQ(1, result.nFields());
    ASSERT_TRUE(result.firstElement().booleanSafe());
}

TEST(NormalizationOpts, RoundedFloatingPointNumericsAreEqual) {
    auto bson = fromjson("{a: {z: 1.234567890123456789, y: [9.876543210987654321, 1]}}");
    auto opts = NormalizationOpts::kRoundFloatingPointNumerics | NormalizationOpts::kSortBSON |
        NormalizationOpts::kSortArrays;

    auto expected = "{ a: { y: [ 1, 9.87654321098765 ], z: 1.23456789012346 } }";
    ASSERT_EQ(normalizeBSONObj(bson, opts).toString(), expected);
}


TEST(GetURIFromArgs, BasicHostNameParsing) {

    std::string uri;

    uri = getURIFromArgs("arg"s, "host"s, "port"s);
    ASSERT_EQ(uri, "mongodb://host:port/arg"s);

    uri = getURIFromArgs("arg"s, "host:port"s, ""s);
    ASSERT_EQ(uri, "mongodb://host:port/arg"s);

    uri = getURIFromArgs("arg"s, "host:port"s, "port"s);
    ASSERT_EQ(uri, "mongodb://host:port/arg"s);
}

DEATH_TEST(GetURIFromArgs,
           HostPortDoesNotMatchProvidedPort,
           "connection string bears different port than provided by --port") {
    getURIFromArgs(""s, "host:port1"s, "port2"s);
}

TEST(GetURIFromArgs, ArgHostNames) {

    std::string uri;

    uri = getURIFromArgs("host/db"s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://host:27017/db"s);

    uri = getURIFromArgs("host.db"s, ""s, "port"s);
    ASSERT_EQ(uri, "mongodb://host.db:port/test"s);

    uri = getURIFromArgs("host.db"s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://host.db:27017/test"s);

    uri = getURIFromArgs("host:1234"s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://host:1234/test"s);

    uri = getURIFromArgs("host:1234"s, ""s, "1234"s);
    ASSERT_EQ(uri, "mongodb://host:1234/test"s);
}

DEATH_TEST(GetURIFromArgs,
           FullURIWithHost,
           "If a full URI is provided, you cannot also specify --host or --port") {
    getURIFromArgs("host/port"s, "host"s, ""s);
}

DEATH_TEST(GetURIFromArgs,
           FullURIWithPort,
           "If a full URI is provided, you cannot also specify --host or --port") {
    getURIFromArgs("host/port"s, ""s, "port"s);
}

DEATH_TEST(GetURIFromArgs,
           ArgHostPortDoesNotMatchProvidedPort,
           "connection string bears different port than provided by --port") {
    getURIFromArgs("host:1234"s, ""s, "5678"s);
}

TEST(GetURIFromArgs, MultipleHosts) {

    std::string uri;

    uri = getURIFromArgs("arg"s, "host1,host2,host3"s, ""s);
    ASSERT_EQ(uri, "mongodb://host1:27017,host2:27017,host3:27017/arg"s);

    uri = getURIFromArgs("arg"s, "host1,,host3"s, ""s);
    ASSERT_EQ(uri, "mongodb://host1:27017,host3:27017/arg"s);

    uri = getURIFromArgs("arg"s, "host1,host2"s, "1234"s);
    ASSERT_EQ(uri, "mongodb://host1:1234,host2:1234/arg"s);
}

TEST(GetURIFromArgs, SpecialPrefixes) {

    std::string uri;

    uri = getURIFromArgs("mongodb://dummy"s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://dummy"s);

    uri = getURIFromArgs(""s, "mongodb://dummy"s, ""s);
    ASSERT_EQ(uri, "mongodb://dummy"s);

    uri = getURIFromArgs(""s, "mongodb+srv://dummy"s, ""s);
    ASSERT_EQ(uri, "mongodb+srv://dummy"s);
}

TEST(GetURIFromArgs, ReplSet) {

    std::string uri;

    uri = getURIFromArgs(""s, "replset/host"s, "port"s);
    ASSERT_EQ(uri, "mongodb://host:port/?replicaSet=replset"s);

    uri = getURIFromArgs("arg"s, "replset/host"s, "port"s);
    ASSERT_EQ(uri, "mongodb://host:port/arg?replicaSet=replset"s);
}

TEST(GetURIFromArgs, UnixDomainSocket) {

    std::string uri;

    uri = getURIFromArgs("arg"s, "/unix/domain.sock"s, "port"s);
    ASSERT_EQ(uri, "mongodb://%2Funix%2Fdomain.sock/arg"s);
}


TEST(GetURIFromArgs, IPv6) {

    std::string uri;

    uri = getURIFromArgs("arg"s, "2001::"s, ""s);
    ASSERT_EQ(uri, "mongodb://[2001::]:27017/arg"s);

    uri = getURIFromArgs("arg"s, "[2001::]"s, ""s);
    ASSERT_EQ(uri, "mongodb://[2001::]:27017/arg"s);

    uri = getURIFromArgs("arg"s, "[2001::]:443"s, ""s);
    ASSERT_EQ(uri, "mongodb://[2001::]:443/arg"s);
}

TEST(GetURIFromArgs, LocalHost) {

    std::string uri;

    uri = getURIFromArgs("host"s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://127.0.0.1:27017/host"s);

    uri = getURIFromArgs(""s, ""s, ""s);
    ASSERT_EQ(uri, "mongodb://127.0.0.1:27017"s);

    uri = getURIFromArgs(""s, ""s, "1234"s);
    ASSERT_EQ(uri, "mongodb://127.0.0.1:1234/"s);
}


}  // namespace
}  // namespace mongo
