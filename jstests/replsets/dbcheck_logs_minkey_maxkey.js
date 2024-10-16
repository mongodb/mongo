/**
 * Test that in a given dbcheck run, the first 'start' field should be either minKey or the
 * user-specified 'start' field, and the last 'end' field should be either maxKey or the
 * user-specified 'end' field.
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */
import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {ReplSetTest} from "jstests/libs/replsettest.js";
import {
    assertCompleteCoverage,
    checkHealthLog,
    clearHealthLog,
    injectInconsistencyOnSecondary,
    logEveryBatch,
    logQueries,
    runDbCheck,
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = jsTestName();
const collName = jsTestName();

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
const primaryDB = primary.getDB(dbName);
let secondary = replSet.getSecondary();
const primaryHealthLog = primary.getDB("local").system.healthlog;
let secondaryHealthLog = secondary.getDB("local").system.healthlog;

function testDataConsistencyAndMissingKeysCheck() {
    clearHealthLog(replSet);
    primaryDB.getCollection(collName).drop();

    // Setting up collection inconsistency.
    jsTestLog("Setting up collection inconsistency.");
    assert.commandWorked(primaryDB.createCollection(collName));
    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));

    replSet.awaitReplication();
    const docArr = [{_id: 0, a: 0}, {_id: 4, a: 4}, {_id: 5, a: 5}];
    injectInconsistencyOnSecondary(
        replSet, dbName, {insert: collName, documents: docArr}, true /*withMissingIndexKeys*/);
    replSet.awaitReplication();
    replSet.awaitSecondaryNodes();
    logEveryBatch(replSet);
    secondary = replSet.getSecondary();
    secondaryHealthLog = secondary.getDB("local").system.healthlog;

    jsTestLog(
        "Testing that dbCheck logs minKey and maxKey in data consistency check in one batch.");
    runDbCheck(replSet,
               primary.getDB(dbName),
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true /*awaitCompletion*/);

    // Verify that both the primary and secondary log minKey and maxKey.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(
        primaryHealthLog,
        {...logQueries.infoBatchQuery, "data.batchStart._id": MinKey, "data.batchEnd._id": MaxKey},
        1);
    assertCompleteCoverage(primaryHealthLog, 3 /*nDocs*/, "_id" /*indexName*/, "" /*docSuffix*/);

    checkHealthLog(secondaryHealthLog,
                   {
                       ...logQueries.inconsistentBatchQuery,
                       "data.batchStart._id": MinKey,
                       "data.batchEnd._id": MaxKey
                   },
                   1);
    checkHealthLog(secondaryHealthLog, logQueries.missingIndexKeysQuery, 3);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 4);
    assertCompleteCoverage(secondaryHealthLog,
                           3 /*nDocs*/,
                           "_id" /*indexName*/,
                           "" /*docSuffix*/,
                           null /*start*/,
                           null /*end*/);

    clearHealthLog(replSet);
    jsTestLog(
        "Testing that dbCheck logs minKey and maxKey in data consistency check in multiple batches.");
    // Insert docs to have more than one batch.
    for (let i = 1; i <= 3; i++) {
        primaryDB.getCollection(collName).insertOne({_id: i, a: i});
    }
    runDbCheck(replSet,
               primary.getDB(dbName),
               collName,
               {maxDocsPerBatch: 1, validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true /*awaitCompletion*/);

    // Verify that both the primary and secondary log minKey and maxKey.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 3);
    checkHealthLog(
        primaryHealthLog, {...logQueries.infoBatchQuery, "data.batchStart._id": MinKey}, 1);
    checkHealthLog(
        primaryHealthLog, {...logQueries.infoBatchQuery, "data.batchEnd._id": MaxKey}, 1);
    // TODO SERVER-92609: Standardize batch bounds between extra index keys check and collection
    // check so we can use assertCompleteCoverage assertCompleteCoverage(
    //     primaryHealthLog, 2 /*nDocs*/, "_id" /*indexName*/, "" /*docSuffix*/);

    checkHealthLog(secondaryHealthLog,
                   {...logQueries.inconsistentBatchQuery, "data.batchStart._id": MinKey},
                   1);
    checkHealthLog(
        secondaryHealthLog, {...logQueries.inconsistentBatchQuery, "data.batchEnd._id": MaxKey}, 1);

    // There should be 1 inconsistent batch (docs 0 and 1), 1 consistent batch (doc 2), 1
    // inconsistent batch (docs 3, 4, 5).
    checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 1);
    checkHealthLog(secondaryHealthLog, logQueries.inconsistentBatchQuery, 2);
    checkHealthLog(secondaryHealthLog, logQueries.missingIndexKeysQuery, 3);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 5);
    // TODO SERVER-92609: Standardize batch bounds between extra index keys check and collection
    // check so we can use assertCompleteCoverage assertCompleteCoverage(
    //     secondaryHealthLog, 12 /*nDocs*/, "_id" /*indexName*/,"" /*docSuffix*/, null /*start*/,
    //     null /*end*/);
}

function testExtraIndexKeysCheck() {
    clearHealthLog(replSet);
    primaryDB.getCollection(collName).drop();

    // Setting up collection inconsistency.
    jsTestLog("Setting up collection inconsistency.");
    const primaryColl = primaryDB.getCollection(collName);
    secondary = replSet.getSecondary();
    const secondaryDB = secondary.getDB(dbName);
    const secondaryColl = secondaryDB.getCollection(collName);

    assert.commandWorked(
        primaryColl.insertMany([{_id: 0, a: 0}, {_id: 4, a: 4}, {_id: 5, a: 5}], {ordered: false}));

    assert.commandWorked(primaryDB.runCommand({
        createIndexes: collName,
        indexes: [{key: {a: 1}, name: 'a_1'}],
    }));
    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 3);
    assert.eq(secondaryColl.find({}).count(), 3);

    // Set up inconsistency.
    const skipUnindexingDocumentWhenDeleted =
        configureFailPoint(secondaryDB, "skipUnindexingDocumentWhenDeleted", {indexName: "a_1"});
    jsTestLog("Deleting docs");
    assert.commandWorked(primaryColl.deleteMany({}));

    replSet.awaitReplication();
    assert.eq(primaryColl.find({}).count(), 0);
    assert.eq(secondaryColl.find({}).count(), 0);

    logEveryBatch(replSet);
    let dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
    };

    jsTestLog(
        "Testing that dbCheck logs minKey and maxKey in extra index keys check in one batch.");
    runDbCheck(
        replSet, primary.getDB(dbName), collName, dbCheckParameters, true /*awaitCompletion*/);

    // Verify that both the primary and secondary log minKey and maxKey.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(
        primaryHealthLog,
        {...logQueries.infoBatchQuery, "data.batchStart.a": MinKey, "data.batchEnd.a": MaxKey},
        1);
    assertCompleteCoverage(primaryHealthLog, 3 /*nDocs*/, "a" /*indexName*/, "" /*docSuffix*/);

    checkHealthLog(secondaryHealthLog,
                   {
                       ...logQueries.inconsistentBatchQuery,
                       "data.batchStart.a": MinKey,
                       "data.batchEnd.a": MaxKey
                   },
                   1);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 1);
    assertCompleteCoverage(secondaryHealthLog,
                           3 /*nDocs*/,
                           "a" /*indexName*/,
                           "" /*docSuffix*/,
                           null /*start*/,
                           null /*end*/);

    clearHealthLog(replSet);
    jsTestLog(
        "Testing that dbCheck logs minKey and maxKey in extra index keys check in multiple batches.");
    // Insert docs to have more than one batch.
    for (let i = 1; i <= 3; i++) {
        primaryDB.getCollection(collName).insertOne({_id: i, a: i});
    }
    dbCheckParameters = {
        validateMode: "extraIndexKeysCheck",
        secondaryIndex: "a_1",
        maxDocsPerBatch: 1
    };
    runDbCheck(
        replSet, primary.getDB(dbName), collName, dbCheckParameters, true /*awaitCompletion*/);

    // Verify that both the primary and secondary log minKey and maxKey.
    checkHealthLog(primaryHealthLog, logQueries.allErrorsOrWarningsQuery, 0);
    checkHealthLog(primaryHealthLog, logQueries.infoBatchQuery, 3);
    checkHealthLog(
        primaryHealthLog, {...logQueries.infoBatchQuery, "data.batchStart.a": MinKey}, 1);
    checkHealthLog(primaryHealthLog, {...logQueries.infoBatchQuery, "data.batchEnd.a": MaxKey}, 1);
    assertCompleteCoverage(primaryHealthLog, 3 /*nDocs*/, "a" /*indexName*/, "" /*docSuffix*/);

    checkHealthLog(
        secondaryHealthLog, {...logQueries.inconsistentBatchQuery, "data.batchStart.a": MinKey}, 1);
    checkHealthLog(
        secondaryHealthLog, {...logQueries.inconsistentBatchQuery, "data.batchEnd.a": MaxKey}, 1);

    // There should be 1 inconsistent batch (docs 0 and 1), 1 consistent batch (doc 2), 1
    // inconsistent batch (docs 3, 4, 5).
    checkHealthLog(secondaryHealthLog, logQueries.infoBatchQuery, 1);
    checkHealthLog(secondaryHealthLog, logQueries.inconsistentBatchQuery, 2);
    checkHealthLog(secondaryHealthLog, logQueries.allErrorsOrWarningsQuery, 2);
    assertCompleteCoverage(secondaryHealthLog,
                           3 /*nDocs*/,
                           "a" /*indexName*/,
                           "" /*docSuffix*/,
                           null /*start*/,
                           null /*end*/);
    skipUnindexingDocumentWhenDeleted.off();
}

testDataConsistencyAndMissingKeysCheck();
testExtraIndexKeysCheck();

replSet.stopSet();
