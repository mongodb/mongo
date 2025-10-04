/**
 *    Copyright (C) 2018-present MongoDB, Inc.
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

#include "mongo/db/pipeline/document_path_support.h"

#include "mongo/base/error_codes.h"
#include "mongo/base/string_data.h"
#include "mongo/bson/bsonmisc.h"
#include "mongo/bson/json.h"
#include "mongo/bson/util/builder.h"
#include "mongo/db/exec/document_value/document.h"
#include "mongo/db/exec/document_value/document_value_test_util.h"
#include "mongo/db/exec/document_value/value.h"
#include "mongo/db/exec/document_value/value_comparator.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/unittest/unittest.h"
#include "mongo/util/assert_util.h"

#include <cstddef>
#include <string>
#include <vector>

namespace mongo {
namespace document_path_support {

namespace {
using std::vector;

const ValueComparator kDefaultValueComparator{};

TEST(VisitAllValuesAtPathTest, NestedObjectWithScalarValue) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Document{{"b", 1}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithEmptyArrayValue) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Document{{"b", vector<Value>{}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithSingletonArrayValue) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Document{{"b", vector{1}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithArrayValue) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Document{{"b", vector{1, 2, 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithArrayOfSubobjectsWithScalarValue) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{
        {"a", vector<Document>{Document{{"b", 1}}, Document{{"b", 2}}, Document{{"b", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithArrayOfSubobjectsWithArrayValues) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a",
                  vector<Document>{Document{{"b", vector{1, 2}}},
                                   Document{{"b", vector{2, 3}}},
                                   Document{{"b", vector{3, 1}}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithTwoDimensionalArrayOfSubobjects) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc(fromjson("{a: [[{b: 0}, {b: 1}], [{b: 2}, {b: 3}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithDiverseStructure) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc(
        fromjson("{a: ["
                 "     {b: 0},"
                 "     [{b: 1}, {b: {c: -1}}],"
                 "     'no b here!',"
                 "     {b: [{c: -2}, 'no c here!']},"
                 "     {b: {c: [-3, -4]}}"
                 "]}"));
    visitAllValuesAtPath(doc, FieldPath("a.b.c"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(-2)), 1UL);
    ASSERT_EQ(values.count(Value(-3)), 1UL);
    ASSERT_EQ(values.count(Value(-4)), 1UL);
}

TEST(VisitAllValuesAtPathTest, AcceptsNumericFieldNames) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Document{{"0", 1}}}};
    visitAllValuesAtPath(doc, FieldPath("a.0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, UsesNumericFieldNameToExtractElementFromArray) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"0", 1}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, TreatsNegativeIndexAsFieldName) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {0, 1, Document{{"-1", "target"_sd}}, Document{{"b", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.-1"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value("target"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ExtractsNoValuesFromOutOfBoundsIndex) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"b", 2}}, Document{{"10", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.10"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotTreatHexStringAsIndexSpecification) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"0x2", 2}}, Document{{"NOT THIS ONE", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.0x2"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptLeadingPlusAsArrayIndex) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"+2", 2}}, Document{{"NOT THIS ONE", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.+2"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptTrailingCharactersForArrayIndex) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"2xyz", 2}}, Document{{"NOT THIS ONE", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.2xyz"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptNonDigitsForArrayIndex) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"2x4", 2}}, Document{{"NOT THIS ONE", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.2x4"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest,
     DoesExtractNestedValuesFromWithinArraysTraversedWithPositionalPaths) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Document{{"2", 2}}, Document{{"target", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.2.target"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesExpandMultiplePositionalPathSpecifications) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.1.0.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value("(1, 0)"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesAcceptNumericInitialField) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", 1}, {"0", 2}};
    visitAllValuesAtPath(doc, FieldPath("0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesExpandArrayFoundAfterPositionalSpecification) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.1.b"), callback);
    ASSERT_EQ(values.size(), 2UL);
    ASSERT_EQ(values.count(Value("(1, 0)"_sd)), 1UL);
    ASSERT_EQ(values.count(Value("(1, 1)"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAddMissingValueToResults) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", Value()}};
    visitAllValuesAtPath(doc, FieldPath("a"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAddMissingValueWithinArrayToResults) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    Document doc{{"a", {1, Value(), 2}}};
    visitAllValuesAtPath(doc, FieldPath("a"), callback);
    ASSERT_EQ(values.size(), 2UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, StrictNumericFields) {
    auto values = kDefaultValueComparator.makeFlatUnorderedValueSet();
    auto callback = [&values](const Value& val) {
        values.insert(val);
    };
    {
        Document doc(fromjson("{a: [[], [{b: [3]}, {b: {\"00\": 2}}]]}"));
        visitAllValuesAtPath(doc, FieldPath("a.1.b.00"), callback);
        // We only find 2.
        ASSERT_EQ(values.size(), 1UL);
        ASSERT_EQ(values.count(Value(2)), 1UL);
    }
    {
        // Test a 0-prefixed case other than "00".
        Document doc(fromjson("{a: [{b: [0, 1]}, {b: {\"01\": 2}}]}"));
        visitAllValuesAtPath(doc, FieldPath("a.b.01"), callback);
        ASSERT_EQ(values.size(), 1UL);
        ASSERT_EQ(values.count(Value(2)), 1UL);
    }
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractTopLevelFieldIfDottedFieldNeeded) {
    Document input(fromjson("{a: 1, b: {c: 1, d: 1}}"));
    BSONObj expected = fromjson("{b: {c: 1, d: 1}}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"b.c"}));
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractEntireArray) {
    Document input(fromjson("{a: [1, 2, 3], b: 1}"));
    BSONObj expected = fromjson("{a: [1, 2, 3]}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"a"}));
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractEntireArrayWithNumericPathComponent) {
    Document input(fromjson("{a: [1, 2, 3], b: 1}"));
    BSONObj expected = fromjson("{a: [1, 2, 3]}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"a.0"}));
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractEntireObjectWithNumericPathComponent) {
    Document input(fromjson("{a: {'0': 2, c: 3}, b: 1}"));
    BSONObj expected = fromjson("{a: {'0': 2, c: 3}}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"a.0"}));
}

TEST(DocumentToBsonWithPathsTest, ShouldOnlyAddPrefixedFieldOnceIfTwoDottedSubfields) {
    Document input(fromjson("{a: 1, b: {c: 1, f: {d: {e: 1}}}}"));
    BSONObj expected = fromjson("{b: {c: 1, f: {d: {e: 1}}}}");
    ASSERT_BSONOBJ_EQ(expected,
                      document_path_support::documentToBsonWithPaths(input, {"b.f", "b.f.d.e"}));
}

TEST(DocumentToBsonWithPathsTest, MissingFieldShouldNotAppearInResult) {
    Document input(fromjson("{a: 1}"));
    BSONObj expected;
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"b", "c"}));
}

TEST(DocumentToBsonWithPathsTest, ShouldSerializeNothingIfNothingIsNeeded) {
    Document input(fromjson("{a: 1, b: {c: 1}}"));
    BSONObj expected;
    ASSERT_BSONOBJ_EQ(expected,
                      document_path_support::documentToBsonWithPaths(input, OrderedPathSet{}));
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractEntireArrayFromPrefixOfDottedField) {
    Document input(fromjson("{a: [{b: 1}, {b: 2}], c: 1}"));
    BSONObj expected = fromjson("{a: [{b: 1}, {b: 2}]}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"a.b"}));
}

TEST(DocumentToBsonWithPathsTest, SizeTraits) {
    constexpr size_t longStringLength = 9 * 1024 * 1024;
    static_assert(longStringLength <= BSONObjMaxInternalSize &&
                  2 * longStringLength > BSONObjMaxInternalSize &&
                  2 * longStringLength <= BufferMaxSize);
    std::string longString(longStringLength, 'A');
    MutableDocument md;
    md.addField("a", Value(longString));
    md.addField("b", Value(longString));
    ASSERT_DOES_NOT_THROW(document_path_support::documentToBsonWithPaths(md.peek(), {"a"}));
    ASSERT_THROWS_CODE(document_path_support::documentToBsonWithPaths(md.peek(), {"a", "b"}),
                       DBException,
                       ErrorCodes::BSONObjectTooLarge);
    ASSERT_THROWS_CODE(document_path_support::documentToBsonWithPaths<BSONObj::DefaultSizeTrait>(
                           md.peek(), {"a", "b"}),
                       DBException,
                       ErrorCodes::BSONObjectTooLarge);
    ASSERT_DOES_NOT_THROW(document_path_support::documentToBsonWithPaths<BSONObj::LargeSizeTrait>(
        md.peek(), {"a", "b"}));
}
}  // namespace
}  // namespace document_path_support
}  // namespace mongo
