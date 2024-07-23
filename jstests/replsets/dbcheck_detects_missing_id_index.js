/**
 * Tests that dbCheck reports an error when the _id index is missing in a collection.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    checkHealthLog,
    clearHealthLog,
    logEveryBatch,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because secondary will be missing _id index.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const rst = new ReplSetTest({
    nodes: 2,
});

rst.startSet();
rst.initiate();
rst.awaitReplication();

const primary = rst.getPrimary();
const secondary = rst.getSecondary();
const dbName = "test";
const db = primary.getDB(dbName);
const collName = "testColl";

const primaryHealthLog = primary.getDB('local').system.healthlog;
const secondaryHealthLog = secondary.getDB('local').system.healthlog;

logEveryBatch(rst);

const skipIdIndexFp = configureFailPoint(secondary, "skipIdIndex");

resetAndInsert(rst, db, collName, 100);
skipIdIndexFp.off();

runDbCheck(rst, db, collName, {}, true /* awaitCompletion */);

// Verify that secondary logs an error with missing _id index.
checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
checkHealthLog(secondaryHealthLog, logQueries.missingIdIndex, 1);
checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1);

// Clear healthlog entries.
clearHealthLog(rst);

// Step up secondary.
rst.stepUp(secondary);
rst.awaitNodesAgreeOnPrimary();
rst.awaitReplication();

const newPrimary = secondary;
const newSecondary = primary;
const newPrimaryHealthLog = newPrimary.getDB("local").system.healthlog;
const newSecondaryHealthLog = newSecondary.getDB("local").system.healthlog;

runDbCheck(rst, newPrimary.getDB(dbName), collName, {}, true /* awaitCompletion */);

// Verify that the new primary logs an error with missing _id index.
checkHealthLog(newSecondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
checkHealthLog(newPrimaryHealthLog, logQueries.missingIdIndex, 1);
checkHealthLog(newPrimaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1);

rst.stopSet();
