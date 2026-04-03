/**
 * Tests that change streams correctly handle rewrites of null, existence and equality checks, for
 * both existent and non-existent fields and subfields.
 * @tags: [
 *   requires_pipeline_optimization,
 *   uses_change_streams,
 *   # This test runs too long to be included in code coverage:
 *   incompatible_with_gcov,
 * ]
 */
import {
    compareOptimizedAndNonOptimizedChangeStreamResults,
    generateEventsAndFieldsToBeTestedForOplogRewrites,
} from "jstests/libs/query/change_stream_oplog_rewrite_test.js";

const dbName = jsTestName();
const collName = "coll1";

// Define the filters that we want to apply to each field.
function generateExprFilters(fieldPath) {
    const valuesToTest = fieldsToBeTested[fieldPath].values.concat(fieldsToBeTested[fieldPath].extraValues);

    const exprFieldPath = "$" + fieldPath;
    const exprs = [
        {$expr: {$eq: [exprFieldPath, null]}},
        {$expr: {$ne: [exprFieldPath, null]}},
        {$expr: {$lte: [exprFieldPath, null]}},
        {$expr: {$gte: [exprFieldPath, null]}},
    ];

    for (let value of valuesToTest) {
        exprs.push({$expr: {$eq: [exprFieldPath, value]}});
    }

    return exprs;
}

const {startPoint, fieldsToBeTested} = generateEventsAndFieldsToBeTestedForOplogRewrites(db, dbName, collName);

let predicatesToTest = [];
for (let fieldToTest in fieldsToBeTested) {
    predicatesToTest = predicatesToTest.concat(generateExprFilters(fieldToTest, fieldsToBeTested));
}

const failedTestCases = compareOptimizedAndNonOptimizedChangeStreamResults(db, dbName, predicatesToTest, startPoint);

// Assert that there were no failed test cases.
assert(failedTestCases.length == 0, failedTestCases);
