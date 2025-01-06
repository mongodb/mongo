/**
 * Tests dbCheck with old format index keys and verifies extra index keys on primary only are
 * checked.
 *
 * @tags: [
 *   requires_fcv_81
 * ]
 */

import {
    DbCheckOldFormatKeysTest,
    defaultNumDocs
} from "jstests/multiVersion/libs/dbcheck_old_format_keys_test.js";
import {
    assertCompleteCoverage,
    checkHealthLog,
    forEachNonArbiterSecondary,
    logQueries,
    resetSnapshotSize,
    runDbCheck,
    setSnapshotSize,
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "testDB";
const collName = "oldFormatIndexKeyTestColl";

const dbCheckTest = new DbCheckOldFormatKeysTest({});
dbCheckTest.insertOldFormatKeyStrings(dbName, collName);
dbCheckTest.upgradeRst();
dbCheckTest.createExtraKeysRecordDoesNotMatchOnPrimary(dbName, collName);

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();

forEachNonArbiterSecondary(rst, function(node) {
    assert.eq(node.getDB(dbName).getCollection(collName).find({}).count(), defaultNumDocs);
});

// Check that batching and snapshotting works properly.
const batchSize = 7;
const snapshotSize = 2;
setSnapshotSize(rst, snapshotSize);

jsTestLog("Running dbCheck extraIndexKeysCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "extraIndexKeysCheck", secondaryIndex: "a_1", maxDocsPerBatch: batchSize},
           true /*awaitCompletion*/);

// Check for record does not match on primary.
const numBatches = Math.ceil(defaultNumDocs / batchSize);
const primaryHealthLog = primary.getDB("local").system.healthlog;
checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, defaultNumDocs);
checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, defaultNumDocs);

checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, numBatches);
assertCompleteCoverage(primaryHealthLog,
                       defaultNumDocs,
                       "a" /*indexName*/,
                       null /* docSuffix */,
                       null /* start */,
                       null /* end */);

// Verify that the correct num of inconsistent batch entries were found on secondary.
forEachNonArbiterSecondary(rst, function(node) {
    // The first batch should return IndexKeyOrderViolation error because the keys in this unique
    // index are the same (set to {a: null}).
    checkHealthLog(
        node.getDB("local").system.healthlog, logQueries.inconsistentBatchQuery, numBatches - 1);
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.indexKeyOrderViolationQuery, 1);
    checkHealthLog(
        node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, numBatches);

    assertCompleteCoverage(node.getDB("local").system.healthlog,
                           defaultNumDocs,
                           "a" /*indexName*/,
                           null /* docSuffix */,
                           null /* start */,
                           null /* end */);
});

resetSnapshotSize(rst);
dbCheckTest.stop();
