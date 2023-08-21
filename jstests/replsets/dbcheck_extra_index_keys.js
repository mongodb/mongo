/**
 * Tests that the dbCheck command's extra index keys check correctly finds extra or inconsistent
 * index keys.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint, kDefaultWaitForFailPointTimeout} from "jstests/libs/fail_point_util.js";
import {
    awaitDbCheckCompletion,
    checkHealthLog,
    resetAndInsert,
    runDbCheck
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
const db = primary.getDB(dbName);
const defaultNumDocs = 1000;
const defaultMaxDocsPerBatch = 100;
const defaultSnapshotSize = 1000;
const writeConcern = {
    w: 'majority'
};

const debugBuild = db.adminCommand('buildInfo').debug;

function checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize) {
    const expectedNumBatches = Math.ceil(nDocs / batchSize);

    let query = {
        "severity": "info",
        "operation": "dbCheckBatch",
    };

    jsTestLog("Checking primary for num batches");
    checkHealthLog(primaryHealthLog, query, expectedNumBatches);

    if (debugBuild) {
        const expectedNumSnapshots =
            Math.ceil((batchSize / Math.min(batchSize, snapshotSize)) * expectedNumBatches);
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

function indexNotFoundBeforeDbCheck() {
    jsTestLog("Testing that an index that doesn't exist will generate a health log entry");

    resetAndInsert(replSet, db, collName, defaultNumDocs);
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because index no longer exists"
    };
    checkHealthLog(primaryHealthLog, query, 1);
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {severity: "error"};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function indexNotFoundDuringDbCheck() {
    jsTestLog("Testing that an index that doesn't exist will generate a health log entry");

    resetAndInsert(replSet, db, collName, defaultNumDocs);
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeReverseLookupCatalogSnapshot =
        configureFailPoint(db, "hangBeforeReverseLookupCatalogSnapshot");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters);

    hangBeforeReverseLookupCatalogSnapshot.wait();

    assert.commandWorked(db.runCommand({dropIndexes: collName, index: "a_1"}));

    hangBeforeReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, db);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because index no longer exists"
    };
    jsTestLog("checking primary");
    checkHealthLog(primaryHealthLog, query, 1);
    jsTestLog("checking secondary");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {severity: "error"};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function allIndexKeysNotFoundDuringDbCheck(nDocs) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that if all the index keys are deleted during dbcheck we don't error");

    resetAndInsert(replSet, db, collName, nDocs);
    const coll = db.getCollection(collName);
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangAfterReverseLookupCatalogSnapshot =
        configureFailPoint(db, "hangAfterReverseLookupCatalogSnapshot");

    // Batch size 1.
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 1,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters);

    hangAfterReverseLookupCatalogSnapshot.wait();

    jsTestLog("Removing all docs");
    assert.commandWorked(coll.deleteMany({}));
    replSet.awaitReplication();
    assert.eq(coll.find({}).count(), 0);

    hangAfterReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, db);
    let query = {$or: [{"severity": "warning"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);

    if (debugBuild) {
        assert(rawMongoProgramOutput().match(
                   /7844803.*could not find lookupStartKeyStringBson in index/),
               "expected 'could not find lookupStartKeyStringBson in index' log");
    }
}

function keyNotFoundDuringDbCheck(nDocs) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that if a key is deleted during dbcheck we don't error");

    resetAndInsert(replSet, db, collName, nDocs);
    const coll = db.getCollection(collName);
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangAfterReverseLookupCatalogSnapshot =
        configureFailPoint(db, "hangAfterReverseLookupCatalogSnapshot");

    // Batch size 1.
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 1,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters);

    hangAfterReverseLookupCatalogSnapshot.wait();
    jsTestLog("Removing one doc");
    assert.commandWorked(coll.deleteOne({"a": 1}));
    replSet.awaitReplication();
    assert.eq(coll.find({}).count(), nDocs - 1);

    // TODO SERVER-80257: Replace using this failpoint with test commands instead.
    const skipUpdatingIndexDocument =
        configureFailPoint(db, "skipUpdatingIndexDocument", {indexName: "a_1"});

    jsTestLog("Updating docs to remove index key field");
    assert.commandWorked(coll.updateMany({}, {$unset: {"a": ""}}));
    replSet.awaitReplication();
    assert.eq(coll.find({a: {$exists: true}}).count(), 0);

    hangAfterReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, db);
    let query = {
        "severity": "error",
        "msg":
            "found index key entry with corresponding document/keystring set that does not contain the expected key string"
    };
    jsTestLog("checking primary");
    checkHealthLog(primaryHealthLog, query, nDocs - 2);
    jsTestLog("checking secondary");
    checkHealthLog(secondaryHealthLog, query, 0);

    skipUpdatingIndexDocument.off();
}

function noExtraIndexKeys(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that a valid index will not result in any health log entries with " + nDocs +
              "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, db, collName, nDocs);
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    assert.commandWorked(primary.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
    replSet.awaitReplication();
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters, true /* awaitCompletion */);

    let query = {$or: [{"severity": "warning"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);

    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);

    assert.commandWorked(db.adminCommand({
        "setParameter": 1,
        "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": defaultSnapshotSize
    }));
}

function recordNotFound(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that an extra key will generate a health log entry with " + nDocs +
              "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, db, collName, nDocs);
    const coll = db.getCollection(collName);
    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    assert.commandWorked(primary.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
    replSet.awaitReplication();
    assert.eq(coll.find({}).count(), nDocs);

    const skipUnindexingDocumentWhenDeleted =
        configureFailPoint(db, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});

    jsTestLog("Deleting docs");
    assert.commandWorked(coll.deleteMany({}));

    replSet.awaitReplication();
    assert.eq(coll.find({}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        "severity": "error",
        "msg": "found extra index key entry without corresponding document"
    };
    jsTestLog("checking primary");
    checkHealthLog(primaryHealthLog, query, nDocs);
    jsTestLog("checking secondary");
    checkHealthLog(secondaryHealthLog, query, 0);

    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);

    skipUnindexingDocumentWhenDeleted.off();
    assert.commandWorked(db.adminCommand({
        "setParameter": 1,
        "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": defaultSnapshotSize
    }));
}

function recordDoesNotMatch(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that a key with a record that does not contain the expected keystring will generate a health log entry with " +
        nDocs + "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, db, collName, nDocs);
    const coll = db.getCollection(collName);

    assert.commandWorked(db.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    assert.commandWorked(primary.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
    replSet.awaitReplication();
    assert.eq(coll.find({}).count(), nDocs);

    const skipUpdatingIndexDocument =
        configureFailPoint(db, "skipUpdatingIndexDocument", {indexName: "a_1"});

    jsTestLog("Updating docs to remove index key field");
    assert.commandWorked(coll.updateMany({}, {$unset: {"a": ""}}));

    replSet.awaitReplication();
    assert.eq(coll.find({a: {$exists: true}}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, db, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        "severity": "error",
        "msg":
            "found index key entry with corresponding document/keystring set that does not contain the expected key string"
    };
    jsTestLog("checking primary");
    checkHealthLog(primaryHealthLog, query, nDocs);
    jsTestLog("checking secondary");
    checkHealthLog(secondaryHealthLog, query, 0);

    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);

    skipUpdatingIndexDocument.off();
    assert.commandWorked(db.adminCommand({
        "setParameter": 1,
        "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": defaultSnapshotSize
    }));
}

indexNotFoundBeforeDbCheck();
indexNotFoundDuringDbCheck();
allIndexKeysNotFoundDuringDbCheck(10);
keyNotFoundDuringDbCheck(10);

function runMainTests(nDocs, batchSize, snapshotSize) {
    noExtraIndexKeys(nDocs, batchSize, snapshotSize);
    recordDoesNotMatch(nDocs, batchSize, snapshotSize);
    recordNotFound(nDocs, batchSize, snapshotSize);
}

// Test with docs < batch size
runMainTests(10, defaultMaxDocsPerBatch, defaultSnapshotSize);

// Test with docs > batch size.
runMainTests(1000, defaultMaxDocsPerBatch, defaultSnapshotSize);

// Test with snapshot size < batch size
runMainTests(1000, defaultMaxDocsPerBatch, 20 /* snapshotSize */);

// TODO SERVER-79849 Add testing for:
// * Reached bytes per batch ends batch.
// * Total bytes seen over limit ends dbcheck
// * Total keys seen over limit ends dbcheck
// * Docs in current interval over limit slows rate.

// TODO SERVER-79846:
// * Test progress meter/stats are correct

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
})();
