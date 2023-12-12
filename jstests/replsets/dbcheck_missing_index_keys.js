/**
 * Test missing index keys check in dbCheck. The missing index keys check logs errors in the
 * healthlog for every document that has a field that is indexed but the index key for it does not
 * exist in the index. The healthlog entry contains a list of all missing index keys.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {configureFailPoint} from "jstests/libs/fail_point_util.js";
import {
    checkHealthLog,
    clearHealthLog,
    forEachNonArbiterNode,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because data is inserted into primary and secondary separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "dbCheckMissingIndexKeysCheck";
const collName = "dbCheckMissingIndexKeysCheck-collection";

const replSet = new ReplSetTest({name: jsTestName(), nodes: 2});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const primaryDb = primary.getDB(dbName);
const secondary = replSet.getSecondary();
const secondaryDb = secondary.getDB(dbName);

const doc1 = {
    a: 1
};
const doc2 = {
    a: 1,
    b: 1
};

const errQuery = {
    operation: "dbCheckBatch",
    severity: "error",
};

const missingKeyErrQuery = {
    ...errQuery,
    msg: "Document has missing index keys",
};

// Return a dbCheck health log query for 'numMissing' missing index keys.
function getMissingIndexKeysQuery(numMissing) {
    return {
        operation: "dbCheckBatch",
        severity: "error",
        msg: "Document has missing index keys",
        "data.context.recordID": {$exists: true},
        $and: [
            {"data.context.missingIndexKeys": {$exists: true}},
            {$expr: {$eq: [{$size: "$data.context.missingIndexKeys"}, numMissing]}}
        ],
        "data.context.missingIndexKeys": {$elemMatch: {"indexSpec": {$exists: true}}}
    };
}

// Insert numDocs documents with missing index keys for testing.
function insertDocsWithMissingIndexKeys(
    collName, doc, numDocs = 1, doPrimary = true, doSecondary = true) {
    assert.commandWorked(primaryDb.createCollection(collName));

    // Create an index for every key in the document.
    let index = {};
    for (let key in doc) {
        index[key] = 1;
        assert.commandWorked(primaryDb[collName].createIndex(index));
        index = {};
    }
    replSet.awaitReplication();

    // dbCheck requires the _id index to iterate through documents in a batch.
    let skipIndexNewRecordsExceptIdPrimary;
    let skipIndexNewRecordsExceptIdSecondary;
    if (doPrimary) {
        skipIndexNewRecordsExceptIdPrimary =
            configureFailPoint(primaryDb, "skipIndexNewRecords", {skipIdIndex: false});
    }
    if (doSecondary) {
        skipIndexNewRecordsExceptIdSecondary =
            configureFailPoint(secondaryDb, "skipIndexNewRecords", {skipIdIndex: false});
    }
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(primaryDb[collName].insert(doc));
    }
    replSet.awaitReplication();
    if (doPrimary) {
        skipIndexNewRecordsExceptIdPrimary.off();
    }
    if (doSecondary) {
        skipIndexNewRecordsExceptIdSecondary.off();
    }

    // Verify that index has been replicated to all nodes, including _id index.
    forEachNonArbiterNode(replSet, function(node) {
        assert.eq(Object.keys(doc).length + 1, node.getDB(dbName)[collName].getIndexes().length);
    });
}

function checkMissingIndexKeys(doc, numDocs = 1, maxDocsPerBatch = 10000) {
    clearHealthLog(replSet);
    primaryDb[collName].drop();

    // Create indexes and insert docs without inserting corresponding index keys.
    insertDocsWithMissingIndexKeys(collName, doc, numDocs);

    runDbCheck(replSet, primary.getDB(dbName), collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck"
    });

    forEachNonArbiterNode(replSet, function(node) {
        // Verify that dbCheck caught the missing index keys inconsistency and logged one health log
        // entry per inconsistency.
        checkHealthLog(node.getDB("local").system.healthlog,
                       getMissingIndexKeysQuery(Object.keys(doc).length),
                       numDocs);
        // Verify that dbCheck did not log any additional errors outside of the missing keys
        // inconsistency.
        checkHealthLog(node.getDB("local").system.healthlog, errQuery, numDocs);
    });
}

function testMultipleMissingKeys() {
    jsTestLog(
        "Testing that dbCheck logs error in health log for each document with missing index key.");

    // Test for documents with 1 or multiple missing index keys.
    checkMissingIndexKeys(doc1);
    checkMissingIndexKeys(doc2);

    // Test for multiple documents with missing index keys in 1 batch.
    checkMissingIndexKeys(doc1, 10);
    checkMissingIndexKeys(doc2, 10);

    // Test for multiple batches with missing index keys.
    checkMissingIndexKeys(doc1, 10, 2);
}

function testMultipleDocsOneInconsistency() {
    jsTestLog(
        "Testing that dbCheck catches the case where there are multiple identical docs but only one of them has an inconsistency");
    clearHealthLog(replSet);
    primaryDb[collName].drop();

    // Insert multiple docs that are consistent.
    insertDocsWithMissingIndexKeys(collName, doc1, 10, false, false);

    // Insert one doc that is inconsistent.
    insertDocsWithMissingIndexKeys(collName, doc1, 1, true, true);

    runDbCheck(replSet,
               primaryDb,
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true);

    forEachNonArbiterNode(replSet, function(node) {
        // Verify that only one missing keys inconsistency was caught.
        checkHealthLog(node.getDB("local").system.healthlog, missingKeyErrQuery, 1);
        // Verify that there were no other error entries in the health log.
        checkHealthLog(node.getDB("local").system.healthlog, errQuery, 1);
    });
}

function testNoInconsistencies() {
    jsTestLog("Testing that dbCheck does not log an error when there are no index inconsistencies");
    clearHealthLog(replSet);
    primaryDb[collName].drop();

    // Insert documents without any inconsistencies.
    insertDocsWithMissingIndexKeys(collName, doc1, 2, false, false);

    runDbCheck(replSet,
               primaryDb,
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true);

    // Verify that no error health log entries were logged.
    forEachNonArbiterNode(replSet, function(node) {
        checkHealthLog(node.getDB("local").system.healthlog, errQuery, 0);
    });
}

function testPrimaryOnly() {
    jsTestLog(
        "Testing that dbCheck logs error in health log for missing index keys on the primary.");

    clearHealthLog(replSet);
    primaryDb[collName].drop();

    // Insert a document that has missing index keys on the primary only.
    insertDocsWithMissingIndexKeys(collName, doc1, 1, true, false);
    replSet.awaitReplication();

    runDbCheck(replSet,
               primary.getDB(dbName),
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true);

    // Verify that the primary has a missing key health log entry, but the secondary does not.
    checkHealthLog(primary.getDB("local").system.healthlog, getMissingIndexKeysQuery(1), 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, getMissingIndexKeysQuery(1), 0);

    // Verify that both the primary and secondary don't have any other errors.
    checkHealthLog(primary.getDB("local").system.healthlog, errQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, errQuery, 0);
}

function testSecondaryOnly() {
    jsTestLog(
        "Testing that dbCheck logs error in health log for missing index keys on the secondary.");

    clearHealthLog(replSet);
    primaryDb[collName].drop();

    // Insert a document that has missing index keys on the secondary only.
    insertDocsWithMissingIndexKeys(collName, doc1, 1, false, true);
    replSet.awaitReplication();

    runDbCheck(replSet,
               primary.getDB(dbName),
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true);

    // Verify that the secondary has a missing key health log entry, but the primary does not.
    checkHealthLog(primary.getDB("local").system.healthlog, getMissingIndexKeysQuery(1), 0);
    checkHealthLog(secondary.getDB("local").system.healthlog, getMissingIndexKeysQuery(1), 1);

    // Verify that both the primary and secondary don't have any other errors.
    checkHealthLog(primary.getDB("local").system.healthlog, errQuery, 0);
    checkHealthLog(secondary.getDB("local").system.healthlog, errQuery, 1);
}

testMultipleMissingKeys();
testMultipleDocsOneInconsistency();
testNoInconsistencies();
testPrimaryOnly();
testSecondaryOnly();

replSet.stopSet();
