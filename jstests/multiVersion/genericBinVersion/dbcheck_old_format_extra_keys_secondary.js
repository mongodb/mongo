/**
 * Tests dbCheck with old format index keys and verifies extra index keys on secondary only are
 * caught.
 *
 * @tags: [
 *   requires_fcv_80
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

// Delete all docs.
dbCheckTest.createExtraKeysRecordNotFoundOnSecondary(
    dbName, collName, {$and: [{a: {$gte: 0}}, {a: {$lte: defaultNumDocs - 1}}]});

forEachNonArbiterSecondary(rst, function(node) {
    assert.eq(node.getDB(dbName).getCollection(collName).find({}).count(), 0);
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
assertCompleteCoverage(primaryHealthLog,
                       defaultNumDocs,
                       "a" /*indexName*/,
                       null /* docSuffix */,
                       null /* start */,
                       null /* end */);

// Check for inconsistent batch on secondary.
forEachNonArbiterSecondary(rst, function(node) {
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.inconsistentBatchQuery, 1);
    checkHealthLog(node.getDB("local").system.healthlog, logQueries.allErrorsOrWarningsQuery, 1);
    assertCompleteCoverage(node.getDB("local").system.healthlog,
                           defaultNumDocs,
                           "a" /*indexName*/,
                           null /* docSuffix */,
                           null /* start */,
                           null /* end */);
});

resetSnapshotSize(rst);
dbCheckTest.stop();
