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

const allErrorsOrWarningsQuery = {
    $or: [{"severity": "warning"}, {"severity": "error"}]
};
const recordNotFoundQuery = {
    "severity": "error",
    "msg": "found extra index key entry without corresponding document",
    "data.context.indexSpec": {$exists: true}
};
const recordDoesNotMatchQuery = {
    "severity": "error",
    "msg":
        "found index key entry with corresponding document/keystring set that does not contain the expected key string",
    "data.context.indexSpec": {$exists: true}
};
const collNotFoundWarningQuery = {
    severity: "warning",
    "msg": "abandoning dbCheck extra index keys check because collection no longer exists"
};
const indexNotFoundWarningQuery = {
    severity: "warning",
    "msg": "abandoning dbCheck extra index keys check because index no longer exists"
};
const warningQuery = {
    "severity": "warning"
};
const infoOrErrorQuery = {
    $or: [{"severity": "info", "operation": "dbCheckBatch"}, {"severity": "error"}]
};
const infoBatchQuery = {
    "severity": "info",
    "operation": "dbCheckBatch"
}

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

    let query = infoBatchQuery;
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

function collNotFoundBeforeDbCheck(docSuffix) {
    jsTestLog(
        "Testing that an collection that doesn't exist before dbcheck will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);

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
    checkHealthLog(primaryHealthLog, collNotFoundWarningQuery, 1);
    // If index not found before db check, we won't create any oplog entry.
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    // No other info or error logs.
    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
}

function indexNotFoundBeforeDbCheck(docSuffix) {
    jsTestLog(
        "Testing that an index that doesn't exist before dbcheck will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    checkHealthLog(primaryHealthLog, indexNotFoundWarningQuery, 1);
    // If index not found before db check, we won't create any oplog entry.
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    // No other info or error logs.
    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
}

function collNotFoundDuringReverseLookup(docSuffix) {
    jsTestLog(
        "Testing that a collection that doesn't exist during reverse lookup will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);
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
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, collNotFoundWarningQuery, 1);
    // If index not found during reverse lookup, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
}

function indexNotFoundDuringReverseLookup(docSuffix) {
    jsTestLog(
        "Testing that an index that doesn't exist during reverse lookup will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);
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
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, indexNotFoundWarningQuery, 1);
    // If index not found during reverse lookup, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
}

function collNotFoundDuringHashing(docSuffix) {
    jsTestLog(
        "Testing that a collection that doesn't exist during hashing will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);
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

    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, collNotFoundWarningQuery, 1);
    // If index not found during hashing, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
}

function indexNotFoundDuringHashing(docSuffix) {
    jsTestLog(
        "Testing that an index that doesn't exist during hashing will generate a health log entry");

    resetAndInsert(replSet, primaryDB, collName, defaultNumDocs, docSuffix);
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
    jsTestLog("checking primary health log");
    checkHealthLog(primaryHealthLog, indexNotFoundWarningQuery, 1);
    // If index not found during hashing, we won't create any oplog entry for that batch.
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, warningQuery, 0);

    checkHealthLog(primaryHealthLog, infoOrErrorQuery, 0);
    checkHealthLog(secondaryHealthLog, infoOrErrorQuery, 0);
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

    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkHealthLog(primaryHealthLog, infoBatchQuery, 2);
    jsTestLog("Checking for correct number of batches on secondary");
    checkHealthLog(secondaryHealthLog, infoBatchQuery, 2);
}

function allIndexKeysNotFoundDuringReverseLookup(nDocs, docSuffix) {
    clearRawMongoProgramOutput();
    jsTestLog(
        "Testing that if all the index keys are deleted during reverse lookup we log a warning and exit dbcheck");

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
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

    checkHealthLog(primaryHealthLog, infoBatchQuery, 1);
    checkHealthLog(secondaryHealthLog, infoBatchQuery, 1);

    // Only the one warning entry.
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 1);
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

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

    jsTestLog("checking primary health log");
    // First doc was valid, second doc was not found but we continue with dbcheck.
    checkHealthLog(primaryHealthLog, recordDoesNotMatchQuery, nDocs - 2);
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocs - 2);
    jsTestLog("checking secondary health log");
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

    jsTestLog("checking primary for correct num of health logs");
    checkHealthLog(primaryHealthLog, infoBatchQuery, nDocs - 1);

    jsTestLog("checking secondary for correct num of health logs");
    checkHealthLog(secondaryHealthLog, infoBatchQuery, nDocs - 1);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
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

    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

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
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 0);
    } else {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, recordNotFoundQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocsChecked);
    }

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

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
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 0);
    } else {
        jsTestLog("Checking primary for record does not match error");
        checkHealthLog(primaryHealthLog, recordDoesNotMatchQuery, nDocsChecked);

        // No other errors on primary.
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocsChecked);
    }
    jsTestLog(
        "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

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
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, 0);

        jsTestLog("Checking secondary for record not found error, should have 0");
        checkHealthLog(secondaryHealthLog, recordNotFoundQuery, 0);
    } else {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, recordNotFoundQuery, nDocsChecked);
        jsTestLog("Checking secondary for record not found error, should have 0");
        checkHealthLog(secondaryHealthLog, recordNotFoundQuery, 0);

        // No other errors on primary.
        checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocsChecked);
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
        indexNotFoundBeforeDbCheck(docSuffix);
        indexNotFoundDuringHashing(docSuffix);
        indexNotFoundDuringReverseLookup(docSuffix);
        collNotFoundBeforeDbCheck(docSuffix);
        collNotFoundDuringHashing(docSuffix);
        collNotFoundDuringReverseLookup(docSuffix);
        indexNotFoundDuringReverseLookup(docSuffix);
        allIndexKeysNotFoundDuringReverseLookup(10, docSuffix);

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

keysChangedBeforeHashing();
keyNotFoundDuringReverseLookup(10);

// Test with start/end parameters and multiple batches/snapshots
runMainTests(1000, 99 /* batchSize */, 98 /* snapshotSize*/, null /*docSuffix*/, 99, 901);
runMainTests(1000, defaultMaxDocsPerBatch, 19 /* snapshotSize */, null /*docSuffix*/, -1, 301);
runMainTests(1000, 99 /* batchSize */, 20 /* snapshotSize */, null /*docSuffix*/, 699, 1000);

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
