/**
 * Tests dbCheck with old format index keys and verifies extra index keys on secondary only are
 * caught.
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

const rst = dbCheckTest.getRst();
const primary = dbCheckTest.getPrimary();

// TODO (SERVER-86323): Make sure this also works if we delete all docs except one
// Delete all docs except first and last so that dbcheck still runs for the whole range.
dbCheckTest.createExtraKeysRecordNotFoundOnSecondary(
    dbName, collName, {$and: [{a: {$gt: 0}}, {a: {$lt: defaultNumDocs - 1}}]});

forEachNonArbiterSecondary(rst, function(node) {
    assert.eq(node.getDB(dbName).getCollection(collName).find({}).count(), 2);
});

const batchSize = 7;
const snapshotSize = 2;
setSnapshotSize(rst, snapshotSize);

jsTestLog("Running dbCheck extraIndexKeysCheck");
runDbCheck(rst,
           primary.getDB(dbName),
           collName,
           {validateMode: "extraIndexKeysCheck", secondaryIndex: "a_1", maxDocsPerBatch: batchSize},
           true /*awaitCompletion*/);

// Check for no errors on primary.
const primaryHealthLog = primary.getDB("local").system.healthlog;
checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 1);
assertCompleteCoverage(
    primaryHealthLog, defaultNumDocs, null /* docSuffix */, null /* start */, null /* end */);

// Check for inconsistent batch on secondary.
forEachNonArbiterSecondary(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.inconsistentBatchQuery, 1);
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 1);
    assertCompleteCoverage(node.getDB("local").system.healthlog,
                           defaultNumDocs,
                           null /* docSuffix */,
                           null /* start */,
                           null /* end */,
                           true /* inconsistentBatch */);
});

resetSnapshotSize(rst);
dbCheckTest.stop();
