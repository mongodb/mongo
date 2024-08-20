/**
 * Tests missing index keys check in dbCheck with ranges specified. Primary and secondary will have
 * index inconsistencies for different docs, and dbCheck will miss detecting these inconsistencies
 * if the doc is outside of the range specified in the dbCheck invocation.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {checkHealthLog, clearHealthLog, runDbCheck} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const rst = new ReplSetTest({name: jsTestName(), nodes: 2});
rst.startSet();
rst.initiateWithHighElectionTimeout();

const dbName = "dbCheckRangeMissingKeys";
const collName = "testColl";

const primary = rst.getPrimary();
const primaryDb = primary.getDB(dbName);
const secondary = rst.getSecondary();
const secondaryDb = secondary.getDB(dbName);
assert.commandWorked(primaryDb.runCommand({
    createIndexes: collName,
    indexes: [{key: {a: 1}, name: 'a_1'}],
}));
rst.awaitReplication();

const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;

const errQuery = {
    operation: "dbCheckBatch",
    severity: "error",
};

const missingKeyErrQuery = {
    ...errQuery,
    msg: "Document has missing index keys",
};

// Insert docs with _id from 0 to 100. The doc with _id: 25 will be missing index keys on the
// primary and the doc with _id: 75 will be missing index keys on the secondary.
const numDocs = 100;
let skipIndexingFp;
for (let i = 0; i < numDocs; i++) {
    if (i === 25) {
        // Skip indexing the doc with _id: 25 on the primary.
        skipIndexingFp = configureFailPoint(primaryDb, "skipIndexNewRecords", {skipIdIndex: false});
    } else if (i === 75) {
        // Skip indexing the doc with _id: 75 on the primary.
        skipIndexingFp =
            configureFailPoint(secondaryDb, "skipIndexNewRecords", {skipIdIndex: false});
    }
    assert.commandWorked(primaryDb[collName].insert({_id: i, a: i}));
    // We need to wait the insert to be applied on secondary to make sure there is exactly one index
    // skipped.
    rst.awaitReplication();
    if (skipIndexingFp) {
        skipIndexingFp.off();
        skipIndexingFp = null;
    }
}
rst.awaitReplication();

function runTest(start, end, numExpectedErrorsPrimary, numExpectedErrorsSecondary) {
    // Run dbCheck on the specified range.
    runDbCheck(rst, primaryDb, collName, {
        maxDocsPerBatch: 10,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck",
        start: {_id: start},
        end: {_id: end},
    });

    // Inconsistency detected on primary but not secondary.
    checkHealthLog(primaryHealthLog, errQuery, numExpectedErrorsPrimary);
    checkHealthLog(primaryHealthLog, missingKeyErrQuery, numExpectedErrorsPrimary);

    checkHealthLog(secondaryHealthLog, errQuery, numExpectedErrorsSecondary);
    checkHealthLog(secondaryHealthLog, missingKeyErrQuery, numExpectedErrorsSecondary);

    clearHealthLog(rst);
}

jsTestLog("Run dbCheck on the full range");
runTest(0, 100, 1 /* numExpectedErrorsPrimary */, 1 /* numExpectedErrorsSecondary */);

jsTestLog("Run dbCheck on a range without any inconsistencies.");
runTest(0, 24, 0 /* numExpectedErrorsPrimary */, 0 /* numExpectedErrorsSecondary */);

jsTestLog("Run dbCheck on a range that would detect only the primary inconsistency.");
runTest(0, 50, 1 /* numExpectedErrorsPrimary */, 0 /* numExpectedErrorsSecondary */);

jsTestLog("Run dbCheck on a range that would detect only the secondary inconsistency.");
runTest(50, 100, 0 /* numExpectedErrorsPrimary */, 1 /* numExpectedErrorsSecondary */);

jsTestLog(
    "Run dbCheck with the range specified starting/ending with the missing index key on primary");
runTest(24, 25, 1 /* numExpectedErrorsPrimary */, 0 /* numExpectedErrorsSecondary */);

jsTestLog(
    "Run dbCheck with the range specified starting/ending with the missing index key on secondary");
runTest(74, 75, 0 /* numExpectedErrorsPrimary */, 1 /* numExpectedErrorsSecondary */);

jsTestLog("Run dbCheck when the start/end range does not exist in the collection");
runTest(-1, 50, 1 /* numExpectedErrorsPrimary */, 0 /* numExpectedErrorsSecondary */);
runTest(50, 101, 0 /* numExpectedErrorsPrimary */, 1 /* numExpectedErrorsSecondary */);
runTest(101, 201, 0 /* numExpectedErrorsPrimary */, 0 /* numExpectedErrorsSecondary */);

rst.stopSet();
