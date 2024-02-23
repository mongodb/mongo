/**
 * Tests that the dbCheck command's extra index keys check correctly finds extra or inconsistent
 * index keys.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkHealthLog,
    logQueries,
    resetAndInsert,
    runDbCheck,
} from "jstests/replsets/libs/dbcheck_utils.js";
(function() {
"use strict";

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

const dbName = "dbCheckExtraIndexKeys";
const collName = "dbCheckExtraIndexKeysColl";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {
        setParameter:
            {logComponentVerbosity: tojson({command: 3}), dbCheckHealthLogEveryNBatches: 1},
    }
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthLog = primary.getDB("local").system.healthlog;
const secondaryHealthLog = secondary.getDB("local").system.healthlog;
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);
assert.commandWorked(primaryDB.createCollection(collName));
const defaultMaxDocsPerBatch = 100;
const defaultSnapshotSize = 1000;
const writeConcern = {
    w: 'majority'
};

const debugBuild = primaryDB.adminCommand('buildInfo').debug;

assert.commandWorked(primaryDB.adminCommand({
    setParameter: 1,
    maxDbCheckMBperSec:
        0 /* Turn off throttling because stalls and pauses sometimes break this test */
}));

function setSnapshotSize(snapshotSize) {
    assert.commandWorked(primaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
    assert.commandWorked(secondaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
}
function resetSnapshotSize() {
    setSnapshotSize(defaultSnapshotSize);
}

function getNumDocsChecked(nDocsInserted, start, end) {
    let actualNumDocs = nDocsInserted;
    // Assuming docs are inserted from 0 to nDocsInserted - 1.
    if (start != null && start > 0) {
        actualNumDocs = actualNumDocs - start;
    }
    if (end != null && end < nDocsInserted - 1) {
        actualNumDocs = actualNumDocs - (nDocsInserted - 1 - end);
    }
    return actualNumDocs;
}

function checkNumBatchesAndSnapshots(
    healthLog, nDocs, batchSize, snapshotSize, inconsistentBatch = false) {
    const expectedNumBatches = Math.ceil(nDocs / batchSize);

    let query = logQueries.infoBatchQuery;
    if (inconsistentBatch) {
        query = {"severity": "error", "msg": "dbCheck batch inconsistent"};
    }

    checkHealthLog(healthLog, query, expectedNumBatches);

    if (debugBuild) {
        let expectedNumSnapshots = expectedNumBatches;
        if (snapshotSize < batchSize) {
            const snapshotsPerBatch = Math.ceil(batchSize / snapshotSize);
            const lastBatchSize = nDocs % batchSize == 0 ? batchSize : nDocs % batchSize;
            const lastBatchSnapshots = Math.ceil(lastBatchSize / snapshotSize);

            expectedNumSnapshots =
                ((expectedNumBatches - 1) * snapshotsPerBatch) + lastBatchSnapshots;
        }
        const actualNumSnapshots =
            rawMongoProgramOutput()
                .split(/7844808.*Catalog snapshot for extra index keys check ending/)
                .length -
            1;
        assert.eq(actualNumSnapshots,
                  expectedNumSnapshots,
                  "expected " + expectedNumSnapshots +
                      " catalog snapshots during extra index keys check, found " +
                      actualNumSnapshots);
    }
}

function noExtraIndexKeys(
    nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start = null, end = null) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that a valid index will not result in any health log entries with " + nDocs +
              " docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize +
              ", skipLookupForExtraKeys: " + skipLookupForExtraKeys + ", docSuffix: " + docSuffix +
              ", start:" + start + ", end:" + end);

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(snapshotSize);
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: skipLookupForExtraKeys
    };
    if (start != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, start: {a: start.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, start: {a: start} }
        }
    }
    if (end != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, end: {a: end.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, end: {a: end} }
        }
    }
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    const nDocsChecked = getNumDocsChecked(nDocs, start, end, docSuffix);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocsChecked, batchSize, snapshotSize);

    resetSnapshotSize();
}

function recordNotFound(
    nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start = null, end = null) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that an extra key will generate a health log entry with " + nDocs +
              " docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize +
              ", skipLookupForExtraKeys: " + skipLookupForExtraKeys + ", docSuffix: " + docSuffix +
              ", start:" + start + ", end:" + end);

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    const primaryColl = primaryDB.getCollection(collName);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(snapshotSize);
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs);

    const skipUnindexingDocumentWhenDeletedPrimary =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    const skipUnindexingDocumentWhenDeletedSecondary =
        configureFailPoint(secondaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});

    jsTestLog("Deleting docs");
    assert.commandWorked(primaryColl.deleteMany({}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: skipLookupForExtraKeys
    };
    if (start != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, start: {a: start.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, start: {a: start} }
        }
    }
    if (end != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, end: {a: end.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, end: {a: end} }
        }
    }
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    const nDocsChecked = getNumDocsChecked(nDocs, start, end, docSuffix);

    if (skipLookupForExtraKeys) {
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    } else {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);
    }

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocsChecked, batchSize, snapshotSize);

    skipUnindexingDocumentWhenDeletedPrimary.off();
    skipUnindexingDocumentWhenDeletedSecondary.off();
    resetSnapshotSize();
}

function recordDoesNotMatch(
    nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start = null, end = null) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that a key with a record that does not contain the expected keystring will generate a health log entry with " +
        nDocs + " docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize +
        ", skipLookupForExtraKeys: " + skipLookupForExtraKeys + ", docSuffix: " + docSuffix +
        ", start:" + start + ", end:" + end);

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    const primaryColl = primaryDB.getCollection(collName);

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(snapshotSize);
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs);

    const skipUpdatingIndexDocumentPrimary =
        configureFailPoint(primaryDB, "skipUpdatingIndexDocument", {indexName: "a_1"});
    const skipUpdatingIndexDocumentSecondary =
        configureFailPoint(secondaryDB, "skipUpdatingIndexDocument", {indexName: "a_1"});

    jsTestLog("Updating docs to remove index key field");

    assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: skipLookupForExtraKeys
    };
    if (start != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, start: {a: start.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, start: {a: start} }
        }
    }
    if (end != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, end: {a: end.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, end: {a: end} }
        }
    }
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    const nDocsChecked = getNumDocsChecked(nDocs, start, end, docSuffix);
    if (skipLookupForExtraKeys) {
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    } else {
        jsTestLog("Checking primary for record does not match error");
        checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nDocsChecked);

        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);
    }
    jsTestLog(
        "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocsChecked, batchSize, snapshotSize);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
    resetSnapshotSize();
}

function hashingInconsistentExtraKeyOnPrimary(
    nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start = null, end = null) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that an extra key on only the primary will log an inconsistent batch health log entry: " +
        nDocs + "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize +
        ", skipLookupForExtraKeys: " + skipLookupForExtraKeys + ", docSuffix: " + docSuffix +
        ", start:" + start + ", end:" + end);

    setSnapshotSize(snapshotSize);
    const primaryColl = primaryDB.getCollection(collName);
    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs);

    // Set up inconsistency.
    const skipUnindexingDocumentWhenDeleted =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting docs");
    assert.commandWorked(primaryColl.deleteMany({}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 0);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: skipLookupForExtraKeys
    };
    if (start != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, start: {a: start.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, start: {a: start} }
        }
    }
    if (end != null) {
        if (docSuffix) {
            dbCheckParameters = {...dbCheckParameters, end: {a: end.toString() + docSuffix} }
        } else {
            dbCheckParameters = {...dbCheckParameters, end: {a: end} }
        }
    }
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    const nDocsChecked = getNumDocsChecked(nDocs, start, end, docSuffix);
    if (skipLookupForExtraKeys) {
        jsTestLog("Checking primary for errors");
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

        jsTestLog("Checking secondary for record not found error, should have 0");
        checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);
    } else {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocsChecked);
        jsTestLog("Checking secondary for record not found error, should have 0");
        checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);

        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);
    }

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of inconsistent batches on secondary");
    checkNumBatchesAndSnapshots(
        secondaryHealthLog, nDocsChecked, batchSize, snapshotSize, true /* inconsistentBatch */);

    skipUnindexingDocumentWhenDeleted.off();
    resetSnapshotSize();
}

function runMainTests(nDocs, batchSize, snapshotSize, docSuffix, start = null, end = null) {
    [true, false].forEach((skipLookupForExtraKeys) => {
        noExtraIndexKeys(
            nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start, end);
        recordDoesNotMatch(
            nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start, end);
        recordNotFound(
            nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start, end);
        hashingInconsistentExtraKeyOnPrimary(
            nDocs, batchSize, snapshotSize, skipLookupForExtraKeys, docSuffix, start, end);
    });
}

// Test with integer index entries (1, 2, 3, etc.), single character string entries ("1",
// "2", "3", etc.), and long string entries ("1aaaaaaaaaa")
[null,
 "",
 "aaaaaaaaaa"]
    .forEach((docSuffix) => {
        // Test with docs < batch size
        runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix);

        // Test with docs > batch size.
        runMainTests(1000, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix);

        // Test with snapshot size < batch size
        runMainTests(1000, 99 /* batchSize */, 19 /* snapshotSize */, docSuffix);

        // Pass in start/end parameters with full range.
        runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix, 0, 9);
        // Test a specific range.
        runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix, 2, 8);
        // Start < first doc (a: 0)
        runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix, -1, 8);
        // End > last doc (a: 9)
        if (docSuffix) {
            runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix, "3", "9z");
        } else {
            runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize, docSuffix, 3, 10);
        }
    });

// Test with start/end parameters and multiple batches/snapshots
// Test with specific range in the middle of the index and snapshotSize < batchSize.
runMainTests(1000, 99 /* batchSize */, 98 /* snapshotSize*/, null /*docSuffix*/, 99, 901);
// Test with start < first doc and multiple batches/snapshots.
runMainTests(1000, defaultMaxDocsPerBatch, 19 /* snapshotSize */, null /*docSuffix*/, -1, 301);
// Test with end > last doc and multiple batches/snapshots.
runMainTests(1000, 99 /* batchSize */, 20 /* snapshotSize */, null /*docSuffix*/, 699, 1000);

// TODO SERVER-79846:
// * Test progress meter/stats are correct

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
})();
