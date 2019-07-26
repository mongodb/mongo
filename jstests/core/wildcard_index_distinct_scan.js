/**
 * Tests that a $** index can provide a DISTINCT_SCAN or indexed solution where appropriate.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");  // For arrayEq.
load("jstests/libs/analyze_plan.js");         // For planHasStage and getPlanStages.

const assertArrayEq = (l, r) => assert(arrayEq(l, r), tojson(l) + " != " + tojson(r));

const coll = db.all_paths_distinct_scan;
coll.drop();

// Records whether the field which we are distinct-ing over is multikey.
let distinctFieldIsMultikey = false;

// Insert a set of documents into the collection. The 'listOfValues' argument contains values of
// various types, and we insert numerous documents containing each of the values. This allows us
// to confirm that 'distinct' with a wildcard index (1) can return values of any type, (2) will
// only return the set of unique values, and (3) handles multikey values appropriately in cases
// where 'listOfValues' includes an array.
function insertTestData(fieldName, listOfValues) {
    distinctFieldIsMultikey = listOfValues.some((val) => Array.isArray(val));
    const bulk = coll.initializeUnorderedBulkOp();
    coll.drop();
    for (let i = 0; i < 200; i++) {
        const didx = (i % listOfValues.length);
        bulk.insert({[fieldName]: listOfValues[didx], b: didx, c: (-i)});
    }
    assert.commandWorked(bulk.execute());
}

/**
 * Runs a single wildcard distinct scan test. If 'expectedPath' is non-null, verifies that there
 * is an indexed solution that uses the $** index with the given path string. If 'expectedPath'
 * is null, verifies that no indexed solution was found.
 */
function assertWildcardDistinctScan(
    {distinctKey, query, pathProjection, expectedScanType, expectedResults, expectedPath}) {
    // Drop all indexes before running the test. This allows us to perform the distinct with a
    // COLLSCAN at first, to confirm that the results are as expected.
    assert.commandWorked(coll.dropIndexes());

    // Confirm that the distinct runs with a COLLSCAN.
    let winningPlan = coll.explain().distinct(distinctKey, query).queryPlanner.winningPlan;
    assert(planHasStage(coll.getDB(), winningPlan, "COLLSCAN"));
    // Run the distinct and confirm that it produces the expected results.
    assertArrayEq(coll.distinct(distinctKey, query), expectedResults);

    // Build a wildcard index on the collection and re-run the test.
    const options = (pathProjection ? {wildcardProjection: pathProjection} : {});
    assert.commandWorked(coll.createIndex({"$**": 1}, options));

    // We expect the following outcomes for a 'distinct' that attempts to use a $** index:
    // - No query: COLLSCAN.
    // - Query for object value on distinct field: COLLSCAN.
    // - Query for non-object value on non-multikey distinct field: DISTINCT_SCAN.
    // - Query for non-object value on multikey distinct field: IXSCAN with FETCH.
    // - Query for non-object value on field other than the distinct field: IXSCAN with FETCH.
    const fetchIsExpected = (expectedScanType !== "DISTINCT_SCAN");

    // Explain the query, and determine whether an indexed solution is available. If
    // 'expectedPath' is null, then we do not expect the $** index to provide a plan.
    winningPlan = coll.explain().distinct(distinctKey, query).queryPlanner.winningPlan;
    if (!expectedPath) {
        assert(planHasStage(coll.getDB(), winningPlan, "COLLSCAN"));
        assert.eq(expectedScanType, "COLLSCAN");
        return;
    }

    // Confirm that the $** distinct scan produces the expected results.
    assertArrayEq(coll.distinct(distinctKey, query), expectedResults);
    // Confirm that the $** plan adheres to 'fetchIsExpected' and 'expectedScanType'.
    assert.eq(planHasStage(coll.getDB(), winningPlan, "FETCH"), fetchIsExpected);
    assert(planHasStage(coll.getDB(), winningPlan, expectedScanType));
    assert.docEq({$_path: 1, [expectedPath]: 1},
                 getPlanStages(winningPlan, expectedScanType).shift().keyPattern);
}

// The set of distinct values that should be produced by each of the test listed below.
const distinctValues = [1, 2, "3", null, {c: 5, d: 6}, {d: 6, c: 5}, {}, 9, 10, {e: 11}];

// Define the set of values that the distinct field may take. The first test case consists
// entirely of non-multikey fields, while the second includes multikey fields.
const testCases = [
    // Non-multikey field values.
    {
        insertField: "a",
        queryField: "a",
        fieldValues: [1, 2, "3", null, {c: 5, d: 6}, {d: 6, c: 5}, {}, 9, 10, {e: 11}]
    },
    // Multikey field values. Note that values within arrays are unwrapped by the distinct
    // scan, and empty arrays are thus not included.
    {
        insertField: "a",
        queryField: "a",
        fieldValues: [1, 2, "3", null, {c: 5, d: 6}, {d: 6, c: 5}, {}, [], [9, 10], [{e: 11}]]
    },
    // Non-multikey dotted field values.
    {
        insertField: "a",
        queryField: "a.x",
        fieldValues: [
            {x: 1},
            {x: 2},
            {x: "3"},
            {x: null},
            {x: {c: 5, d: 6}},
            {x: {d: 6, c: 5}},
            {x: {}},
            {x: 9},
            {x: 10},
            {x: {e: 11}}
        ]
    },
    // Multikey dotted field values.
    {
        insertField: "a",
        queryField: "a.x",
        fieldValues: [
            [{x: 1}],
            [{x: 2}],
            [{x: "3"}],
            [{x: null}],
            [{x: {c: 5, d: 6}}],
            [{x: {d: 6, c: 5}}],
            [{x: {}}],
            [{x: []}],
            [{x: 9}, {x: 10}],
            [{x: [{e: 11}]}]
        ]
    }
];

// Run all combinations of query, no-query, multikey and non-multikey distinct tests.
for (let testCase of testCases) {
    // Log the start of the test and create the dataset.
    jsTestLog("Test case: " + tojson(testCase));
    insertTestData(testCase.insertField, testCase.fieldValues);

    // Test that a $** index cannot provide an indexed 'distinct' without a query.
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {},
        expectedScanType: "COLLSCAN",
        expectedResults: distinctValues,
        expectedPath: null
    });

    // Test that a $** index can provide an indexed 'distinct' for distinct-key queries.
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {[testCase.queryField]: {$lt: 3}},
        expectedScanType: (distinctFieldIsMultikey ? "IXSCAN" : "DISTINCT_SCAN"),
        expectedResults: [1, 2],
        expectedPath: testCase.queryField
    });

    // Test that a $** index can provide an indexed 'distinct' for a query on another field.
    const offset = Math.floor(testCase.fieldValues.length / 2);
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {b: {$gte: offset}},
        expectedScanType: "IXSCAN",
        expectedResults: distinctValues.slice(offset),
        expectedPath: "b"
    });

    // Test that a $** index cannot provide an indexed 'distinct' for object value queries.
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {[testCase.queryField]: {$gte: {c: 5}}},
        expectedScanType: "COLLSCAN",
        expectedResults: [{c: 5, d: 6}, {d: 6, c: 5}, {e: 11}],
        expectedPath: null
    });

    // Test that a $** index can provide an indexed 'distinct' for a MinMax query.
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {[testCase.queryField]: {$gte: MinKey, $lte: MaxKey}},
        expectedScanType: "IXSCAN",
        expectedResults: distinctValues,
        expectedPath: testCase.queryField
    });

    // Test that a $** index cannot provide an indexed 'distinct' for excluded fields.
    assertWildcardDistinctScan({
        distinctKey: testCase.queryField,
        query: {c: {$lt: 0}},
        pathProjection: {c: 0},
        expectedScanType: "COLLSCAN",
        expectedResults: distinctValues,
        expectedPath: null
    });
}
})();
