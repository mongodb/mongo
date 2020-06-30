/**
 *    Copyright (C) 2020-present MongoDB, Inc.
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

#include "mongo/platform/basic.h"

#include "mongo/bson/json.h"
#include "mongo/db/update/document_diff_calculator.h"
#include "mongo/unittest/unittest.h"

namespace mongo {
namespace {

TEST(DocumentDiffTest, SameObjectsNoDiff) {
    auto assertDiffEmpty = [](const BSONObj& doc) {
        auto diff = doc_diff::computeDiff(doc, doc);
        ASSERT(diff);
        ASSERT_BSONOBJ_BINARY_EQ(*diff, BSONObj());
    };
    assertDiffEmpty(fromjson("{field1: 1}"));
    assertDiffEmpty(fromjson("{field1: []}"));
    assertDiffEmpty(fromjson("{field1: [{}]}"));
    assertDiffEmpty(fromjson("{field1: [0, 1, 2, 3, 4]}"));
    assertDiffEmpty(fromjson("{field1: null}"));
    assertDiffEmpty(fromjson("{'0': [0]}"));
    assertDiffEmpty(fromjson("{'0': [[{'0': [[{'0': [[]]} ]]}]]}"));
}

TEST(DocumentDiffTest, LargeDelta) {
    ASSERT_FALSE(
        doc_diff::computeDiff(fromjson("{a: 1, b: 2, c: 3}"), fromjson("{A: 1, B: 2, C: 3}")));

    // Inserting field at the beginning produces a large delta.
    ASSERT_FALSE(doc_diff::computeDiff(fromjson("{b: 1, c: 1, d: 1}"),
                                       fromjson("{a: 1, b: 1, c: 1, d: 1}")));

    // Empty objects.
    ASSERT_FALSE(doc_diff::computeDiff(BSONObj(), BSONObj()));

    // Full diff.
    ASSERT_FALSE(doc_diff::computeDiff(fromjson("{b: 1}"), BSONObj()));
    ASSERT_FALSE(doc_diff::computeDiff(BSONObj(), fromjson("{b: 1}")));
}

TEST(DocumentDiffTest, DeleteAndInsertFieldAtTheEnd) {
    auto diff = doc_diff::computeDiff(fromjson("{a: 1, b: 2, c: 3, d: 4}"),
                                      fromjson("{b: 2, c: 3, d: 4, a: 1}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{i: {a: 1}}"));
}

TEST(DocumentDiffTest, DeletesRecordedInAscendingOrderOfFieldNames) {
    auto diff = doc_diff::computeDiff(fromjson("{b: 1, a: 2, c: 3, d: 4, e: 'value'}"),
                                      fromjson("{c: 3, d: 4, e: 'value'}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{d: {a: false, b: false}}"));

    // Delete at the end still follow the order.
    diff = doc_diff::computeDiff(
        fromjson("{b: 1, a: 2, c: 'value', d: 'value', e: 'value', g: 1, f: 1}"),
        fromjson("{c: 'value', d: 'value', e: 'value'}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{d: {g: false, f: false, a: false, b: false}}"));
}

TEST(DocumentDiffTest, DataTypeChangeRecorded) {
    const auto objWithDoubleValue =
        fromjson("{a: 1, b: 2, c: {subField1: 1, subField2: 3.0}, d: 4}");
    const auto objWithIntValue = fromjson("{a: 1, b: 2, c: {subField1: 1, subField2: 3}, d: 4}");
    const auto objWithLongValue =
        fromjson("{a: 1, b: 2, c: {subField1: 1, subField2: NumberLong(3)}, d: 4}");
    auto diff = doc_diff::computeDiff(objWithDoubleValue, objWithIntValue);
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {c: {u: {subField2: 3}} }}"));

    diff = doc_diff::computeDiff(objWithIntValue, objWithLongValue);
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {c: {u: {subField2: NumberLong(3)}} }}"));

    diff = doc_diff::computeDiff(objWithLongValue, objWithDoubleValue);
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {c: {u: {subField2: 3.0}} }}"));
}

TEST(DocumentDiffTest, NullAndMissing) {
    auto diff = doc_diff::computeDiff(fromjson("{a: null}"), fromjson("{}"));
    ASSERT_FALSE(diff);

    diff = doc_diff::computeDiff(fromjson("{a: null, b: undefined, c: null, d: 'someVal'}"),
                                 fromjson("{a: null, b: undefined, c: undefined, d: 'someVal'}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{u: {c: undefined}}"));
}

TEST(DocumentDiffTest, FieldOrder) {
    auto diff = doc_diff::computeDiff(fromjson("{a: 1, b: 2, c: {p: 1, q: 1, s: 1, r: 2}, d: 4}"),
                                      fromjson("{a: 1, b: 2, c: {p: 1, q: 1, r: 2, s: 1}, d: 4}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {c: {i: {s: 1}} }}"));
}

TEST(DocumentDiffTest, SimpleArrayPush) {
    auto diff = doc_diff::computeDiff(fromjson("{field1: 'abcd', field2: [1, 2, 3]}"),
                                      fromjson("{field1: 'abcd', field2: [1, 2, 3, 4]}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {field2: {a: true, 'u3': 4}}}"));
}

TEST(DocumentDiffTest, NestedArray) {
    auto diff = doc_diff::computeDiff(fromjson("{field1: 'abcd', field2: [1, 2, 3, [[2]]]}"),
                                      fromjson("{field1: 'abcd', field2: [1, 2, 3, [[4]]]}"));
    ASSERT(diff);
    // When the sub-array delta is larger than the size of the sub-array, we record it as an update
    // operation.
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {field2: {a: true, 'u3': [[4]]}}}"));

    diff = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [2], 4, 5, 6], 5, 5, 5]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, [1, 'longString', [4], 4], 5, 6]}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *diff,
        fromjson("{s: {field2: {a: true, l: 6, 's3': {a: true, l: 4, 'u2': [4]}, 'u5': 6}}}"));
}

TEST(DocumentDiffTest, SubObjInSubArrayUpdateElements) {
    auto diff = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, "
                 "{field3: ['veryLargeStringValueveryLargeStringValue', 2, 3, 4]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': "
                 "['veryLargeStringValueveryLargeStringValue', 2, 4, 3, "
                 "5]}]}"));

    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *diff,
        fromjson("{s: {field2: {a: true, 's3': {s: {field3: {a: true, 'u2': 4, 'u3': 3, "
                 "'u4': 5}} }} }}"));
}

TEST(DocumentDiffTest, SubObjInSubArrayDeleteElements) {
    auto diff = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 4, 5]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {'field3': ['largeString', 2, 3, 5]}]}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *diff,
        fromjson("{s: {field2: {a: true, 's3': {s: {field3: {a: true, l: 4, 'u3': 5}} }} }}"));
}

TEST(DocumentDiffTest, NestedSubObjs) {
    auto diff = doc_diff::computeDiff(
        fromjson("{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {p: 1}, "
                 "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: 3}"),
        fromjson("{level1Field1: 'abcd', level1Field2: {level2Field1: {level3Field1: {q: 1}, "
                 "level3Field2: {q: 1}}, level2Field2: 2}, level1Field3: '3'}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *diff,
        fromjson("{u: {level1Field3: '3'}, s: {level1Field2: {s: {'level2Field1': "
                 "{u: {level3Field1: {q: 1}}} }} }}"));
}

TEST(DocumentDiffTest, SubArrayInSubArrayLargeDelta) {
    auto diff = doc_diff::computeDiff(
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [2, 3, 4, 5]}]}"),
        fromjson("{field1: 'abcd', field2: [1, 2, 3, {field3: [1, 2, 3, 4, 5]}]}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(
        *diff, fromjson("{s: {field2: {a: true, 'u3': {field3: [1, 2, 3, 4, 5]}} }}"));
}

TEST(DocumentDiffTest, SubObjInSubArrayLargeDelta) {
    auto diff =
        doc_diff::computeDiff(fromjson("{field1: [1, 2, 3, 4, 5, 6, {a: 1, b: 2, c: 3, d: 4}, 7]}"),
                              fromjson("{field1: [1, 2, 3, 4, 5, 6, {p: 1, q: 2}, 7]}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {field1: {a: true, 'u6': {p: 1, q: 2}} }}"));
}

TEST(DocumentDiffTest, SubObjInSubObjLargeDelta) {
    auto diff = doc_diff::computeDiff(
        fromjson("{field: {p: 'someString', q: 2, r: {a: 1, b: 2, c: 3, 'd': 4}, s: 3}}"),
        fromjson("{field: {p: 'someString', q: 2, r: {p: 1, q: 2}, s: 3}}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {field: {u: {r: {p: 1, q: 2} }} }}"));
}

TEST(DocumentDiffTest, SubArrayInSubObjLargeDelta) {
    auto diff =
        doc_diff::computeDiff(fromjson("{field: {p: 'someString', q: 2, r: [1, 3, 4, 5], s: 3}}"),
                              fromjson("{field: {p: 'someString', q: 2, r: [1, 2, 3, 4], s: 3}}"));
    ASSERT(diff);
    ASSERT_BSONOBJ_BINARY_EQ(*diff, fromjson("{s: {field: {u: {r: [1, 2, 3, 4]}} }}"));
}

}  // namespace
}  // namespace mongo
