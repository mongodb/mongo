/**
 * Tests that the dbCheck command's extra index keys check correctly finds extra or inconsistent
 * consecutive identical index keys.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    awaitDbCheckCompletion,
    checkHealthLog,
    checkNumSnapshots,
    clearHealthLog,
    logQueries,
    resetAndInsertIdentical,
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
const defaultNumMaxIdenticalKeys = 1000;
const writeConcern = {
    w: 'majority'
};

const debugBuild = primaryDB.adminCommand('buildInfo').debug;

function setSnapshotSize(snapshotSize) {
    assert.commandWorked(primaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxTotalIndexKeysPerSnapshot": snapshotSize}));
    assert.commandWorked(secondaryDB.adminCommand(
        {"setParameter": 1, "dbCheckMaxTotalIndexKeysPerSnapshot": snapshotSize}));
}
function resetSnapshotSize() {
    setSnapshotSize(defaultSnapshotSize);
}

function setNumMaxIdenticalKeys(numMaxIdenticalKeys) {
    assert.commandWorked(primaryDB.adminCommand({
        "setParameter": 1,
        "dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot": numMaxIdenticalKeys
    }));
    // TODO SERVER-86858: Investigate removing the use of this parameter on secondaries.
    assert.commandWorked(secondaryDB.adminCommand({
        "setParameter": 1,
        "dbCheckMaxConsecutiveIdenticalIndexKeysPerSnapshot": numMaxIdenticalKeys
    }));
}

function resetNumMaxIdenticalKeys() {
    setNumMaxIdenticalKeys(defaultNumMaxIdenticalKeys);
}

function onlyIdenticalKeys(nDocs, batchSize, snapshotSize, numMaxIdenticalKeys, failpoint = null) {
    clearRawMongoProgramOutput();
    jsTestLog(`Testing behavior with a collection of only identical index keys with ${nDocs} 
              docs, batchSize: ${batchSize}, + snapshotSize: ${snapshotSize}, 
              numMaxIdenticalIndexKeys: ${numMaxIdenticalKeys}, failpoint: ${failpoint}`);

    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(snapshotSize);
    setNumMaxIdenticalKeys(numMaxIdenticalKeys);
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);

    let primaryFailpoint;
    let secondaryFailpoint;
    const primaryColl = primaryDB.getCollection(collName);
    if (failpoint != null) {
        primaryFailpoint = configureFailPoint(primaryDB, failpoint, {indexName: "a_1"});
    }

    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog(
            "Deleting docs and skipping unindexing document on primary to check recordNotFound and inconsistent batch");
        assert.commandWorked(primaryColl.deleteMany({}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({}).count(), 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        secondaryFailpoint = configureFailPoint(secondaryDB, failpoint, {indexName: "a_1"});
        jsTestLog(
            "Updating docs to remove index key field and skipping updating document on primary and secondary to check recordDoesNotMatch");
        assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({a: {$exists: true}}).count(), 0);
    }

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    const nDocsChecked = Math.min(numMaxIdenticalKeys, nDocs);
    if (failpoint == null) {
        jsTestLog("Checking no errors were found on primary or secondary");
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    } else if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

        jsTestLog(
            "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        jsTestLog("Checking primary for record does not match error");
        checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

        jsTestLog(
            "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    jsTestLog("Checking for correct number of batches on primary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": nDocsChecked,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nDocsChecked,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for correct number of batches on secondary");
    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        query = {
            ...logQueries.inconsistentBatchQuery,
            "data.count": 0,
            "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 0,
        };
        checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
        checkHealthLog(
            secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1 /* expectedNumBatches */);
    } else {
        checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
    }

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    if (primaryFailpoint != null) {
        primaryFailpoint.off();
    }
    if (secondaryFailpoint != null) {
        secondaryFailpoint.off();
    }
}

// This sets up a collection with `nIdenticalDocs` of distinct key docs, then `nIdenticalDocs`
// of identical key docs, then `nIdenticalDocs` of distinct key docs.
// Ex: if `nIdenticalDocs` is 10, we will have {a: -10},...,{a: -1}, 10 docs
// with {a:0}, {a:1},...{a:10}.
function setUpIdenticalKeysInMiddleOfColl(nIdenticalDocs,
                                          batchSize,
                                          snapshotSize,
                                          numMaxIdenticalKeys,
                                          failpoint,
                                          skipErrorChecks = false) {
    clearRawMongoProgramOutput();

    resetAndInsertIdentical(replSet, primaryDB, collName, nIdenticalDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(snapshotSize);
    setNumMaxIdenticalKeys(numMaxIdenticalKeys);

    // Insert different index keys before and after the identical ones.
    for (let i = 1; i <= nIdenticalDocs; i++) {
        assert.commandWorked(primaryDB[collName].insertOne({a: i}));
        assert.commandWorked(primaryDB[collName].insertOne({a: -1 * i}));
    }
    replSet.awaitReplication();
    assert.eq(primaryDB.getCollection(collName).find({}).count(), nIdenticalDocs * 3);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nIdenticalDocs * 3);

    let primaryFailpoint;
    let secondaryFailpoint;
    const primaryColl = primaryDB.getCollection(collName);
    if (failpoint != null) {
        primaryFailpoint = configureFailPoint(primaryDB, failpoint, {indexName: "a_1"});
    }

    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog(
            "Deleting docs and skipping unindexing document on primary to check recordNotFound and inconsistent batch");
        assert.commandWorked(primaryColl.deleteMany({}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({}).count(), 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        secondaryFailpoint = configureFailPoint(secondaryDB, failpoint, {indexName: "a_1"});
        jsTestLog(
            "Updating docs to remove index key field and skipping updating document on primary and secondary to check recordDoesNotMatch");
        assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({a: {$exists: true}}).count(), 0);
    }

    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: batchSize,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    if (!skipErrorChecks) {
        const nDiffDocs = nIdenticalDocs * 2;
        const nIdenticalDocsChecked = Math.min(numMaxIdenticalKeys, nIdenticalDocs);
        const nDocsChecked = nIdenticalDocsChecked + nDiffDocs;

        if (failpoint == null) {
            jsTestLog("Checking no errors were found on primary or secondary");
            checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
            checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        } else if (failpoint == "skipUnindexingDocumentWhenDeleted") {
            jsTestLog("Checking primary for record not found error");
            checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocsChecked);
            // No other errors on primary.
            checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

            jsTestLog(
                "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
            checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);
        } else if (failpoint == "skipUpdatingIndexDocument") {
            jsTestLog("Checking primary for record does not match error");
            checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nDocsChecked);
            // No other errors on primary.
            checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

            jsTestLog(
                "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
            checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        }
    }

    return primaryFailpoint, secondaryFailpoint;
}

function simpleIdenticalKeysInMiddleOfColl(
    nIdenticalDocs, batchSize, snapshotSize, numMaxIdenticalKeys, failpoint) {
    jsTestLog(`Testing simple identical key behavior in middle of collection with ${nIdenticalDocs} 
              docs with identical index keys, batchSize: ${batchSize}, + snapshotSize: ${snapshotSize}, 
              numMaxIdenticalIndexKeys: ${numMaxIdenticalKeys}, failpoint: ${failpoint}`);

    // This sets up a collection with `nIdenticalDocs` of distinct key docs, then `nIdenticalDocs`
    // of identical key docs, then `nIdenticalDocs` of distinct key docs.
    // Ex: if `nIdenticalDocs` is 10, we will have {a: -10},...,{a: -1}, 10 docs
    // with {a:0}, {a:1},...{a:10}.
    let primaryFailpoint,
        secondaryFailpoint = setUpIdenticalKeysInMiddleOfColl(
            nIdenticalDocs, batchSize, snapshotSize, numMaxIdenticalKeys, failpoint);

    const nDiffDocs = nIdenticalDocs * 2;
    const nIdenticalDocsChecked = Math.min(numMaxIdenticalKeys, nIdenticalDocs);
    // Calculate number of batches. We assume nDiffDocs will be exactly divisible
    // by batchSize, so that all the identical keys will go into a single batch (the + 1).
    const expectedNumBatchesForDistinctDocs = Math.ceil(nDiffDocs / batchSize);
    const expectedNumBatches = expectedNumBatchesForDistinctDocs + 1;

    jsTestLog("Checking for correct number of batches on primary");
    const identicalKeysQuery = {
        ...logQueries.infoBatchQuery,
        "data.count": nIdenticalDocsChecked,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nIdenticalDocsChecked,
    };
    checkHealthLog(primaryHealthLog, identicalKeysQuery, 1);
    // + 1 for identical keys batch.
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);

    if (debugBuild) {
        // Calculate number of snapshots. We assume nDiffDocs will be exactly divisible
        // by snapshotSize & batchSize, so that all the identical keys will go into a single
        // snapshot (the + 1).
        let expectedNumSnapshotsForDistinctDocs = expectedNumBatchesForDistinctDocs;
        if (snapshotSize < batchSize) {
            const snapshotsPerBatch = Math.ceil(batchSize / snapshotSize);
            const lastBatchSize =
                nIdenticalDocs % batchSize == 0 ? batchSize : nIdenticalDocs % batchSize;
            const lastBatchSnapshots = Math.ceil(lastBatchSize / snapshotSize);

            expectedNumSnapshotsForDistinctDocs =
                ((expectedNumBatchesForDistinctDocs - 1) * snapshotsPerBatch) + lastBatchSnapshots;
        }
        const expectedNumSnapshots = expectedNumSnapshotsForDistinctDocs + 1;
        checkNumSnapshots(debugBuild, expectedNumSnapshots);
    }

    jsTestLog("Checking for correct number of batches on secondary");
    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        checkHealthLog(secondaryHealthLog, logQueries.inconsistentBatchQuery, expectedNumBatches);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, expectedNumBatches);
    } else {
        checkHealthLog(secondaryHealthLog, identicalKeysQuery, 1);
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    resetSnapshotSize();
    resetNumMaxIdenticalKeys();

    if (primaryFailpoint != null) {
        primaryFailpoint.off();
    }
    if (secondaryFailpoint != null) {
        secondaryFailpoint.off();
    }
}

function allKeysInOneBatch(failpoint) {
    const nIdenticalDocs = 10;
    const batchSize = 30;
    const snapshotSize = 30;
    const numMaxIdenticalKeys = 6;
    jsTestLog(`Testing all keys in one batch with ${nIdenticalDocs} 
              docs with identical index keys, batchSize: ${batchSize}, + snapshotSize: ${snapshotSize}, 
              numMaxIdenticalIndexKeys: ${numMaxIdenticalKeys}, failpoint: ${failpoint}`);
    // This sets up a collection with `nIdenticalDocs` of distinct key docs, then `nIdenticalDocs`
    // of identical key docs, then `nIdenticalDocs` of distinct key docs.
    // Ex: if `nIdenticalDocs` is 10, we will have {a: -10},...,{a: -1}, 10 docs
    // with {a:0}, {a:1},...{a:10}.
    let primaryFailpoint,
        secondaryFailpoint = setUpIdenticalKeysInMiddleOfColl(nIdenticalDocs,
                                                              batchSize,
                                                              snapshotSize,
                                                              numMaxIdenticalKeys,
                                                              failpoint,
                                                              true /* skipErrorChecks */);

    const nTotalDocs = nIdenticalDocs * 3;

    if (failpoint == null) {
        jsTestLog("Checking no errors were found on primary or secondary");
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    } else if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nTotalDocs);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nTotalDocs);

        jsTestLog(
            "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        jsTestLog("Checking primary for record does not match error");
        checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nTotalDocs);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nTotalDocs);

        jsTestLog(
            "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    const expectedNumBatches = 1;

    jsTestLog("Checking for correct number of batches on primary");
    const identicalKeysQuery = {
        ...logQueries.infoBatchQuery,
        "data.count": nTotalDocs,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 1,
    };
    checkHealthLog(primaryHealthLog, identicalKeysQuery, 1);
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for correct number of batches on secondary");
    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        checkHealthLog(secondaryHealthLog, logQueries.inconsistentBatchQuery, expectedNumBatches);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, expectedNumBatches);
    } else {
        checkHealthLog(secondaryHealthLog, identicalKeysQuery, 1);
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);
    }

    resetSnapshotSize();
    resetNumMaxIdenticalKeys();

    if (primaryFailpoint != null) {
        primaryFailpoint.off();
    }
    if (secondaryFailpoint != null) {
        secondaryFailpoint.off();
    }
}

function identicalKeysAtEndOfBatch(failpoint) {
    const nIdenticalDocs = 5;
    const batchSize = 6;
    const snapshotSize = 4;
    const numMaxIdenticalKeys = 4;
    jsTestLog(`Testing identical key behavior at end of batch with ${nIdenticalDocs} 
              docs with identical index keys, batchSize: ${batchSize}, + snapshotSize: ${snapshotSize}, 
              numMaxIdenticalIndexKeys: ${numMaxIdenticalKeys}, failpoint: ${failpoint}`);

    // This sets up a collection with `nIdenticalDocs` of distinct key docs, then `nIdenticalDocs`
    // of identical key docs, then `nIdenticalDocs` of distinct key docs.
    // Ex: if `nIdenticalDocs` is 10, we will have {a: -10},...,{a: -1}, 10 docs
    // with {a:0}, {a:1},...{a:10}.
    let primaryFailpoint,
        secondaryFailpoint = setUpIdenticalKeysInMiddleOfColl(
            nIdenticalDocs, batchSize, snapshotSize, numMaxIdenticalKeys, failpoint);

    const nIdenticalDocsChecked = Math.min(numMaxIdenticalKeys, nIdenticalDocs);

    // First batch: 5 diff docs + 4 identical key docs.
    // Second batch: 5 remaining diff docs.
    const expectedNumBatches = 2;

    jsTestLog("Checking for correct number of batches on primary");
    const identicalKeysQuery = {
        ...logQueries.infoBatchQuery,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nIdenticalDocsChecked,
    };
    checkHealthLog(primaryHealthLog, identicalKeysQuery, 1);
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);

    // 1st snapshot: 4 diff docs, 2nd: 1 + 4 identical key docs, 3rd: 4 diff docs, 4th: 1
    // remaining diff doc.
    const expectedNumSnapshots = 4;
    checkNumSnapshots(debugBuild, expectedNumSnapshots);

    jsTestLog("Checking for correct number of batches on secondary");

    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        checkHealthLog(secondaryHealthLog, logQueries.inconsistentBatchQuery, expectedNumBatches);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, expectedNumBatches);
    } else {
        checkHealthLog(secondaryHealthLog, identicalKeysQuery, 1);
        checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, expectedNumBatches);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    resetSnapshotSize();
    resetNumMaxIdenticalKeys();

    if (primaryFailpoint != null) {
        primaryFailpoint.off();
    }
    if (secondaryFailpoint != null) {
        secondaryFailpoint.off();
    }
}

function nConsecutiveIdenticalIndexKeysSeenAtEndIsReset(failpoint) {
    clearRawMongoProgramOutput();
    primaryDB.getCollection(collName).drop();
    clearHealthLog(replSet);
    jsTestLog(
        "Testing that nConsecutiveIdenticalIndexKeysSeenAtEnd is reset when encountering a new distinct key");

    const nDocs = 3;
    setSnapshotSize(defaultSnapshotSize);

    // Coll is 0, 0, 0, 1, 1, 1
    // DbCheck should not hit the maxIdenticalKeys limit as nConsecutiveIdenticalIndexKeysSeenAtEnd
    // should be reset when we encounter a distinct key.
    setNumMaxIdenticalKeys(4);
    assert.commandWorked(primaryDB.getCollection(collName).insertMany(
        [...Array(nDocs).keys()].map(x => ({_id: x, a: 0})), {ordered: false}));

    assert.commandWorked(primaryDB.getCollection(collName).insertMany(
        [...Array(nDocs).keys()].map(x => ({_id: x + nDocs, a: 1})), {ordered: false}));

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs * 2);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs * 2);

    let primaryFailpoint;
    let secondaryFailpoint;
    const primaryColl = primaryDB.getCollection(collName);
    if (failpoint != null) {
        primaryFailpoint = configureFailPoint(primaryDB, failpoint, {indexName: "a_1"});
    }

    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog(
            "Deleting docs and skipping unindexing document on primary to check recordNotFound and inconsistent batch");
        assert.commandWorked(primaryColl.deleteMany({}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({}).count(), 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        secondaryFailpoint = configureFailPoint(secondaryDB, failpoint, {indexName: "a_1"});
        jsTestLog(
            "Updating docs to remove index key field and skipping updating document on primary and secondary to check recordDoesNotMatch");
        assert.commandWorked(primaryColl.updateMany({}, {$unset: {"a": ""}}));

        replSet.awaitReplication();
        assert.eq(primaryColl.find({a: {$exists: true}}).count(), 0);
        assert.eq(secondaryDB.getCollection(collName).find({a: {$exists: true}}).count(), 0);
    }

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    const nDocsChecked = nDocs * 2;
    if (failpoint == null) {
        jsTestLog("Checking no errors were found on primary or secondary");
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    } else if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        jsTestLog("Checking primary for record not found error");
        checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

        jsTestLog(
            "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);
    } else if (failpoint == "skipUpdatingIndexDocument") {
        jsTestLog("Checking primary for record does not match error");
        checkHealthLog(primaryHealthLog, logQueries.recordDoesNotMatchQuery, nDocsChecked);
        // No other errors on primary.
        checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocsChecked);

        jsTestLog(
            "Checking secondary for record does not match error, should have 0 since secondary skips reverse lookup");
        checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    }

    jsTestLog("Checking for correct number of batches on primary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": nDocsChecked,
        // Should reset when seeing a new distinct key
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nDocs,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for correct number of batches on secondary");
    if (failpoint == "skipUnindexingDocumentWhenDeleted") {
        query = {
            ...logQueries.inconsistentBatchQuery,
            "data.count": 0,
            "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 0,
        };
        checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
        checkHealthLog(
            secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1 /* expectedNumBatches */);
    } else {
        checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
    }

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    if (primaryFailpoint != null) {
        primaryFailpoint.off();
    }
    if (secondaryFailpoint != null) {
        secondaryFailpoint.off();
    }
}

function hashingExtraIdenticalIndexKeysOnPrimary() {
    clearRawMongoProgramOutput();
    jsTestLog("Testing that hashing will catch extra identical index keys on primary");
    const nDocs = 10;

    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(defaultSnapshotSize);
    setNumMaxIdenticalKeys(defaultNumMaxIdenticalKeys);
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);

    const primaryFailpoint =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    const primaryColl = primaryDB.getCollection(collName);

    jsTestLog("Deleting n-1 docs");
    for (let i = 1; i < nDocs; i++) {
        assert.commandWorked(primaryColl.deleteOne({_id: i}));
    }

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 1);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), 1);

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    jsTestLog("Checking primary for record not found error");
    checkHealthLog(primaryHealthLog, logQueries.recordNotFoundQuery, nDocs - 1);
    // No other errors on primary.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, nDocs - 1);

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": nDocs,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nDocs,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for correct number of batches on secondary");
    query = {
        ...logQueries.inconsistentBatchQuery,
        "data.count": 1,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 1,
    };
    checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
    checkHealthLog(
        secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1 /*expectedNumBatches*/);

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    primaryFailpoint.off();
}

function hashingExtraIdenticalIndexKeysOnSecondary() {
    clearRawMongoProgramOutput();
    const nDocs = 20;
    jsTestLog(
        "Testing that hashing will catch extra identical index keys on secondary for any extra identical index keys up to numMaxIdenticalIndexKeys");

    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(defaultSnapshotSize);
    const numMaxIdenticalKeys = 10;
    setNumMaxIdenticalKeys(numMaxIdenticalKeys);
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);

    const secondaryFailpoint =
        configureFailPoint(secondaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting docs");
    const primaryColl = primaryDB.getCollection(collName);
    // Delete docs. There will still be 20 identical index keys on secondary.
    for (let i = 9; i < 20; i++) {
        assert.commandWorked(primaryColl.deleteOne({_id: i}));
    }

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 9);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), 9);

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    jsTestLog("Checking primary for no errors");
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": numMaxIdenticalKeys - 1,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": numMaxIdenticalKeys - 1,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for secondary inconsistency");
    query = {
        ...logQueries.inconsistentBatchQuery,
        "data.count": numMaxIdenticalKeys,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": numMaxIdenticalKeys,
    };
    checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
    checkHealthLog(
        secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1 /*expectedNumBatches*/);

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    secondaryFailpoint.off();
}

function extraIdenticalIndexKeysOnSecondaryBeyondMax() {
    clearRawMongoProgramOutput();
    const nDocs = 20;
    jsTestLog(
        "Testing that hashing will not catch extra identical index keys on secondary for the extra identical index keys beyond numMaxIdenticalIndexKeys");

    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(defaultSnapshotSize);
    const numMaxIdenticalKeys = 10;
    setNumMaxIdenticalKeys(numMaxIdenticalKeys);
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs);

    const secondaryFailpoint =
        configureFailPoint(secondaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting 1 doc");
    const primaryColl = primaryDB.getCollection(collName);
    // Delete docs after numMaxIdenticalKeys.
    for (let i = 10; i < 20; i++) {
        assert.commandWorked(primaryColl.deleteOne({_id: i}));
    }

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 10);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), 10);

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    jsTestLog("Checking primary for no errors");
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog("Checking for correct number of batches on primary and secondary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": numMaxIdenticalKeys,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": numMaxIdenticalKeys,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);
    checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    secondaryFailpoint.off();
}

function hashingExtraIdenticalIndexKeysOnSecondaryMiddleOfBatch() {
    clearRawMongoProgramOutput();
    const nDocs = 10;
    jsTestLog(
        "Testing that hashing will catch extra identical index keys on secondary in the middle of a batch");

    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    setSnapshotSize(defaultSnapshotSize);
    setNumMaxIdenticalKeys(defaultNumMaxIdenticalKeys);
    // Insert different index keys before and after the identical ones.
    for (let i = 1; i <= nDocs; i++) {
        assert.commandWorked(primaryDB[collName].insertOne({a: i}));
        assert.commandWorked(primaryDB[collName].insertOne({a: -1 * i}));
    }
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs * 3);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs * 3);

    const secondaryFailpoint =
        configureFailPoint(secondaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting 1 doc");
    const primaryColl = primaryDB.getCollection(collName);
    assert.commandWorked(primaryColl.deleteOne({_id: 0}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), nDocs * 3 - 1);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs * 3 - 1);

    // Running DbCheck.
    const dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern,
        skipLookupForExtraKeys: false,
    };

    runDbCheck(replSet, primaryDB, collName, dbCheckParameters, true /* awaitCompletion */);

    // Checking for correct batches and errors.
    jsTestLog("Checking primary for no errors");
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    jsTestLog(
        "Checking secondary for record not found error, should have 0 since secondary skips reverse lookup");
    checkHealthLog(secondaryHealthLog, logQueries.recordNotFoundQuery, 0);

    jsTestLog("Checking for correct number of batches on primary");
    let query = {
        ...logQueries.infoBatchQuery,
        "data.count": nDocs * 3 - 1,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 1,
    };
    checkHealthLog(primaryHealthLog, query, 1 /* expectedNumBatches */);

    checkNumSnapshots(debugBuild, 1);

    jsTestLog("Checking for secondary inconsistency");
    query = {
        ...logQueries.inconsistentBatchQuery,
        "data.count": nDocs * 3,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": 1,
    };
    checkHealthLog(secondaryHealthLog, query, 1 /* expectedNumBatches */);
    checkHealthLog(
        secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1 /*expectedNumBatches*/);

    // Cleanup test.
    resetSnapshotSize();
    resetNumMaxIdenticalKeys();
    secondaryFailpoint.off();
}

function identicalKeysChangedBeforeHashing() {
    jsTestLog(
        "Testing that if identical keys change in between reverse lookup and hashing we won't error.");
    setSnapshotSize(defaultSnapshotSize);
    setNumMaxIdenticalKeys(defaultNumMaxIdenticalKeys);

    const nDocs = 10;
    resetAndInsertIdentical(replSet, primaryDB, collName, nDocs);

    const primaryColl = primaryDB.getCollection(collName);
    // Delete one doc.
    primaryColl.deleteOne({_id: 3});

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();

    assert.eq(primaryDB.getCollection(collName).find({}).count(), nDocs - 1);
    assert.eq(secondaryDB.getCollection(collName).find({}).count(), nDocs - 1);

    const hangBeforeExtraIndexKeysHashing =
        configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysHashing");

    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: defaultMaxDocsPerBatch,
        batchWriteConcern: writeConcern
    };
    runDbCheck(replSet, primaryDB, collName, dbCheckParameters);

    // Reverse lookup snapshot will find 9 {a:0} index keys.
    hangBeforeExtraIndexKeysHashing.wait();

    // Actual batch will have 8.
    primaryColl.deleteOne({_id: 0});
    primaryColl.insertOne({_id: 3, a: 0});
    primaryColl.deleteOne({_id: 9});

    hangBeforeExtraIndexKeysHashing.off();

    awaitDbCheckCompletion(replSet, primaryDB);

    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);

    const query = {
        ...logQueries.infoBatchQuery,
        "data.count": nDocs - 2,
        "data.nConsecutiveIdenticalIndexKeysSeenAtEnd": nDocs - 2,
    };
    jsTestLog("Checking for correct number of batches on primary");
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 1);
    jsTestLog("Checking for correct number of batches on secondary");
    checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 1);
}

[null /* no errors */,
 "skipUnindexingDocumentWhenDeleted", /* recordNotFound and inconsistent batch errors are caught
                                       */
 "skipUpdatingIndexDocument" /* recordDoesNotMatch errors are caught.*/]
    .forEach((failpoint) => {
        // Check maxIdenticalKeys > numDocs > batch/snapshot size - all keys should be checked in
        // one batch/snapshot.
        onlyIdenticalKeys(11 /*numDocs*/,
                          10 /*batchSize*/,
                          5 /*snapshotSize*/,
                          defaultNumMaxIdenticalKeys,
                          failpoint);

        // numDocs > maxIdenticalKeys > batch/snapshotsize - should only check up to
        // numMaxIdenticalKeys.
        onlyIdenticalKeys(20 /*numDocs*/,
                          5 /*batchSize*/,
                          6 /*snapshotSize*/,
                          7 /*numMaxIdenticalKeys*/,
                          failpoint);

        // Simple tests with distinct keys before and after identical keys.
        // Tests nIdenticalDocs < numMaxIdenticalKeys, batchSize < snapshotSize.
        simpleIdenticalKeysInMiddleOfColl(11 /*nIdenticalDocs*/,
                                          1 /*batchSize*/,
                                          2 /*snapshotSize*/,
                                          defaultNumMaxIdenticalKeys,
                                          failpoint);
        // Tests nIdenticalDocs > numMaxIdenticalKeys, batchSize == snapshotSize.
        simpleIdenticalKeysInMiddleOfColl(20 /*nIdenticalDocs*/,
                                          5 /*batchSize*/,
                                          5 /*snapshotSize*/,
                                          6 /*numMaxIdenticalKeys*/,
                                          failpoint);
        // Tests nIdenticalDocs > numMaxIdenticalKeys, batchSize > snapshotSize.
        simpleIdenticalKeysInMiddleOfColl(20 /*nIdenticalDocs*/,
                                          5 /*batchSize*/,
                                          2 /*snapshotSize*/,
                                          6 /*numMaxIdenticalKeys*/,
                                          failpoint);

        // Identical keys at the end of the batch/snapshot size result in batch/snapshot limit
        // getting ignored.
        identicalKeysAtEndOfBatch(failpoint);

        // Batch/snapshot size >= nDocs, numMaxIdenticalKeys is ignored.
        allKeysInOneBatch(failpoint);

        // Testing that nConsecutiveIdenticalIndexKeysSeenAtEnd is reset when encountering a new
        // distinct key.
        nConsecutiveIdenticalIndexKeysSeenAtEndIsReset(failpoint);
    });

hashingExtraIdenticalIndexKeysOnPrimary();
hashingExtraIdenticalIndexKeysOnSecondary();
hashingExtraIdenticalIndexKeysOnSecondaryMiddleOfBatch();
identicalKeysChangedBeforeHashing();
extraIdenticalIndexKeysOnSecondaryBeyondMax();

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
})();
