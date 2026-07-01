/**
 * Tests that change streams correctly handle rewrites of null, existence and equality checks, for
 * both existent and non-existent fields and subfields.
 * @tags: [
 *   # The test runs a lot of queries and is massively slower when running against a secondary.
 *   assumes_read_preference_unchanged,
 *   requires_pipeline_optimization,
 *   uses_change_streams,
 *   # This test runs too long to be included in code coverage:
 *   incompatible_with_gcov,
 *   # SERVER-36681 changed the behavior of SBE and classic engines
 *   requires_fcv_90,
 * ]
 */
import {
    compareOptimizedAndNonOptimizedChangeStreamResults,
    generateEventsAndFieldsToBeTestedForOplogRewrites,
} from "jstests/libs/query/change_stream_oplog_rewrite_test.js";

const dbName = jsTestName();
const collName = "coll1";

// Define the filters that we want to apply to each field.
function generateMatchFilters(fieldPath, fieldsToBeTested) {
    const valuesToTest = fieldsToBeTested[fieldPath].values.concat(
        fieldsToBeTested[fieldPath].extraValues,
    );

    const filters = [
        {[fieldPath]: {$eq: null}},
        {[fieldPath]: {$ne: null}},
        {[fieldPath]: {$lte: null}},
        {[fieldPath]: {$gte: null}},
        {[fieldPath]: {$exists: true}},
        {[fieldPath]: {$exists: false}},
    ];

    for (let value of valuesToTest) {
        filters.push({[fieldPath]: value});
    }

    return filters;
}

const {startPoint, fieldsToBeTested} = generateEventsAndFieldsToBeTestedForOplogRewrites(
    db,
    dbName,
    collName,
);

let predicatesToTest = [];
for (let fieldToTest in fieldsToBeTested) {
    predicatesToTest = predicatesToTest.concat(generateMatchFilters(fieldToTest, fieldsToBeTested));
}

const failedTestCases = compareOptimizedAndNonOptimizedChangeStreamResults(
    db,
    dbName,
    predicatesToTest,
    startPoint,
);

// Assert that there were no failed test cases.
assert(failedTestCases.length == 0, failedTestCases);
