/**
 * Tests that the dbCheck command's extra index keys check follows the rate and overall limits.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkHealthLog,
    clearHealthLog,
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
const defaultSnapshotSize = 1000;
const writeConcern = {
    w: 'majority'
};

const debugBuild = primaryDB.adminCommand('buildInfo').debug;

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
                .split(/7844808.*Catalog snapshot for reverse lookup check ending/)
                .length -
            1;
        assert.eq(actualNumSnapshots,
                  expectedNumSnapshots,
                  "expected " + expectedNumSnapshots +
                      " catalog snapshots during extra index keys check, found " +
                      actualNumSnapshots);
    }
}

function exceedMaxCount(nDocs, batchSize, maxCount, docSuffix) {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that dbcheck terminates after exceeding " + maxCount +
              " number of health log entries: nDocs: " + nDocs + ", batchSize: " + batchSize +
              ", docSuffix: " + docSuffix);

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    const primaryColl = primaryDB.getCollection(collName);

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
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
        maxCount: maxCount
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    // dbcheck will check Math.ceil(maxCount / batchSize) * batchSize documents because we wait
    // until the end of a batch to check if we have reached the overall limits of dbcheck
    // (maxCount/maxSize), so the number of documents checked will always be a multiple of batchSize
    // unless there are fewer documents than maxCount.
    const nDocsChecked = Math.min(nDocs, Math.ceil(maxCount / batchSize) * batchSize);
    jsTestLog("Checking primary for record does not match error");
    checkHealthLog(primaryHealthLog, recordDoesNotMatchQuery, nDocsChecked);
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocsChecked);

    jsTestLog(
        "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, defaultSnapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocsChecked, batchSize, defaultSnapshotSize);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
}

function exceedMaxSize(nDocs, batchSize, maxSize, docSuffix) {
    clearRawMongoProgramOutput();

    // For an index that has `a` as key, the keystring looks like {"": 10}. The size of the encoded
    // keystring is 4 or 5 (the first keystring has size 4 and the rest have size 5), which is used
    // to keep track of bytesSeen.
    // maximum number of documents that should be checked:
    const maxCount = Math.ceil((maxSize - 4) / 5 + 1)

    jsTestLog("Testing that dbcheck terminates after seeing more than " + maxSize +
              " bytes: nDocs: " + nDocs + ", batchSize: " + batchSize +
              ", docSuffix: " + docSuffix);

    resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
    const primaryColl = primaryDB.getCollection(collName);

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
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
        maxSize: maxSize
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    // dbcheck will check Math.ceil(maxCount / batchSize) * batchSize documents because we wait
    // until the end of a batch to check if we have reached the overall limits of dbcheck
    // (maxCount/maxSize), so the number of documents checked will always be a multiple of batchSize
    // unless there are fewer documents than maxCount.
    const nDocsChecked = Math.min(nDocs, Math.ceil(maxCount / batchSize) * batchSize);
    jsTestLog("Checking primary for record does not match error");
    checkHealthLog(primaryHealthLog, recordDoesNotMatchQuery, nDocsChecked);
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocsChecked);

    jsTestLog(
        "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocsChecked, batchSize, defaultSnapshotSize);
    jsTestLog("Checking for correct number of batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog, nDocsChecked, batchSize, defaultSnapshotSize);

    skipUpdatingIndexDocumentPrimary.off();
    skipUpdatingIndexDocumentSecondary.off();
}

function exceedMaxDbCheckMBPerSec() {
    clearRawMongoProgramOutput();
    const nDocs = 5;
    const batchSize = 10;
    jsTestLog(
        "Testing that dbcheck will not read more than maxDbCheckMBperSec (1 MB) per second: nDocs: " +
        5 + ", batchSize: " + batchSize);

    const primaryColl = primaryDB.getCollection(collName);

    primaryDB[collName].drop();
    clearHealthLog(replSet);
    // Insert nDocs, each slightly larger than the maxDbCheckMBperSec value (1MB), which is the
    // default value, while maxBatchTimeMillis is 1 second. Consequently, we will have only 1MB
    // per batch.
    const chars = ['a', 'b', 'c', 'd', 'e'];
    primaryColl.insertMany(
        [...Array(nDocs).keys()].map(x => ({a: chars[x].repeat(1024 * 1024 * 2)})),
        {ordered: false});

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs);

    // Set up inconsistency.
    const skipUnindexingDocumentWhenDeleted =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    assert.commandWorked(primaryColl.deleteMany({}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 0);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), 0);

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        maxBatchTimeMillis: 1000
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    // DbCheck logs (nDocs) batches to account for each batch hitting the time deadline after
    // processing only one document.
    jsTestLog("Checking primary for record not found error");
    checkHealthLog(primaryHealthLog, recordNotFoundQuery, nDocs);
    jsTestLog("Checking secondary for record not found error, should have 0");
    checkHealthLog(secondaryHealthLog, recordNotFoundQuery, 0);
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocs);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, 1 /* batchSize */, defaultSnapshotSize);
    jsTestLog("Checking for correct number of inconsistent batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog,
                                nDocs,
                                1 /* batchSize */,
                                defaultSnapshotSize,
                                true /* inconsistentBatch */);

    skipUnindexingDocumentWhenDeleted.off();
}

function exceedMaxBatchTimeMillis() {
    clearRawMongoProgramOutput();
    const maxBatchTimeMillis = 10;
    const nDocs = 10;
    const batchSize = 20;
    jsTestLog("Testing that dbcheck will not spend more than maxBatchTimeMillis: " +
              maxBatchTimeMillis + " ms per batch: nDocs: " + nDocs + ", batchSize: " + batchSize);

    const primaryColl = primaryDB.getCollection(collName);

    primaryDB[collName].drop();
    clearHealthLog(replSet);
    // Insert nDocs that are almost 1 MB while maxDbCheckMBperSec is set to the default value of 1
    // MB and maxBatchTimeMillis is set to 10 ms. Consequently, we will have 1 doc per batch.
    const chars = ['a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j'];
    primaryColl.insertMany([...Array(nDocs).keys()].map(x => ({a: chars[x].repeat(1024 * 800)})),
                           {ordered: false});

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
        maxBatchTimeMillis: maxBatchTimeMillis
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /*awaitCompletion*/);

    // DbCheck logs (nDocs) batches to account for each batch hitting the time deadline after
    // processing only one document.
    jsTestLog("Checking primary for record not found error");
    checkHealthLog(primaryHealthLog, recordNotFoundQuery, nDocs);
    jsTestLog("Checking secondary for record not found error, should have 0");
    checkHealthLog(secondaryHealthLog, recordNotFoundQuery, 0);
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, allErrorsOrWarningsQuery, nDocs);

    jsTestLog("Checking for correct number of batches on primary");
    checkNumBatchesAndSnapshots(primaryHealthLog, nDocs, 1 /* batchSize */, defaultSnapshotSize);
    jsTestLog("Checking for correct number of inconsistent batches on secondary");
    checkNumBatchesAndSnapshots(secondaryHealthLog,
                                nDocs,
                                1 /* batchSize */,
                                defaultSnapshotSize,
                                true /* inconsistentBatch */);

    skipUnindexingDocumentWhenDeleted.off();
}

exceedMaxDbCheckMBPerSec();

exceedMaxBatchTimeMillis();

// dbcheck will stop when it reads at least maxSize bytes. Each batch would be around batchSize * 5
// bytes and dbcheck will stop after the last batch that reaches maxSize.
// Test with maxSize = 1 batch
exceedMaxSize(100 /* nDocs */, 10 /* batchSize */, 49 /* maxSize */, null /* docSuffix */);
// Test with maxSize = 2 batches
exceedMaxSize(100 /* nDocs */, 5 /* batchSize */, 49 /* maxSize */, null /* docSuffix */);
// Test with maxSize that does not fit into full batches, so we will check over maxSize at the next
// batch boundary
exceedMaxSize(100 /* nDocs */, 7 /* batchSize */, 49 /* maxSize */, null /* docSuffix */);
// Test with maxSize > size for nDocs
exceedMaxSize(100 /* nDocs */, 3 /* batchSize */, 528 /* maxSize */, null /* docSuffix */);

// Test with integer index entries (1, 2, 3, etc.), single character string entries ("1",
// "2", "3", etc.), and long string entries ("1aaaaaaaaaa")
[null, "", "aaaaaaaaaa"].forEach((docSuffix) => {
    // Test with maxCount = 1 batch
    exceedMaxCount(100 /* nDocs */, 10 /* batchSize */, 10 /* maxCount */, docSuffix);
    // Test with maxCount = 2 batches
    exceedMaxCount(100 /* nDocs */, 20 /* batchSize */, 10 /* maxCount */, docSuffix);
    // Test with maxCount that is not a multiple of batchSize, so we will check over maxCount at the
    // next batch boundary
    exceedMaxCount(100 /* nDocs */, 8 /* batchSize */, 10 /* maxCount */, docSuffix);
    // Test with maxCount > nDocs
    exceedMaxCount(100 /* nDocs */, 50 /* batchSize */, 200 /* maxCount */, docSuffix);
});

// TODO SERVER-79846:
// * Test progress meter/stats are correct

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
})();
