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
const primaryDB = primary.getDB(dbName);
const secondaryDB = secondary.getDB(dbName);
assert.commandWorked(primaryDB.createCollection(collName));
const defaultNumDocs = 1000;
const defaultMaxDocsPerBatch = 100;
const defaultSnapshotSize = 1000;
const writeConcern = {
    w: 'majority'
};

const debugBuild = primaryDB.adminCommand('buildInfo').debug;

function setSnapshotSize(snapshotSize) {
    assert.commandWorked(primaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
    assert.commandWorked(secondaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxExtraIndexKeysReverseLookupPerSnapshot": snapshotSize}));
}
function resetSnapshotSize() {
    setSnapshotSize(defaultSnapshotSize);
}

function checkNumBatchesAndSnapshots(
    healthLog, nDocs, batchSize, snapshotSize, inconsistentBatch = false) {
    const expectedNumBatches = Math.ceil(nDocs / batchSize);

    let query = {
        "severity": "info",
        "operation": "dbCheckBatch",
    };
    if (inconsistentBatch) {
        query = {"severity": "error", "msg": "dbCheck batch inconsistent"};
    }

    checkHealthLog(healthLog, query, expectedNumBatches);

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

function collNotFoundBeforeDbCheck() {
    jsTestLog(
        "Testing that an collection that doesn't exist before dbcheck will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);

    const hangBeforeExtraIndexKeysCheck =
        configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysCheck");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeExtraIndexKeysCheck.wait();

    assert.commandWorked(primaryDB.runCommand({drop: collName}));
    replSet.awaitReplication();

    hangBeforeExtraIndexKeysCheck.off();
    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because collection no longer exists"
    };
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found before db check, we won't create any oplog entry.
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function indexNotFoundBeforeDbCheck() {
    jsTestLog(
        "Testing that an index that doesn't exist before dbcheck will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because index no longer exists"
    };
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found before db check, we won't create any oplog entry.
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function collNotFoundDuringReverseLookup() {
    jsTestLog(
        "Testing that a collection that doesn't exist during reverse lookup will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeReverseLookupCatalogSnapshot =
        configureFailPoint(primaryDB, "hangBeforeReverseLookupCatalogSnapshot");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeReverseLookupCatalogSnapshot.wait();

    assert.commandWorked(primaryDB.runCommand({drop: collName}));
    replSet.awaitReplication();

    hangBeforeReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because collection no longer exists"
    };
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found during reverse lookup, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function indexNotFoundDuringReverseLookup() {
    jsTestLog(
        "Testing that an index that doesn't exist during reverse lookup will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeReverseLookupCatalogSnapshot =
        configureFailPoint(primaryDB, "hangBeforeReverseLookupCatalogSnapshot");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeReverseLookupCatalogSnapshot.wait();

    assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

    hangBeforeReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because index no longer exists"
    };
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found during reverse lookup, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function collNotFoundDuringHashing() {
    jsTestLog(
        "Testing that a collection that doesn't exist during hashing will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeExtraIndexKeysHashing =
        configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeExtraIndexKeysHashing.wait();

    assert.commandWorked(primaryDB.runCommand({drop: collName}));
    replSet.awaitReplication();

    hangBeforeExtraIndexKeysHashing.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because collection no longer exists"
    };
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found during hashing, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function indexNotFoundDuringHashing() {
    jsTestLog(
        "Testing that an index that doesn't exist during hashing will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeExtraIndexKeysHashing =
        configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeExtraIndexKeysHashing.wait();

    assert.commandWorked(primaryDB.runCommand({dropIndexes: collName, index: "a_1"}));

    hangBeforeExtraIndexKeysHashing.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg": "abandoning dbCheck extra index keys check because index no longer exists"
    };
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, query, 1);
    // If index not found during hashing, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {$or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);
}

function keysChangedBeforeHashing() {
    jsTestLog(
        "Testing that if keys within batch boundaries change in between reverse lookup and hashing we won't error.");

    resetAndInsert(replSet, primaryDB, collName, 10);
    const primaryColl = primaryDB.getCollection(collName);
    primaryColl.deleteOne({a: 3});

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangBeforeExtraIndexKeysHashing =
        configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

    // First batch should 0, 1, 2, 4, 5. Batch boundaries will be [0, 5].
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 5,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangBeforeExtraIndexKeysHashing.wait();

    // Actual batch will be 1, 2, 3, 4.
    primaryColl.deleteOne({a: 0});
    primaryColl.insertOne({a: 3});
    primaryColl.deleteOne({a: 5});

    hangBeforeExtraIndexKeysHashing.off();

    awaitDbCheckCompletion(replSet, primaryDB);

    let query = {$or: [{"severity": "warning"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {
        "severity": "info",
        "operation": "dbCheckBatch",
    };
    jsTestLog("Checking for correct number of batches on primary");
    checkHealthLog(primaryHealthLog, query, 2);
    jsTestLog("Checking for correct number of batches on secondary");
    checkHealthLog(secondaryHealthLog, query, 2);
}

function allIndexKeysNotFoundDuringReverseLookup(nDocs) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that if all the index keys are deleted during reverse lookup we log a warning and exit dbcheck");

    resetAndInsert(replSet, primaryDB, collName, nDocs);
    const primaryColl = primaryDB.getCollection(collName);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangAfterReverseLookupCatalogSnapshot =
        configureFailPoint(primaryDB, "hangAfterReverseLookupCatalogSnapshot");

    // Batch size 1.
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 1,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangAfterReverseLookupCatalogSnapshot.wait();

    jsTestLog("Removing all docs");
    assert.commandWorked(primaryColl.deleteMany({}));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 0);

    hangAfterReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        severity: "warning",
        "msg":
            "abandoning dbCheck extra index keys check because there are no keys left in the index"
    };
    checkHealthLog(primaryHealthLog, query, 1);
    // If all index keys are deleted during reverse lookup, we won't create any oplog entry.
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {"severity": "info", "operation": "dbCheckBatch"};
    checkHealthLog(primaryHealthLog, query, 1);
    checkHealthLog(secondaryHealthLog, query, 1);
    query = {"severity": "error"};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);

    if (debugBuild) {
        assert(rawMongoProgramOutput().match(/7844803.*could not find any keys in index/),
               "expected 'could not find any keys in index' log");
    }
}

function keyNotFoundDuringReverseLookup(nDocs) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that if a key is deleted during reverse lookup we continue with db check");

    resetAndInsert(replSet, primaryDB, collName, nDocs);
    const primaryColl = primaryDB.getCollection(collName);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    const hangAfterReverseLookupCatalogSnapshot =
        configureFailPoint(primaryDB, "hangAfterReverseLookupCatalogSnapshot");

    // Batch size 1.
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 1,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    hangAfterReverseLookupCatalogSnapshot.wait();
    jsTestLog("Removing one doc");
    assert.commandWorked(primaryColl.deleteOne({"a": 1}));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs - 1);

    // TODO SERVER-80257: Replace using this failpoint with test commands instead.
    const skipUpdatingIndexDocumentPrimary =
        configureFailPoint(primaryDB, "skipUpdatingIndexDocument", {indexName: "a_1"});
    const skipUpdatingIndexDocumentSecondary =
        configureFailPoint(secondaryDB, "skipUpdatingIndexDocument", {indexName: "a_1"});

    jsTestLog("Updating docs to remove index key field");
    assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);

    hangAfterReverseLookupCatalogSnapshot.off();

    awaitDbCheckCompletion(replSet, primaryDB);
    let query = {
        "severity": "error",
        "msg":
            "found index key entry with corresponding document/keystring set that does not contain the expected key string"
    };
    jsTestLog("checking primary health log");
    // First doc was valid, second doc was not found but we continue with dbcheck.
    checkHealthLog(primaryHealthLog, query, nDocs - 2);
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {"msg": "dbcheck extra keys check batch on primary"};
    jsTestLog("checking primary for correct num of health logs");
    checkHealthLog(primaryHealthLog, query, nDocs - 1);

    query = {"msg": "dbCheck batch consistent"};
    jsTestLog("checking secondary for correct num of health logs");
    checkHealthLog(secondaryHealthLog, query, nDocs - 1);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
}

function noExtraIndexKeys(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that a valid index will not result in any health log entries with " + nDocs +
              "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, primaryDB, collName, nDocs);
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
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    let query = {$or: [{"severity": "warning"}, {"severity": "error"}]};
    checkHealthLog(primaryHealthLog, query, 0);
    checkHealthLog(secondaryHealthLog, query, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocs, batchSize, snapshotSize);

    resetSnapshotSize();
}

function recordNotFound(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that an extra key will generate a health log entry with " + nDocs +
              "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, primaryDB, collName, nDocs);
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
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        "severity": "error",
        "msg": "found extra index key entry without corresponding document"
    };
    jsTestLog("Checking primary for record not found error");
    checkHealthLog(primaryHealthLog, query, nDocs);
    query = {"severity": "error"};
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, query, nDocs);
    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, query, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocs, batchSize, snapshotSize);

    skipUnindexingDocumentWhenDeletedPrimary.off();
    skipUnindexingDocumentWhenDeletedSecondary.off();
    resetSnapshotSize();
}

function recordDoesNotMatch(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that a key with a record that does not contain the expected keystring will generate a health log entry with " +
        nDocs + "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    resetAndInsert(replSet, primaryDB, collName, nDocs);
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
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        "severity": "error",
        "msg":
            "found index key entry with corresponding document/keystring set that does not contain the expected key string"
    };
    jsTestLog("Checking primary for record does not match error");
    checkHealthLog(primaryHealthLog, query, nDocs);

    query = {"severity": "error"};
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, query, nDocs);
    jsTestLog(
        "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, query, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocs, batchSize, snapshotSize);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
    resetSnapshotSize();
}

function hashingInconsistentExtraKeyOnPrimary(nDocs, batchSize, snapshotSize) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that an extra key on only the primary will log an inconsistent batch health log entry: " +
        nDocs + "docs, batchSize: " + batchSize + ", snapshotSize: " + snapshotSize);

    setSnapshotSize(snapshotSize);
    const primaryColl = primaryDB.getCollection(collName);
    resetAndInsert(replSet, primaryDB, collName, nDocs);
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
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    let query = {
        "severity": "error",
        "msg": "found extra index key entry without corresponding document"
    };
    jsTestLog("Checking primary for record not found error");
    checkHealthLog(primaryHealthLog, query, nDocs);
    jsTestLog("Checking secondary for record not found error, should have 0");
    checkHealthLog(secondaryHealthLog, query, 0);

    query = {"severity": "error"};
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, query, nDocs);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, batchSize, snapshotSize);
    jsTestLog("Checking for correct number of inconsistent batches on secondary");
    checkNumBatchesAndSnapshots(
        secondaryHealthLog, nDocs, batchSize, snapshotSize, true /* inconsistentBatch */);

    skipUnindexingDocumentWhenDeleted.off();
    resetSnapshotSize();
}

indexNotFoundBeforeDbCheck();
indexNotFoundDuringHashing();
indexNotFoundDuringReverseLookup();
collNotFoundBeforeDbCheck();
collNotFoundDuringHashing();
collNotFoundDuringReverseLookup();
keysChangedBeforeHashing();
indexNotFoundDuringReverseLookup();
allIndexKeysNotFoundDuringReverseLookup(10);
keyNotFoundDuringReverseLookup(10);

function runMainTests(nDocs, batchSize, snapshotSize) {
    noExtraIndexKeys(nDocs, batchSize, snapshotSize);
    recordDoesNotMatch(nDocs, batchSize, snapshotSize);
    recordNotFound(nDocs, batchSize, snapshotSize);
    hashingInconsistentExtraKeyOnPrimary(nDocs, batchSize, snapshotSize);
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
