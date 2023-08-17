/**
 * Test BSON validation in the dbCheck command.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck
 * ]
 */

import {
    checkHealthlog,
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

const doc1 = {
    _id: 0,
    a: 1
};

// The default WC is majority and godinsert command on a secondary is incompatible with
// wc:majority.
assert.commandWorked(primary.adminCommand(
    {setDefaultRWConcern: 1, defaultWriteConcern: {w: 1}, writeConcern: {w: "majority"}}));

// Insert corrupt document for testing via failpoint.
let insertCorruptDocument = function(db, collName) {
    assert.commandWorked(
        db.adminCommand({configureFailPoint: "corruptDocumentOnInsert", mode: "alwaysOn"}));
    // Use godinsert to insert into primary and secondary individually.
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
    let query = {
        operation: "dbCheckBatch",
        severity: "error",
        msg: "Document is not well-formed BSON",
        scope: "document",
        "data.context.recordID": {$exists: true}
    };
    let errorQuery = {operation: "dbCheckBatch", severity: "error"};

    // Primary and secondary have the same invalid BSON document so there is an error for invalid
    // BSON but not data inconsistency. We check that the only errors in the health log are for
    // invalid BSON.
    checkHealthlog(primary.getDB("local").system.healthlog, query, 1);
    checkHealthlog(secondary.getDB("local").system.healthlog, query, 1);
    checkHealthlog(primary.getDB("local").system.healthlog, errorQuery, 1);
    checkHealthlog(secondary.getDB("local").system.healthlog, errorQuery, 1);
}

function testMultipleBatches() {
    jsTestLog("Testing that invalid BSON check works across multiple batches.");

    // Primary contains 10 valid documents, secondary contains 9 valid and 1 invalid document.
    resetAndInsert(replSet, primaryDb, collName, nDocs - 1);
    assert.commandWorked(primaryDb.runCommand({godinsert: collName, obj: {_id: 0, a: nDocs}}));
    insertCorruptDocument(secondaryDb, collName);

    // Secondary includes invalid BSON when hashing so there is data inconsistency.
    runDbCheck(replSet, primaryDb, collName, {
        maxDocsPerBatch: maxDocsPerBatch,
        validateMode: "dataConsistencyAndMissingIndexKeysCheck"
    });

    let invalidBSONQuery = {
        operation: "dbCheckBatch",
        severity: "error",
        msg: "Document is not well-formed BSON",
        scope: "document",
        "data.context.recordID": {$exists: true}
    };
    let invalidHashQuery = {
        operation: "dbCheckBatch",
        severity: "error",
        msg: "dbCheck batch inconsistent",
        scope: "cluster",
        "data.md5": {$exists: true}
    };
    let successfulBatchQuery = {
        operation: "dbCheckBatch",
        severity: "info",
        msg: "dbCheck batch consistent",
        "data.count": maxDocsPerBatch
    };

    // Secondary logs an error for invalid BSON and data inconsistency.
    checkHealthlog(secondary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthlog(secondary.getDB("local").system.healthlog, invalidHashQuery, 1);

    // Primary and secondary do not terminate after first batch and finish dbCheck successfully.
    checkHealthlog(
        primary.getDB("local").system.healthlog, successfulBatchQuery, nDocs / maxDocsPerBatch);
    checkHealthlog(secondary.getDB("local").system.healthlog,
                   successfulBatchQuery,
                   (nDocs / maxDocsPerBatch) - 1);
    checkHealthlog(primary.getDB("local").system.healthlog, {operation: "dbCheckStop"}, 1);
    checkHealthlog(secondary.getDB("local").system.healthlog, {operation: "dbCheckStop"}, 1);
}

testKDefaultBSONValidation();
testMultipleBatches();

replSet.stopSet();
