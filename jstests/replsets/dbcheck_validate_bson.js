/**
 * Test BSON validation in the dbCheck command.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck,
 *   corrupts_data,
 * ]
 */

import {
    checkHealthLog,
    clearHealthLog,
    forEachNonArbiterNode,
    resetAndInsert,
    runDbCheck
} from "jstests/replsets/libs/dbcheck_utils.js";

// Skipping data consistency checks because invalid BSON is inserted into primary and secondary
// separately.
TestData.skipCollectionAndIndexValidation = true;
TestData.skipCheckDBHashes = true;

const dbName = "dbCheckBSONValidation";
const collName = "dbCheckBSONValidation-collection";

const replSet = new ReplSetTest({
    name: jsTestName(),
    nodes: 2,
    nodeOptions: {setParameter: {dbCheckHealthLogEveryNBatches: 1}}
});
replSet.startSet();
replSet.initiateWithHighElectionTimeout();

const primary = replSet.getPrimary();
const primaryDb = primary.getDB(dbName);
const secondary = replSet.getSecondary();
const secondaryDb = secondary.getDB(dbName);
const nDocs = 10;
const maxDocsPerBatch = 2;

const errQuery = {
    operation: "dbCheckBatch",
    severity: "error",
};
const invalidBSONQuery = {
    operation: "dbCheckBatch",
    severity: "error",
    msg: "Document is not well-formed BSON",
    "data.context.recordID": {$exists: true}
};
const invalidHashQuery = {
    operation: "dbCheckBatch",
    severity: "error",
    msg: "dbCheck batch inconsistent",
    "data.md5": {$exists: true}
};
const successfulBatchQuery = {
    operation: "dbCheckBatch",
    severity: "info",
    msg: "dbCheck batch consistent",
    "data.count": maxDocsPerBatch
};

const doc1 = {
    _id: 0,
    a: 1
};

// The default WC is majority and godinsert command on a secondary is incompatible with
// wc:majority.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Insert corrupt document for testing via failpoint.
const insertCorruptDocument = function(db, collName) {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "alwaysOn"}));
    // Use godinsert to insert into the node directly.
    assert.commandWorked(db.runCommand({godinsert: collName, obj: doc1}));
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "off"}));
};

function testKDefaultBSONValidation() {
    jsTestLog("Testing that dbCheck logs error in health log when node has kDefault invalid BSON.");

    clearHealthLog(replSet);

    // Insert invalid BSON that fails the kDefault check into both nodes in the replica set.
    assert.commandWorked(primaryDb.createCollection(collName));
    replSet.awaitReplication();
    forEachNonArbiterNode(replSet, function(node) {
        insertCorruptDocument(node.getDB(dbName), collName);
    });

    // Both primary and secondary should have error in health log for invalid BSON.
    runDbCheck(
        replSet, primaryDb, collName, {validateMode: "dataConsistencyAndMissingIndexKeysCheck"});

    // Primary and secondary have the same invalid BSON document so there is an error for invalid
    // BSON but not data inconsistency. We check that the only errors in the health log are for
    // invalid BSON.
    checkHealthLog(primary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(primary.getDB("local").system.healthlog, errQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, errQuery, 1);
}

function testPrimaryInvalidBson() {
    jsTestLog("Testing when the primary has invalid BSON but the secondary does not.");

    clearHealthLog(replSet);

    // Insert an invalid document on the primary.
    insertCorruptDocument(primaryDb, collName);

    // Insert a normal document on the secondary.
    assert.commandWorked(secondaryDb.runCommand({godinsert: collName, obj: doc1}));

    runDbCheck(replSet, primaryDb, collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck"
    });

    // Verify primary logs an error for invalid BSON and data inconsistency.
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidHashQuery, 1);
}

function testMultipleBatches() {
    jsTestLog("Testing that invalid BSON check works across multiple batches.");

    clearHealthLog(replSet);

    // Primary contains 10 valid documents, secondary contains 9 valid and 1 invalid document.
    resetAndInsert(replSet, primaryDb, collName, nDocs - 1);
    assert.commandWorked(primaryDb.runCommand({godinsert: collName, obj: {_id: 0, a: nDocs}}));
    insertCorruptDocument(secondaryDb, collName);

    runDbCheck(replSet, primaryDb, collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck"
    });

    // Secondary logs an error for invalid BSON and data inconsistency.
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidHashQuery, 1);

    // Primary and secondary do not terminate after first batch and finish dbCheck successfully.
    checkHealthLog(
        primary.getDB("local").system.healthlog, successfulBatchQuery, nDocs / maxDocsPerBatch);
    checkHealthLog(secondary.getDB("local").system.healthlog,
                   successfulBatchQuery,
                   (nDocs / maxDocsPerBatch) - 1);
    // There should be no other error queries.
    checkHealthLog(primary.getDB("local").system.healthlog, errQuery, 0);
    checkHealthLog(secondary.getDB("local").system.healthlog, errQuery, 2);
    checkHealthLog(primary.getDB("local").system.healthlog, {operation: "dbCheckStop"}, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, {operation: "dbCheckStop"}, 1);
}

testKDefaultBSONValidation();
testMultipleBatches();

replSet.stopSet();
