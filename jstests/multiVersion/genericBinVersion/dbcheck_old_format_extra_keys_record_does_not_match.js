/**
 * Tests dbCheck with old format index keys and verifies extra index keys with record does not match
 * error are checked.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
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
dbCheckTest.createExtraKeysRecordDoesNotMatchOnAllNodes(dbName, collName);

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();

forEachNonArbiterSecondary(rst, function(node) {
    assert.eq(node.getDB(dbName).getCollection(collName).find({}).count(), defaultNumDocs);
});

// Check that batching and snapshotting works properly.
const batchSize = 3;
const snapshotSize = 3;
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

// Verify that no error or warning health log entries were logged on
// secondary.
forEachNonArbiterSecondary(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.infoBatchQuery, numBatches);
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 0);
    assertCompleteCoverage(node.getDB("local").system.healthlog,
                           defaultNumDocs,
                           "a" /*indexName*/,
                           null /* docSuffix */,
                           null /* start */,
                           null /* end */);
});

resetSnapshotSize(rst);
dbCheckTest.stop();
