/**
 * Copyright (C) 2016 MongoDB Inc.
 *
 * This program is free software: you can redistribute it and/or  modify
 * it under the terms of the GNU Affero General Public License, version 3,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, the copyright holders give permission to link the
 * code of portions of this program with the OpenSSL library under certain
 * conditions as described in each individual source file and distribute
 * linked combinations including the program with the OpenSSL library. You
 * must comply with the GNU Affero General Public License in all respects
 * for all of the code used other than as permitted herein. If you modify
 * file(s) with this exception, you may extend this exception to your
 * version of the file(s), but you are not obligated to do so. If you do not
 * wish to do so, delete this exception statement from your version. If you
 * delete this exception statement from all source files in the program,
 * then also delete it in the license file.
 */

#include "mongo/platform/basic.h"

#include <boost/optional.hpp>
#include <vector>

#include "mongo/base/string_data.h"
#include "mongo/bson/json.h"
#include "mongo/db/pipeline/document.h"
#include "mongo/db/pipeline/document_comparator.h"
#include "mongo/db/pipeline/document_path_support.h"
#include "mongo/db/pipeline/document_value_test_util.h"
#include "mongo/db/pipeline/field_path.h"
#include "mongo/db/pipeline/value.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace document_path_support {

namespace {
using std::vector;

const ValueComparator kDefaultValueComparator{};

TEST(VisitAllValuesAtPathTest, NestedObjectWithScalarValue) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Document{{"b", 1}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithEmptyArrayValue) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Document{{"b", vector<Value>{}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithSingletonArrayValue) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Document{{"b", vector<Value>{Value(1)}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, NestedObjectWithArrayValue) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Document{{"b", vector<Value>{Value(1), Value(2), Value(3)}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithArrayOfSubobjectsWithScalarValue) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{
        {"a", vector<Document>{Document{{"b", 1}}, Document{{"b", 2}}, Document{{"b", 3}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithArrayOfSubobjectsWithArrayValues) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a",
                  vector<Document>{Document{{"b", vector<Value>{Value(1), Value(2)}}},
                                   Document{{"b", vector<Value>{Value(2), Value(3)}}},
                                   Document{{"b", vector<Value>{Value(3), Value(1)}}}}}};
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 3UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithTwoDimensionalArrayOfSubobjects) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc(fromjson("{a: [[{b: 0}, {b: 1}], [{b: 2}, {b: 3}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.b"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, ObjectWithDiverseStructure) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
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
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Document{{"0", 1}}}};
    visitAllValuesAtPath(doc, FieldPath("a.0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, UsesNumericFieldNameToExtractElementFromArray) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", vector<Value>{Value(1), Value(Document{{"0", 1}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
}

TEST(VisitAllValuesAtPathTest, TreatsNegativeIndexAsFieldName) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{
        {"a",
         vector<Value>{
             Value(0), Value(1), Value(Document{{"-1", "target"_sd}}), Value(Document{{"b", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.-1"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value("target"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, ExtractsNoValuesFromOutOfBoundsIndex) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{
        {"a", vector<Value>{Value(1), Value(Document{{"b", 2}}), Value(Document{{"10", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.10"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotTreatHexStringAsIndexSpecification) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"0x2", 2}}),
                                Value(Document{{"NOT THIS ONE", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.0x2"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptLeadingPlusAsArrayIndex) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a",
                  vector<Value>{
                      Value(1), Value(Document{{"+2", 2}}), Value(Document{{"NOT THIS ONE", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.+2"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptTrailingCharactersForArrayIndex) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"2xyz", 2}}),
                                Value(Document{{"NOT THIS ONE", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.2xyz"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAcceptNonDigitsForArrayIndex) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a",
                  vector<Value>{Value(1),
                                Value(Document{{"2x4", 2}}),
                                Value(Document{{"NOT THIS ONE", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.2x4"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest,
     DoesExtractNestedValuesFromWithinArraysTraversedWithPositionalPaths) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{
        {"a", vector<Value>{Value(1), Value(Document{{"2", 2}}), Value(Document{{"target", 3}})}}};
    visitAllValuesAtPath(doc, FieldPath("a.2.target"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(3)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesExpandMultiplePositionalPathSpecifications) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.1.0.b"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value("(1, 0)"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesAcceptNumericInitialField) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", 1}, {"0", 2}};
    visitAllValuesAtPath(doc, FieldPath("0"), callback);
    ASSERT_EQ(values.size(), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesExpandArrayFoundAfterPositionalSpecification) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc(fromjson("{a: [[{b: '(0, 0)'}, {b: '(0, 1)'}], [{b: '(1, 0)'}, {b: '(1, 1)'}]]}"));
    visitAllValuesAtPath(doc, FieldPath("a.1.b"), callback);
    ASSERT_EQ(values.size(), 2UL);
    ASSERT_EQ(values.count(Value("(1, 0)"_sd)), 1UL);
    ASSERT_EQ(values.count(Value("(1, 1)"_sd)), 1UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAddMissingValueToResults) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", Value()}};
    visitAllValuesAtPath(doc, FieldPath("a"), callback);
    ASSERT_EQ(values.size(), 0UL);
}

TEST(VisitAllValuesAtPathTest, DoesNotAddMissingValueWithinArrayToResults) {
    auto values = kDefaultValueComparator.makeUnorderedValueSet();
    auto callback = [&values](const Value& val) { values.insert(val); };
    Document doc{{"a", vector<Value>{Value(1), Value(), Value(2)}}};
    visitAllValuesAtPath(doc, FieldPath("a"), callback);
    ASSERT_EQ(values.size(), 2UL);
    ASSERT_EQ(values.count(Value(1)), 1UL);
    ASSERT_EQ(values.count(Value(2)), 1UL);
}

TEST(ExtractElementAlongNonArrayPathTest, ReturnsMissingIfPathDoesNotExist) {
    Document doc{{"a", 1}, {"b", 2}};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"c.d"});
    ASSERT_OK(result.getStatus());
    ASSERT_VALUE_EQ(result.getValue(), Value{});
}

TEST(ExtractElementAlongNonArrayPathTest, ReturnsMissingIfPathPartiallyExists) {
    Document doc{fromjson("{a: {b: {c: 1}}}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b.c.d"});
    ASSERT_OK(result.getStatus());
    ASSERT_VALUE_EQ(result.getValue(), Value{});
}

TEST(ExtractElementAlongNonArrayPathTest, ReturnsValueIfPathExists) {
    Document doc{fromjson("{a: {b: {c: {d: {e: 1}}}}}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b.c.d"});
    ASSERT_OK(result.getStatus());
    ASSERT_VALUE_EQ(result.getValue(), Value{BSON("e" << 1)});
}

TEST(ExtractElementAlongNonArrayPathTest, FailsIfPathTerminatesAtEmptyArray) {
    Document doc{fromjson("{a: {b: {c: {d: []}}}}}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b.c.d"});
    ASSERT_EQ(result.getStatus(), ErrorCodes::InternalError);
}

TEST(ExtractElementAlongNonArrayPathTest, FailsIfPathTerminatesAtNonEmptyArray) {
    Document doc{fromjson("{a: {b: {c: {d: [1, 2, 3]}}}}}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b.c.d"});
    ASSERT_EQ(result.getStatus(), ErrorCodes::InternalError);
}

TEST(ExtractElementAlongNonArrayPathTest, FailsIfPathContainsArray) {
    Document doc{fromjson("{a: {b: [{c: {d: 1}}]}}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b.c.d"});
    ASSERT_EQ(result.getStatus(), ErrorCodes::InternalError);
}

TEST(ExtractElementAlongNonArrayPathTest, FailsIfFirstPathComponentIsArray) {
    Document doc{fromjson("{a: [1, 2, {b: 1}]}")};
    auto result = extractElementAlongNonArrayPath(doc, FieldPath{"a.b"});
    ASSERT_EQ(result.getStatus(), ErrorCodes::InternalError);
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
    ASSERT_BSONOBJ_EQ(
        expected, document_path_support::documentToBsonWithPaths(input, std::set<std::string>{}));
}

TEST(DocumentToBsonWithPathsTest, ShouldExtractEntireArrayFromPrefixOfDottedField) {
    Document input(fromjson("{a: [{b: 1}, {b: 2}], c: 1}"));
    BSONObj expected = fromjson("{a: [{b: 1}, {b: 2}]}");
    ASSERT_BSONOBJ_EQ(expected, document_path_support::documentToBsonWithPaths(input, {"a.b"}));
}

}  // namespace
}  // namespace document_path_support
}  // namespace mongo
