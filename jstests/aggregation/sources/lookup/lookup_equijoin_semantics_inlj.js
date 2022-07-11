/**
 * Tests for $lookup with localField/foreignField syntax using indexed nested loop join algorithm.
 */
(function() {
"use strict";

load("jstests/aggregation/sources/lookup/lookup_equijoin_semantics_lib.js");  // For runTests and
                                                                              // runTest_*.

localColl = db.lookup_arrays_semantics_local_inlj;
foreignColl = db.lookup_arrays_semantics_foreign_inlj;

/**
 * Run the tests with sorted ascending/descending indexes.
 */
currentJoinAlgorithm = JoinAlgorithm.INLJ_Asc;
runTests();

currentJoinAlgorithm = JoinAlgorithm.INLJ_Dec;
runTests();

/**
 * Tests with hashed index. Because hashed index doesn't support array data, it's easier to provide
 * separate tests for it (even if with some duplication) than adjust the data in the common tests.
 */
(function runHashedIndexTests() {
    currentJoinAlgorithm = JoinAlgorithm.INLJ_Hashed;

    (function testVariousDataTypes() {
        // NOTE: There is no shell equivalent for the following BSON types:
        // - Code (13)
        // - Symbol (14)
        // - CodeWScope (15)
        const docs = [
            {_id: 0, a: NumberInt(0)},
            {_id: 1, a: 3.14},
            {_id: 2, a: NumberDecimal(3.14)},
            {_id: 3, a: "abc"},
            {_id: 4, a: {b: 1, c: 2, d: 3}},
            {_id: 5, a: true},
            {_id: 6, a: false},
            {_id: 7, a: new ISODate("2022-01-01T00:00:00.00Z")},
            {_id: 8, a: new Timestamp(1, 123)},
            {_id: 9, a: new ObjectId("0102030405060708090A0B0C")},
            {_id: 10, a: new BinData(0, "BBBBBBBBBBBBBBBBBBBBBBBBBBBB")},
            {_id: 11, a: /hjkl/},
            {_id: 12, a: /hjkl/g},
            {_id: 13, a: new DBRef("collection", "id", "database")},
            {_id: 14, a: null},
        ];
        docs.forEach(doc => {
            runTest_SingleLocalRecord({
                testDescription: "Various data types in foreign matching to: " + tojson(doc),
                localRecord: {b: doc.a},
                localField: "b",
                foreignRecords: docs,
                foreignField: "a",
                idsExpectedToMatch: [doc._id]
            });
        });

        // Hashed indexes don't support array data, but asking to match an array from local is
        // allowed and should produce no matches.
        runTest_SingleLocalRecord({
            testDescription: "Various data types in foreign matching to an array",
            localRecord: {b: [[1, 2]]},
            localField: "b",
            foreignRecords: docs,
            foreignField: "a",
            idsExpectedToMatch: []
        });
    })();

    (function testNullMissingUndefined() {
        const foreignRecords = [
            {_id: 0, a: null},
            {_id: 1, no_a: 42},

            {_id: 10, a: 0},
            {_id: 11, a: false},
            {_id: 12, a: {}},
            {_id: 13, a: ""},
        ];

        runTest_SingleLocalRecord({
            testDescription: "Null in local",
            localRecord: {b: null},
            localField: "b",
            foreignRecords,
            foreignField: "a",
            idsExpectedToMatch: [0, 1]
        });
    })();

    (function testMatchingWithNestedPaths() {
        const foreignRecords = [
            {_id: 0, a: {b: {c: 1}}},
            {_id: 1, a: {no_b: 1}},
            {_id: 2, a: {b: {no_c: 1}}},
        ];

        runTest_SingleLocalRecord({
            testDescription: "Index join with nested path in foreign field",
            localRecord: {b: 1},
            localField: "b",
            foreignRecords,
            foreignField: "a.b.c",
            idsExpectedToMatch: [0]
        });
    })();
})();

/**
 * Other miscelaneous tests for INLJ.
 */
(function runMiscelaneousInljTests() {
    currentJoinAlgorithm = JoinAlgorithm.INLJ_Asc;

    (function testServer66119() {
        const docs = [
            {_id: 0, a: {b: [1, 2, 1]}},
            {_id: 1, a: {b: [1, 3, [1, 2]]}},
            {_id: 2, a: {b: [1, 3, [2, 1]]}},
            {_id: 3, a: {b: [1, 3, [1]]}},
            {_id: 4, a: [{b: 1}, {b: [1, 2]}]},
            {_id: 5, a: [{b: 1}, {b: [[1], 2]}]},
            {_id: 6, a: [{b: [1, 2]}, {b: [3, [1]]}]},
        ];

        runTest_SingleForeignRecord({
            testDescription: "Nested arrays with the same value as another key value in local",
            localRecords: docs,
            localField: "a.b",
            foreignRecord: {_id: 0, a: 1},
            foreignField: "a",
            idsExpectedToMatch: [0, 1, 2, 3, 4, 5, 6]
        });
    })();
})();
})();
