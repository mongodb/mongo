/**
 * Tests explode for sort query planner behavior when the input query plan contains OR > FETCH >
 * IXSCAN, OR > IXSCAN subtrees or a mix of both.
 * @tags: [
 *   # Does not work with legacy shellWriteMode.
 *   requires_find_command,
 *   sbe_incompatible,
 * ]
 */
(function() {
"use strict";

const testDB = db.getSiblingDB(jsTestName());
assert.commandWorked(testDB.dropDatabase());
const coll = testDB.explode_for_sort_fetch;

// Executes a test case that creates indexes, inserts documents, issues a find query on a
// collection and checks the results.
function executeQueryTestCase(testCase) {
    jsTestLog(tojson(testCase));

    // Drop the collection.
    coll.drop();

    // Create indexes.
    assert.commandWorked(coll.createIndexes(testCase.indexes));

    // Insert some documents into the collection.
    assert.commandWorked(coll.insert(testCase.inputDocuments));

    // Issue a find query and compare results to expected.
    assert.eq(coll.find(testCase.filter).sort(testCase.sort).toArray(),
              testCase.expectedResults,
              `Test case ${testCase.name}`);
}

const testCases = [
    {
        // Verifies that query produces correct results when the query plan passed to explode for
        // sort contains subtree of shape OR > FETCH > IXSCAN. Attribute 'f' is fetched by the FETCH
        // stage.
        name: "ExplodeForSortIxscanFetchOr",
        indexes: [{a: 1, b: 1, e: 1}, {c: -1, d: -1, e: -1}],
        filter: {
            $or: [
                {a: {$in: [1, 2]}, b: {$in: [3, 4]}, f: 5},
                {c: {$in: [1, 2]}, d: {$in: [3, 4]}, f: 5}
            ]
        },
        sort: {e: 1},
        inputDocuments: [
            {_id: 3, a: 1, b: 3, f: 5, e: 3},
            {_id: 4, a: 2, b: 4, f: 5, e: 4},
            {_id: 8, a: 1, b: 4, f: 5, e: 8},
            {_id: 1, a: 2, b: 3, f: 5, e: 1},
            {_id: 0, a: 2, b: 3, f: 3, e: 0},
            {_id: 7, c: 1, d: 3, f: 5, e: 7},
            {_id: 6, c: 2, d: 4, f: 5, e: 6},
            {_id: 2, c: 1, d: 4, f: 5, e: 2},
            {_id: 5, c: 2, d: 3, f: 5, e: 5},
            {_id: 9, c: 2, d: 3, f: 3, e: 9},
        ],
        expectedResults: [
            {_id: 1, a: 2, b: 3, f: 5, e: 1},
            {_id: 2, c: 1, d: 4, f: 5, e: 2},
            {_id: 3, a: 1, b: 3, f: 5, e: 3},
            {_id: 4, a: 2, b: 4, f: 5, e: 4},
            {_id: 5, c: 2, d: 3, f: 5, e: 5},
            {_id: 6, c: 2, d: 4, f: 5, e: 6},
            {_id: 7, c: 1, d: 3, f: 5, e: 7},
            {_id: 8, a: 1, b: 4, f: 5, e: 8},
        ]
    },
    {
        // Verifies that query produces correct results when the query plan passed to explode for
        // sort contains a mix of subtrees of shape OR > FETCH > IXSCAN and OR > IXSCAN. Attribute
        // 'f' is fetched by the FETCH stage.
        name: "ExplodeForSortIxscanFetchOrMixedWithIxscanOr",
        indexes: [{a: 1, b: 1, e: 1}, {c: -1, d: 1, e: -1}],
        filter:
            {$or: [{a: {$in: [1, 2]}, b: {$in: [3, 4]}, f: 5}, {c: {$in: [1, 2]}, d: {$in: [3]}}]},
        sort: {e: 1},
        inputDocuments: [
            {_id: 3, a: 1, b: 3, f: 5, e: 3},
            {_id: 4, a: 2, b: 4, f: 5, e: 4},
            {_id: 8, a: 1, b: 4, f: 5, e: 8},
            {_id: 1, a: 2, b: 3, f: 5, e: 1},
            {_id: 0, a: 2, b: 3, f: 3, e: 0},
            {_id: 7, c: 1, d: 3, f: 5, e: 7},
            {_id: 5, c: 2, d: 3, f: 5, e: 5},
            {_id: 9, c: 2, d: 3, f: 3, e: 9},
        ],
        expectedResults: [
            {_id: 1, a: 2, b: 3, f: 5, e: 1},
            {_id: 3, a: 1, b: 3, f: 5, e: 3},
            {_id: 4, a: 2, b: 4, f: 5, e: 4},
            {_id: 5, c: 2, d: 3, f: 5, e: 5},
            {_id: 7, c: 1, d: 3, f: 5, e: 7},
            {_id: 8, a: 1, b: 4, f: 5, e: 8},
            {_id: 9, c: 2, d: 3, f: 3, e: 9},
        ]
    },
    {
        // Verifies that query produces correct results when the query plan passed to explode for
        // sort contains a subtree of shape OR > IXSCAN.
        name: "ExplodeForSortIxscanOr",
        indexes: [{a: 1, b: 1, e: 1}, {c: -1, d: 1, e: -1}],
        filter: {$or: [{a: {$in: [1, 2]}, b: 3}, {c: {$in: [1, 2]}, d: 3}]},
        sort: {e: 1},
        inputDocuments: [
            {_id: 1, a: 1, b: 3, f: 5, e: 1},
            {_id: 3, a: 2, b: 3, f: 5, e: 3},
            {_id: 2, c: 1, d: 3, f: 5, e: 2},
            {_id: 4, c: 2, d: 3, f: 5, e: 4},
        ],
        expectedResults: [
            {_id: 1, a: 1, b: 3, f: 5, e: 1},
            {_id: 2, c: 1, d: 3, f: 5, e: 2},
            {_id: 3, a: 2, b: 3, f: 5, e: 3},
            {_id: 4, c: 2, d: 3, f: 5, e: 4},
        ]
    },
];
testCases.forEach(executeQueryTestCase);
}());