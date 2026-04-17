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
#include "mongo/db/query/compiler/physical_model/index_bounds/index_bounds.h"
#include "mongo/db/query/compiler/physical_model/interval/interval.h"
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

// Build a single closed-interval OIL [lo, hi].
OrderedIntervalList makeOIL(int lo, int hi) {
    OrderedIntervalList oil;
    oil.intervals.emplace_back(BSON("" << lo << "" << hi), true, true);
    return oil;
}

// Build a point OIL [v, v].
OrderedIntervalList makePointOIL(int v) {
    return makeOIL(v, v);
}

// Build an OIL that matches null (and missing, under $eq semantics).
OrderedIntervalList makeNullOIL() {
    OrderedIntervalList oil;
    oil.intervals.emplace_back(BSON("" << BSONNULL << "" << BSONNULL), true, true);
    return oil;
}


TEST(CountNDV, BoundsPointFilter) {
    const std::vector<BSONObj> docs = {
        fromjson("{a: 1}"), fromjson("{a: 2}"), fromjson("{a: 3}"), fromjson("{a: 2}")};
    // Only value 2 is a matching key.
    const std::vector<OrderedIntervalList> bounds = {makePointOIL(2)};
    ASSERT_EQ(1, countNDV({{.path = "a"}}, docs, std::span{bounds}));
}

TEST(CountNDV, BoundsRangeFilter) {
    const std::vector<BSONObj> docs = {
        fromjson("{a: 1}"), fromjson("{a: 2}"), fromjson("{a: 3}"), fromjson("{a: 4}")};
    // Values 2 and 3 are in [2, 3].
    const std::vector<OrderedIntervalList> bounds = {makeOIL(2, 3)};
    ASSERT_EQ(2, countNDV({{.path = "a"}}, docs, std::span{bounds}));
}

TEST(CountNDV, BoundsFilterAllOut) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1}"), fromjson("{a: 2}")};
    // No document matches [10, 20].
    const std::vector<OrderedIntervalList> bounds = {makeOIL(10, 20)};
    ASSERT_EQ(0, countNDV({{.path = "a"}}, docs, std::span{bounds}));
}

TEST(CountNDV, BoundsMultiField) {
    const std::vector<BSONObj> docs = {fromjson("{a: 1, b: 1}"),
                                       fromjson("{a: 1, b: 2}"),
                                       fromjson("{a: 2, b: 1}"),
                                       fromjson("{a: 2, b: 2}")};
    // Restrict a to [1,1] and b to [1,2] -- all four docs match, but a=1 is fixed so NDV of
    // compound key is: (1,1) and (1,2) = 2.
    const std::vector<OrderedIntervalList> bounds = {makePointOIL(1), makeOIL(1, 2)};
    ASSERT_EQ(2, countNDV({{.path = "a"}, {.path = "b"}}, docs, std::span{bounds}));
}

TEST(CountNDV, BoundsNullMatchesMissingField) {
    // Docs with missing "b" should match null bounds under $eq semantics.
    const std::vector<BSONObj> docs = {
        fromjson("{a: 1}"),           // b missing -> null
        fromjson("{a: 2}"),           // b missing -> null
        fromjson("{a: 3, b: null}"),  // b explicitly null
        fromjson("{a: 4, b: 1}"),     // b = 1, filtered out
    };
    // Bound on b = [null, null]: the three docs with null/missing b all collapse to one NDV.
    const std::vector<OrderedIntervalList> bounds = {makeNullOIL()};
    ASSERT_EQ(1, countNDV({{.path = "b"}}, docs, std::span{bounds}));
}

TEST(CountNDVMultiKey, SingleArrayField) {
    // All distinct elements.
    {
        auto r = countNDVMultiKey({{.path = "a"}}, {fromjson("{a: [1, 2, 3]}")});
        ASSERT_EQ(3, r.totalSampleKeys);
        ASSERT_EQ(3, r.sampleUniqueKeys);
    }
    // Duplicates within the array.
    {
        auto r = countNDVMultiKey({{.path = "a"}}, {fromjson("{a: [1, 1, 2]}")});
        ASSERT_EQ(3, r.totalSampleKeys);
        ASSERT_EQ(2, r.sampleUniqueKeys);
    }
    // Multiple documents, overlapping values.
    {
        auto r =
            countNDVMultiKey({{.path = "a"}}, {fromjson("{a: [1, 2]}"), fromjson("{a: [2, 3]}")});
        ASSERT_EQ(4, r.totalSampleKeys);
        ASSERT_EQ(3, r.sampleUniqueKeys);
    }
}

TEST(CountNDVMultiKey, NonArrayFieldBehavesLikeCountNDV) {
    auto r = countNDVMultiKey({{.path = "a"}},
                              {fromjson("{a: 1}"), fromjson("{a: 2}"), fromjson("{a: 1}")});
    ASSERT_EQ(3, r.totalSampleKeys);
    ASSERT_EQ(2, r.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, MixedArrayAndScalarDocs) {
    auto r = countNDVMultiKey({{.path = "a"}}, {fromjson("{a: 1}"), fromjson("{a: [2, 3]}")});
    ASSERT_EQ(3, r.totalSampleKeys);
    ASSERT_EQ(3, r.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, CompoundKeyOneArrayField) {
    // Array in the second field of a compound key.
    {
        auto r =
            countNDVMultiKey({{.path = "a"}, {.path = "b"}}, {fromjson("{a: 1, b: [1, 2, 3]}")});
        // Produces (1,1), (1,2), (1,3).
        ASSERT_EQ(3, r.totalSampleKeys);
        ASSERT_EQ(3, r.sampleUniqueKeys);
    }
    // Multiple docs, no shared compound values.
    {
        auto r = countNDVMultiKey({{.path = "a"}, {.path = "b"}},
                                  {fromjson("{a: 1, b: [1, 2]}"), fromjson("{a: 2, b: [1, 3]}")});
        // (1,1),(1,2),(2,1),(2,3) -- all distinct.
        ASSERT_EQ(4, r.totalSampleKeys);
        ASSERT_EQ(4, r.sampleUniqueKeys);
    }
    // Multiple docs sharing a compound key value.
    {
        auto r = countNDVMultiKey({{.path = "a"}, {.path = "b"}},
                                  {fromjson("{a: 1, b: [1, 2]}"), fromjson("{a: 1, b: [2, 3]}")});
        // (1,1),(1,2),(1,2),(1,3) -- (1,2) is duplicated.
        ASSERT_EQ(4, r.totalSampleKeys);
        ASSERT_EQ(3, r.sampleUniqueKeys);
    }
}

TEST(CountNDVMultiKey, EmptyArrayProducesUndefinedKey) {
    // MongoDB multikey indexes represent {a: []} as an undefined key.
    auto r = countNDVMultiKey({{.path = "a"}}, {fromjson("{a: []}")});
    ASSERT_EQ(1, r.totalSampleKeys);
    ASSERT_EQ(1, r.sampleUniqueKeys);

    // {a: []} maps to undefined; {a: null} and {} map to null -- two distinct keys.
    auto r2 = countNDVMultiKey({{.path = "a"}},
                               {fromjson("{a: []}"), fromjson("{a: null}"), fromjson("{}")});
    ASSERT_EQ(3, r2.totalSampleKeys);
    ASSERT_EQ(2, r2.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, MissingFieldTreatedAsNull) {
    auto r = countNDVMultiKey({{.path = "a"}},
                              {fromjson("{}"), fromjson("{a: null}"), fromjson("{a: 1}")});
    ASSERT_EQ(3, r.totalSampleKeys);
    ASSERT_EQ(2, r.sampleUniqueKeys);  // null/missing count as one, plus 1
}

// ---------------------------------------------------------------------------
// countNDVMultiKey -- bounds filtering
// ---------------------------------------------------------------------------

TEST(CountNDVMultiKey, BoundsPointFilter) {
    const std::vector<BSONObj> docs = {fromjson("{a: [1, 2, 3]}"), fromjson("{a: [2, 4]}")};
    // Only value 2 passes the [2,2] bound.
    const std::vector<OrderedIntervalList> bounds = {makePointOIL(2)};
    auto r = countNDVMultiKey({{.path = "a"}}, docs, std::span{bounds});
    ASSERT_EQ(5, r.totalSampleKeys);
    ASSERT_EQ(2, r.uniqueMatchingKeys);  // one 2 from each doc
    ASSERT_EQ(1, r.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, BoundsRangeFilter) {
    const std::vector<BSONObj> docs = {fromjson("{a: [1, 2, 3, 4]}"), fromjson("{a: [2, 3, 5]}")};
    // Values in [2,3] only.
    const std::vector<OrderedIntervalList> bounds = {makeOIL(2, 3)};
    auto r = countNDVMultiKey({{.path = "a"}}, docs, std::span{bounds});
    ASSERT_EQ(7, r.totalSampleKeys);
    ASSERT_EQ(4, r.uniqueMatchingKeys);  // 2,3 from first doc, 2,3 from second
    ASSERT_EQ(2, r.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, BoundsFilterAllOut) {
    const std::vector<BSONObj> docs = {fromjson("{a: [1, 2]}")};
    const std::vector<OrderedIntervalList> bounds = {makeOIL(10, 20)};
    auto r = countNDVMultiKey({{.path = "a"}}, docs, std::span{bounds});
    ASSERT_EQ(2, r.totalSampleKeys);
    ASSERT_EQ(0, r.uniqueMatchingKeys);
    ASSERT_EQ(0, r.sampleUniqueKeys);
}

TEST(CountNDVMultiKey, BoundsNullMatchesMissingAndNull) {
    // Null bounds [null,null] match missing and explicit null, but not empty array (undefined).
    const std::vector<BSONObj> docs = {
        fromjson("{}"),           // missing -> null
        fromjson("{a: null}"),    // explicit null
        fromjson("{a: []}"),      // empty array -> undefined (not matched by null bounds)
        fromjson("{a: [1, 2]}"),  // filtered out
    };
    const std::vector<OrderedIntervalList> bounds = {makeNullOIL()};
    auto r = countNDVMultiKey({{.path = "a"}}, docs, std::span{bounds});
    ASSERT_EQ(5, r.totalSampleKeys);
    ASSERT_EQ(2, r.uniqueMatchingKeys);
    ASSERT_EQ(1, r.sampleUniqueKeys);
}

// For a collection of documents {a: i % n}, countNDVMultiKey must agree with countNDV:
//
// {a: v} (scalar) -> sampleUniqueKeys == countNDV result, totalSampleKeys == numDocs
// {a: [v]} (single-element) -> same unique keys as scalar, one key per doc
// {a: [v,v,v,v]} (multi-element)  -> same unique keys, four keys per doc

struct CountNDVMultiKeyConsistencyParams {
    size_t numDocs;
    size_t uniqueValueCount;
};

class CountNDVMultiKeyConsistencyTest
    : public ::testing::TestWithParam<CountNDVMultiKeyConsistencyParams> {};

TEST_P(CountNDVMultiKeyConsistencyTest, ConsistentWithScalarForSameValueArrays) {
    const size_t numDocs = GetParam().numDocs;
    const size_t uniqueValueCount = GetParam().uniqueValueCount;

    std::vector<BSONObj> scalarDocs, singleElemDocs, multiElemDocs;
    for (size_t i = 0; i < numDocs; i++) {
        const int v = static_cast<int>(i % uniqueValueCount);
        scalarDocs.push_back(BSON("_id" << static_cast<int>(i) << "a" << v));
        singleElemDocs.push_back(BSON("_id" << static_cast<int>(i) << "a" << BSON_ARRAY(v)));
        multiElemDocs.push_back(
            BSON("_id" << static_cast<int>(i) << "a" << BSON_ARRAY(v << v << v << v)));
    }

    const size_t ndvScalar = countNDV({{.path = "a"}}, scalarDocs);
    ASSERT_EQ(uniqueValueCount, ndvScalar);

    // {a: [v]}: one key per doc, same unique values as the scalar case.
    const auto rSingle = countNDVMultiKey({{.path = "a"}}, singleElemDocs);
    ASSERT_EQ(numDocs, rSingle.totalSampleKeys);
    ASSERT_EQ(ndvScalar, rSingle.sampleUniqueKeys);

    // {a: [v,v,v,v]}: four keys per doc, but duplicates within each array leave unique count
    // identical to the scalar case.
    const auto rMulti = countNDVMultiKey({{.path = "a"}}, multiElemDocs);
    ASSERT_EQ(4 * numDocs, rMulti.totalSampleKeys);
    ASSERT_EQ(ndvScalar, rMulti.sampleUniqueKeys);
}

INSTANTIATE_TEST_SUITE_P(
    ConsistencyParams,
    CountNDVMultiKeyConsistencyTest,
    ::testing::Values(CountNDVMultiKeyConsistencyParams{.numDocs = 10, .uniqueValueCount = 1},
                      CountNDVMultiKeyConsistencyParams{.numDocs = 10, .uniqueValueCount = 5},
                      CountNDVMultiKeyConsistencyParams{.numDocs = 100, .uniqueValueCount = 1},
                      CountNDVMultiKeyConsistencyParams{.numDocs = 100, .uniqueValueCount = 10}));

DEATH_TEST(CountNDVMultiKeyDeathTest, ThrowsOnParallelArrays, "Parallel arrays are not supported") {
    countNDVMultiKey({{.path = "a"}, {.path = "b"}}, {fromjson("{a: [1, 2], b: [3, 4]}")});
}
TEST(CountUniqueDocuments, AllUnique) {
    const std::vector<BSONObj> docs = {
        fromjson("{_id: 1, a: 10}"),
        fromjson("{_id: 2, a: 20}"),
        fromjson("{_id: 3, a: 30}"),
    };
    ASSERT_EQ(3, countUniqueDocuments(docs));
}

TEST(CountUniqueDocuments, DuplicateDoc) {
    const std::vector<BSONObj> docs = {
        fromjson("{_id: 1, a: 10}"),
        fromjson("{_id: 2, a: 20}"),
        fromjson("{_id: 3, a: 30}"),
        fromjson("{_id: 1, a: 10}"),
    };
    ASSERT_EQ(3, countUniqueDocuments(docs));
}

TEST(CountUniqueDocuments, UniqueIdsButSharedFieldValues) {
    // Documents with distinct _id but identical other fields count as unique.
    const std::vector<BSONObj> docs = {
        fromjson("{_id: 1, a: 99}"),
        fromjson("{_id: 2, a: 99}"),
        fromjson("{_id: 3, a: 99}"),
    };
    ASSERT_EQ(3, countUniqueDocuments(docs));
}

TEST(CountUniqueDocuments, EmptyList) {
    ASSERT_EQ(0, countUniqueDocuments({}));
}

// Helper to build a BSONElement from an int stored in a persistent BSONObj.
// The returned element points into 'storage', which must outlive the element.
static BSONElement intElt(int v, BSONObj& storage) {
    storage = BSON("" << v);
    return storage.firstElement();
}

TEST(MatchesIntervalOIL, EmptyOILReturnsFalse) {
    OrderedIntervalList oil;
    BSONObj storage;
    ASSERT_FALSE(matchesInterval(oil, intElt(5, storage)));
}

TEST(MatchesIntervalOIL, SingleInclusiveInterval) {
    // OIL: [3, 7]
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 3 << "" << 7), true, true));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(2, s)));  // before start
    ASSERT_TRUE(matchesInterval(oil, intElt(3, s)));   // == start, inclusive
    ASSERT_TRUE(matchesInterval(oil, intElt(5, s)));   // strictly inside
    ASSERT_TRUE(matchesInterval(oil, intElt(7, s)));   // == end, inclusive
    ASSERT_FALSE(matchesInterval(oil, intElt(8, s)));  // after end
}

TEST(MatchesIntervalOIL, SingleExclusiveBounds) {
    // OIL: (3, 7)
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 3 << "" << 7), false, false));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(3, s)));  // == start, exclusive
    ASSERT_TRUE(matchesInterval(oil, intElt(4, s)));   // inside
    ASSERT_FALSE(matchesInterval(oil, intElt(7, s)));  // == end, exclusive
}

TEST(MatchesIntervalOIL, MultipleAscendingIntervals) {
    // OIL: [1,3], [5,7], [9,11]
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, true));
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 7), true, true));
    oil.intervals.push_back(Interval(BSON("" << 9 << "" << 11), true, true));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(0, s)));   // before all
    ASSERT_TRUE(matchesInterval(oil, intElt(1, s)));    // start of first
    ASSERT_TRUE(matchesInterval(oil, intElt(2, s)));    // inside first
    ASSERT_TRUE(matchesInterval(oil, intElt(3, s)));    // end of first
    ASSERT_FALSE(matchesInterval(oil, intElt(4, s)));   // gap [3,5)
    ASSERT_TRUE(matchesInterval(oil, intElt(5, s)));    // start of second
    ASSERT_TRUE(matchesInterval(oil, intElt(6, s)));    // inside second
    ASSERT_TRUE(matchesInterval(oil, intElt(7, s)));    // end of second
    ASSERT_FALSE(matchesInterval(oil, intElt(8, s)));   // gap [7,9)
    ASSERT_TRUE(matchesInterval(oil, intElt(9, s)));    // start of third
    ASSERT_TRUE(matchesInterval(oil, intElt(10, s)));   // inside third
    ASSERT_TRUE(matchesInterval(oil, intElt(11, s)));   // end of third
    ASSERT_FALSE(matchesInterval(oil, intElt(12, s)));  // after all
}

TEST(MatchesIntervalOIL, MultipleIntervalsWithExclusiveBounds) {
    // OIL: [1,3), (5,7]
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, false));
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 7), false, true));

    BSONObj s;
    ASSERT_TRUE(matchesInterval(oil, intElt(1, s)));   // inclusive start
    ASSERT_FALSE(matchesInterval(oil, intElt(3, s)));  // exclusive end of first
    ASSERT_FALSE(matchesInterval(oil, intElt(4, s)));  // gap
    ASSERT_FALSE(matchesInterval(oil, intElt(5, s)));  // exclusive start of second
    ASSERT_TRUE(matchesInterval(oil, intElt(6, s)));   // inside second
    ASSERT_TRUE(matchesInterval(oil, intElt(7, s)));   // inclusive end
}

TEST(MatchesIntervalOIL, MixedRangesAndPointIntervals) {
    // OIL: [1,3], [5,5], [7,9]
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << 3), true, true));
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));
    oil.intervals.push_back(Interval(BSON("" << 7 << "" << 9), true, true));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(0, s)));   // before all
    ASSERT_TRUE(matchesInterval(oil, intElt(1, s)));    // start of range [1,3]
    ASSERT_TRUE(matchesInterval(oil, intElt(2, s)));    // inside [1,3]
    ASSERT_TRUE(matchesInterval(oil, intElt(3, s)));    // end of range [1,3]
    ASSERT_FALSE(matchesInterval(oil, intElt(4, s)));   // gap between [1,3] and [5,5]
    ASSERT_TRUE(matchesInterval(oil, intElt(5, s)));    // point interval [5,5]
    ASSERT_FALSE(matchesInterval(oil, intElt(6, s)));   // gap between [5,5] and [7,9]
    ASSERT_TRUE(matchesInterval(oil, intElt(7, s)));    // start of range [7,9]
    ASSERT_TRUE(matchesInterval(oil, intElt(8, s)));    // inside [7,9]
    ASSERT_TRUE(matchesInterval(oil, intElt(9, s)));    // end of range [7,9]
    ASSERT_FALSE(matchesInterval(oil, intElt(10, s)));  // after all
}

TEST(MatchesIntervalOIL, DescendingIntervals) {
    // OIL descending: [7,3] then [1,-1]
    // In a descending OIL, start >= end and intervals are ordered from high to low.
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 7 << "" << 3), true, true));
    oil.intervals.push_back(Interval(BSON("" << 1 << "" << -1), true, true));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(8, s)));   // above first interval's start
    ASSERT_TRUE(matchesInterval(oil, intElt(7, s)));    // == start of first
    ASSERT_TRUE(matchesInterval(oil, intElt(5, s)));    // inside first
    ASSERT_TRUE(matchesInterval(oil, intElt(3, s)));    // == end of first
    ASSERT_FALSE(matchesInterval(oil, intElt(2, s)));   // gap [1,3)
    ASSERT_TRUE(matchesInterval(oil, intElt(1, s)));    // start of second
    ASSERT_TRUE(matchesInterval(oil, intElt(0, s)));    // inside second
    ASSERT_TRUE(matchesInterval(oil, intElt(-1, s)));   // end of second
    ASSERT_FALSE(matchesInterval(oil, intElt(-2, s)));  // below last interval's end
}
TEST(MatchesIntervalOIL, PointIntervalInclusive) {
    // OIL: [5, 5] — computeDirection() returns kDirectionNone; direction defaults to ascending.
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), true, true));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(4, s)));  // before the point
    ASSERT_TRUE(matchesInterval(oil, intElt(5, s)));   // exactly the point
    ASSERT_FALSE(matchesInterval(oil, intElt(6, s)));  // after the point
}

TEST(MatchesIntervalOIL, PointIntervalExclusive) {
    // OIL: (5, 5) — degenerate empty interval; computeDirection() returns kDirectionNone.
    // Nothing can fall inside an exclusive point, so all values should return false.
    OrderedIntervalList oil;
    oil.intervals.push_back(Interval(BSON("" << 5 << "" << 5), false, false));

    BSONObj s;
    ASSERT_FALSE(matchesInterval(oil, intElt(4, s)));
    ASSERT_FALSE(matchesInterval(oil, intElt(5, s)));
    ASSERT_FALSE(matchesInterval(oil, intElt(6, s)));
}
}  // namespace mongo::ce
