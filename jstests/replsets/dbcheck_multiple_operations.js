/**
 * Tests dbcheck's behavior when multiple dbcheck commands are issued and ensures that only one of
 * them is running at a time.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkHealthLog,
    insertDocsWithMissingIndexKeys,
    logQueries,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

const dbName = "dbCheckMultipleOperations";
const collName1 = "dbCheckMultipleOperations-collection1";
const collName2 = "dbCheckMultipleOperations-collection2";
const collName3 = "dbCheckMultipleOperations-collection3";
const collName4 = "dbCheckMultipleOperations-collection4";
const collName5 = "dbCheckMultipleOperations-collection5";
const collName6 = "dbCheckMultipleOperations-collection6";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const secondary = replSet.getSecondary();
const primaryHealthlog = primary.getDB("local").system.healthlog;
const secondaryHealthlog = secondary.getDB("local").system.healthlog;
const primaryDB = primary.getDB(dbName);

const nDocs = 10;
const maxDocsPerBatch = 10;
const writeConcern = {
    w: 'majority'
};
const doc = {
    a: 1
};
const dbCheckParametersMissingKeysCheck = {
    validateMode: "dataConsistencyAndMissingIndexKeysCheck",
    maxDocsPerBatch: maxDocsPerBatch,
    batchWriteConcern: writeConcern
};
const dbCheckParametersExtraKeysCheck = {
    validateMode: "extraIndexKeysCheck",
    secondaryIndex: "a_1",
    maxDocsPerBatch: maxDocsPerBatch,
    batchWriteConcern: writeConcern
};

// This test injects inconsistencies between replica set members; do not fail because of expected
// dbHash differences.
TestData.skipCheckDBHashes = true;

function testMultipleDbCheckOnDiffCollections(docSuffix) {
    jsTestLog("Testing that only one dbcheck command will be running at a time.");

    const primaryColl1 = primaryDB.getCollection(collName1);
    const primaryColl2 = primaryDB.getCollection(collName2);
    resetAndInsert(replSet, primaryDB, collName1, nDocs, docSuffix);
    primaryDB[collName2].drop();
    insertDocsWithMissingIndexKeys(replSet, dbName, collName2, doc, nDocs);

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName1,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName2,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();
    assert.eq(primaryColl1.find({}).count(), nDocs);
    assert.eq(primaryColl2.find({}).count(), nDocs);

    // Set up inconsistency for coll1.
    const skipUnindexingDocumentWhenDeleted1 =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting docs");
    assert.commandWorked(primaryColl1.deleteMany({}));
    replSet.awaitReplication();
    assert.eq(primaryColl1.find({}).count(), 0);
    skipUnindexingDocumentWhenDeleted1.off();

    const firstDbCheckFailPoint = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysCheck");

    runDbCheck(replSet, primaryDB, collName1, dbCheckParametersExtraKeysCheck);
    firstDbCheckFailPoint.wait();
    runDbCheck(replSet, primaryDB, collName2, dbCheckParametersMissingKeysCheck);
    firstDbCheckFailPoint.off();

    // Wait for dbcheck to complete.
    checkHealthLog(primaryHealthlog, {"operation": "dbCheckStop"}, 2);

    jsTestLog("printing primary healthlog first test");
    jsTestLog(primaryHealthlog.find({}).toArray());

    // Make sure that the dbcheck operations ran in order.
    // The contents of the health log would be (in this order):
    // For coll1: 1 start, 10 extra keys error entries, 1 info batch, 1 stop
    // For coll2: 1 start, 5 missing keys error entries, 1 info batch, 5 missing keys, 1 info batch,
    // 1 stop
    // For coll2: Since docsSeen + keysSeen is used to see if we exceeded the maxDocsPerBatch limit
    // and keysSeen is incremented even if the index key is missing, there will be 2 info batch
    // entries for 10 documents with maxDocsPerBatch 10.
    // Check that the contents of the primary health log is correct.
    checkHealthLog(primaryHealthlog, logQueries.recordNotFoundQuery, 10);
    checkHealthLog(primaryHealthlog, logQueries.missingIndexKeysQuery, 10);
    checkHealthLog(primaryHealthlog, logQueries.allErrorsOrWarningsQuery, 20);
    checkHealthLog(primaryHealthlog, logQueries.infoBatchQuery, 3);

    // Check that the health log entries are in the correct order.
    const healthlogArray = primaryHealthlog.find({}).toArray();
    for (let i = 0; i <= 12; i++) {
        assert.eq(healthlogArray[i].data.dbCheckParameters.validateMode, "extraIndexKeysCheck");
    }
    assert.eq(healthlogArray[0].operation, "dbCheckStart");
    assert.eq(healthlogArray[0].namespace,
              "dbCheckMultipleOperations.dbCheckMultipleOperations-collection1");
    assert.eq(healthlogArray[12].operation, "dbCheckStop");
    assert.eq(healthlogArray[12].namespace,
              "dbCheckMultipleOperations.dbCheckMultipleOperations-collection1");

    for (let i = 13; i <= 26; i++) {
        assert.eq(healthlogArray[i].data.dbCheckParameters.validateMode,
                  "dataConsistencyAndMissingIndexKeysCheck");
    }
    assert.eq(healthlogArray[13].operation, "dbCheckStart");
    assert.eq(healthlogArray[13].namespace,
              "dbCheckMultipleOperations.dbCheckMultipleOperations-collection2");
    assert.eq(healthlogArray[26].operation, "dbCheckStop");
    assert.eq(healthlogArray[26].namespace,
              "dbCheckMultipleOperations.dbCheckMultipleOperations-collection2");
}

function testTooManyDbChecks(docSuffix) {
    jsTestLog(
        "Testing that running too many dbcheck operations will generate an error health log entry.");

    const collectionNames = [collName1, collName2, collName3, collName4, collName5, collName6];
    collectionNames.forEach((collName) => {
        primaryDB.getCollection(collName);
        resetAndInsert(replSet, primaryDB, collName, nDocs, docSuffix);
        assert.commandWorked(primaryDB.runCommand({
            createIndexes: collName,
            indexes: [{key: {a: 1}, name: 'a_1'}],
        }));
    });
    replSet.awaitReplication();

    // Set up inconsistency for coll1.
    const primaryColl1 = primaryDB.getCollection(collName1);
    const skipUnindexingDocumentWhenDeleted1 =
        configureFailPoint(primaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting docs");
    assert.commandWorked(primaryColl1.deleteMany({}));
    replSet.awaitReplication();
    assert.eq(primaryColl1.find({}).count(), 0);
    skipUnindexingDocumentWhenDeleted1.off();

    const firstDbCheckFailPoint = configureFailPoint(primaryDB, "hangBeforeExtraIndexKeysCheck");

    runDbCheck(replSet, primaryDB, collName1, dbCheckParametersExtraKeysCheck);
    firstDbCheckFailPoint.wait();
    runDbCheck(replSet, primaryDB, collName2, dbCheckParametersMissingKeysCheck);
    runDbCheck(replSet, primaryDB, collName3, dbCheckParametersMissingKeysCheck);
    runDbCheck(replSet, primaryDB, collName4, dbCheckParametersExtraKeysCheck);
    runDbCheck(replSet, primaryDB, collName5, dbCheckParametersMissingKeysCheck);
    runDbCheck(replSet, primaryDB, collName6, dbCheckParametersExtraKeysCheck);

    // Check that an error health log entry is generated for having too many dbchecks in queue.
    checkHealthLog(
        primaryHealthlog,
        {...logQueries.tooManyDbChecksInQueue, $expr: {$eq: [{$size: "$data.dbCheckQueue"}, 5]}},
        1);

    firstDbCheckFailPoint.off();

    // Only 5 dbcheck operations will run in total because the last one was issued when the queue is
    // too large, so it was not added to the queue.
    checkHealthLog(primaryHealthlog, {"operation": "dbCheckStop"}, 5);

    // Check that the primary generated an error health log entry for each missing document in
    // coll1.
    checkHealthLog(primaryHealthlog, logQueries.recordNotFoundQuery, 10);
    // Check that there are no other warnings/errors other than recordNotFound and
    // tooManyDbChecksInQueue.
    checkHealthLog(primaryHealthlog, logQueries.allErrorsOrWarningsQuery, 11);
}

testMultipleDbCheckOnDiffCollections("");
testTooManyDbChecks("aaaaaaaaaa");

replSet.stopSet(undefined /* signal */,
                false /* forRestart */,
                {skipCheckDBHashes: true, skipValidation: true});
