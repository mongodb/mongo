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

#include "mongo/db/query/compiler/ce/ce_common.h"

#include "mongo/bson/bsonobj.h"
#include "mongo/bson/json.h"
#include "mongo/unittest/death_test.h"
#include "mongo/unittest/unittest.h"


namespace mongo::ce {

TEST(CountNDV, VariousUniqueValues) {
    // "a" has all unique values, "b" has some unique values, "c" has a single unique value, "d" is
    // missing in some documents, "e" is missing in all docs.
    const std::vector<BSONObj> docs = {fromjson("{a:1, b:1, c:1, d: 2}"),
                                       fromjson("{a:2, b:2, c:1}"),
                                       fromjson("{a:3, b:3, c:1}"),
                                       fromjson("{a:4, b:1, c:1}")};
    ASSERT_EQ(4, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(1, countNDV({{.path = "c"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "d"}}, docs));
    ASSERT_EQ(1, countNDV({{.path = "e"}}, docs));
}

TEST(CountNDV, NullAndMissingAreDistinguishedOnlyForExpr) {
    ASSERT_EQ(1, countNDV({{.path = "a"}}, {fromjson("{a:null}"), fromjson("{}")}));
    ASSERT_EQ(2,
              countNDV({{.path = "a", .isExprEq = true}}, {fromjson("{a:null}"), fromjson("{}")}));
}

TEST(CountNDV, DifferentObjectFieldOrdersAreDistinct) {
    ASSERT_EQ(
        2,
        countNDV({{.path = "a"}}, {fromjson("{a: {b: 1, c: 1}}"), fromjson("{a: {c: 1, b: 1}}")}));
}

TEST(CountNDV, DifferentArrayOrdersUnderObjectAreDistinct) {
    ASSERT_EQ(2,
              countNDV({{.path = "a"}},
                       {fromjson("{a: {b: 1, c: [1, 2]}}"), fromjson("{a: {b: 1, c: [2, 1]}}")}));
}

TEST(CountNDV, VariousBSONTypes) {
    const std::vector<BSONObj> docs = {
        // Minkey
        fromjson("{a: {$minKey: 1}}"),

        // EOO
        fromjson("{}"),

        // Null
        fromjson("{a: null}"),

        // Numeric
        fromjson("{a: 0}"),
        fromjson("{a: 1}"),

        // String
        fromjson("{a: ''}"),
        fromjson("{a: '0'}"),
        fromjson("{a: '1'}"),

        // Object
        fromjson("{a: {}}"),
        fromjson("{a: {b: 0}}"),
        fromjson("{a: {b: 0, c: 1}}"),

        // BinData
        fromjson("{a: {$binary: '0000000000000000000000000000', $type: '00'}}"),
        fromjson("{a: {$binary: '0000000000000000000000000000', $type: '01'}}"),

        // OID
        fromjson("{a: ObjectId('507c7f79bcf86cd7994f6c0e')}"),
        fromjson("{a: ObjectId('507c7f79bcf86cd7994f6c01')}"),

        // Boolean
        fromjson("{a: true}"),
        fromjson("{a: false}"),

        // Date
        fromjson("{a: new Date(0)}"),
        fromjson("{a: new Date(1)}"),

        // RegEx
        fromjson("{a: /0/}"),
        fromjson("{a: /1/}"),

        // Code
        fromjson("{a: {$function: {body: 'function() { return 0; }', args: [], lang: 'js'}}}"),
        fromjson("{a: {$function: {body: 'function() { return 1; }', args: [], lang: 'js'}}}"),

        // Timestamp
        fromjson("{a: Timestamp(0,0)}"),
        fromjson("{a: Timestamp(0,1)}"),

        // MaxKey
        fromjson("{a: {$maxKey: 1}}")};

    std::vector<BSONObj> doubleDocs;
    doubleDocs.insert(doubleDocs.end(), docs.begin(), docs.end());
    doubleDocs.insert(doubleDocs.end(), docs.begin(), docs.end());

    // For regular $eq, we treat null & missing the same.
    ASSERT_EQ(docs.size() - 1, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(docs.size() - 1, countNDV({{.path = "a"}}, doubleDocs));

    // For $expr eq, null & missing are treated as being distinct.
    ASSERT_EQ(docs.size(), countNDV({{.path = "a", .isExprEq = true}}, docs));
    ASSERT_EQ(docs.size(), countNDV({{.path = "a", .isExprEq = true}}, doubleDocs));
}

TEST(CountNDV, NestedField) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1}"),
                                       fromjson("{a: {b: {c: 1}}}"),
                                       fromjson("{a: {b: {c: 1}, c: 3}}"),
                                       fromjson("{a: {b: {c: 2}}}"),
                                       fromjson("{a: {c: [1,2,3]}}")};
    ASSERT_EQ(5, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "a.b"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "a.b.c"}}, docs));
}

TEST(CountNDV, NestedFieldNull) {
    const std::vector<BSONObj> docs = {fromjson("{}"),
                                       fromjson("{a: null}"),
                                       fromjson("{a: {}}"),
                                       fromjson("{a: {b: null}}"),
                                       fromjson("{a: {b: {}}}"),
                                       fromjson("{a: {b: {c: null}}}")};
    // Regular eq semantics.
    ASSERT_EQ(5, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "a.b"}}, docs));
    ASSERT_EQ(1, countNDV({{.path = "a.b.c"}}, docs));

    // Regular $expr eq semantics.
    ASSERT_EQ(6, countNDV({{.path = "a", .isExprEq = true}}, docs));
    ASSERT_EQ(4, countNDV({{.path = "a.b", .isExprEq = true}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "a.b.c", .isExprEq = true}}, docs));
}

TEST(CountNDV, DifferentNumericTypesAreNotDistinguished) {
    ASSERT_EQ(1,
              countNDV({{.path = "a"}},
                       {fromjson("{a: 1}"),
                        fromjson("{a: NumberLong(1)}"),
                        fromjson("{a: NumberInt(1)}"),
                        fromjson("{a: NumberDecimal('1.00000')}")}));
}

DEATH_TEST(CountNDVDeathTest, ThrowsOnEmptyArray, "unexpected array in NDV computation") {
    countNDV({{.path = "a"}}, {fromjson("{a: []}")});
}

DEATH_TEST(CountNDVDeathTest, ThrowsOnNonEmptyArray, "unexpected array in NDV computation") {
    countNDV({{.path = "a"}}, {fromjson("{a: [1, 2, 3]}")});
}

DEATH_TEST(CountNDVDeathTest, ThrowsOnNestedEmptyArray, "unexpected array in NDV computation") {
    countNDV({{.path = "a.b"}}, {fromjson("{a: {b: []}}")});
}

DEATH_TEST(CountNDVDeathTest, ThrowsOnNestedArray, "unexpected array in NDV computation") {
    countNDV({{.path = "a.b"}}, {fromjson("{a: {b: [1, 2, 3]}}")});
}

DEATH_TEST(CountNDVDeathTest,
           ThrowsOnEmptyArrayInTraversal,
           "unexpected array in NDV computation") {
    countNDV({{.path = "a.b"}}, {fromjson("{a: []}")});
}

DEATH_TEST(CountNDVDeathTest, ThrowsOnArrayInTraversal, "unexpected array in NDV computation") {
    countNDV({{.path = "a.b"}}, {fromjson("{a: [{b: 1}]}")});
}

TEST(CountNDV, BasicMultiField) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1, b: 1}"),
                                       fromjson("{a: 1, b: 2}"),
                                       fromjson("{a: 2, b: 2}"),
                                       fromjson("{a: 2, b: 2}"),
                                       fromjson("{a: 3}"),
                                       fromjson("{c: 3}"),
                                       fromjson("{}")};
    ASSERT_EQ(4, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "a"}, {.path = "b"}}, docs));
}

TEST(CountNDV, MultiFieldManyFields) {
    const std::vector<BSONObj> docs = {
        // Note: different orders of fields in these documents.
        fromjson("{a: 1, b: 1, c: 1, d: 1, e: 1}"),
        fromjson("{b: 1, c: 1, a: 1, d: 1, e: 1}"),
        // Note: different orders of fields in these documents.
        fromjson("{a: 1, b: 1, c: 1, d: 1, e: 2}"),
        fromjson("{d: 1, e: 2, a: 1, b: 1, c: 1}"),
        // Note: different orders of fields in these documents.
        fromjson("{a: 1, b: 2, c: 2, d: 2, e: 2}"),
        fromjson("{e: 2, c: 2, b: 2, d: 2, a: 1}"),
    };
    ASSERT_EQ(1, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "c"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "d"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "e"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "a"}, {.path = "b"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "b"}, {.path = "c"}}, docs));
    ASSERT_EQ(3,
              countNDV({{.path = "a"}, {.path = "b"}, {.path = "c"}, {.path = "d"}, {.path = "e"}},
                       docs));
}

TEST(CountNDV, MultiFieldOrderInsensitive) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1, b: 1}"),
                                       fromjson("{b: 1, a: 1}"),
                                       fromjson("{a: 1, b: 2}"),
                                       fromjson("{b: 2, a: 1}"),
                                       fromjson("{a: 2, b: 2}"),
                                       fromjson("{b: 2, a: 2}")};
    ASSERT_EQ(2, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "a"}, {.path = "b"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "b"}, {.path = "a"}}, docs));
}

TEST(CountNDV, MultiFieldNullMissing) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1}"),
                                       fromjson("{b: 1}"),
                                       fromjson("{a: null}"),
                                       fromjson("{b: null}"),
                                       fromjson("{a: null, b: 1}"),
                                       fromjson("{a: 1, b: null}"),
                                       fromjson("{a: null, b: null}"),
                                       fromjson("{}")};
    // No $expr: count null/missing as the same.
    ASSERT_EQ(2, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "a"}, {.path = "b"}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "b"}, {.path = "a"}}, docs));

    // Only "a" in $expr; treat null/missing as different for "a".
    ASSERT_EQ(3, countNDV({{.path = "a", .isExprEq = true}}, docs));
    ASSERT_EQ(2, countNDV({{.path = "b"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "a", .isExprEq = true}, {.path = "b"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "b"}, {.path = "a", .isExprEq = true}}, docs));

    // Both $expr. Treat null/missing as different for both.
    ASSERT_EQ(3, countNDV({{.path = "a", .isExprEq = true}}, docs));
    ASSERT_EQ(3, countNDV({{.path = "b", .isExprEq = true}}, docs));
    ASSERT_EQ(8,
              countNDV({{.path = "a", .isExprEq = true}, {.path = "b", .isExprEq = true}}, docs));
    ASSERT_EQ(8,
              countNDV({{.path = "b", .isExprEq = true}, {.path = "a", .isExprEq = true}}, docs));
}

TEST(CountNDV, MultiFieldDuplicateAndNestedFields) {
    const std::vector<BSONObj> docs = {
        fromjson("{a: 1, b: 1}"),
        fromjson("{a: 1, b: 2}"),
        fromjson("{a: 2, b: 1}"),
        fromjson("{a: {b: 10}}"),
        fromjson("{a: {b: 20}}"),
        fromjson("{a: {b: 20}}"),
        fromjson("{b: {b: 10}}"),
    };
    ASSERT_EQ(5, countNDV({{.path = "a"}}, docs));
    ASSERT_EQ(4, countNDV({{.path = "b"}}, docs));

    // Arguably these queries don't make the most sense, but we handle them correctly.
    ASSERT_EQ(5, countNDV({{.path = "a"}, {.path = "a"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "a"}, {.path = "a.b"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "a.b"}, {.path = "a"}}, docs));
    ASSERT_EQ(5, countNDV({{.path = "a.b"}, {.path = "b"}}, docs));
}
}  // namespace mongo::ce
