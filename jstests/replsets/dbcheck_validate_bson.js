/**
 * Test BSON validation in the dbCheck command.
 *
 * @tags: [
 *   featureFlagSecondaryIndexChecksInDbCheck,
 *   corrupts_data,
 * ]
 */

import {ReplSetTest} from "jstests/libs/replsettest.js";
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
replSet.initiate();

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
const warningQuery = {
    operation: "dbCheckBatch",
    severity: "warning"
};
const BSONWarningQuery = {
    operation: "dbCheckBatch",
    severity: "warning",
    msg: "Document is not well-formed BSON"
};
const successfulBatchQuery = {
    operation: "dbCheckBatch",
    severity: "info",
    msg: "dbCheck batch consistent",
    "data.count": maxDocsPerBatch
};
const errAndWarningQuery = {
    operation: "dbCheckBatch",
    $or: [{severity: "error"}, {severity: "warning"}]
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
    runDbCheck(replSet,
               primaryDb,
               collName,
               {validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
               true /* awaitCompletion */);

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

    primaryDb[collName].drop();
    assert.commandWorked(primaryDb.createCollection(collName));
    replSet.awaitReplication();

    // Insert an invalid document on the primary.
    insertCorruptDocument(primaryDb, collName);

    // Insert a normal document on the secondary.
    assert.commandWorked(secondaryDb.runCommand({godinsert: collName, obj: doc1}));

    runDbCheck(
        replSet,
        primaryDb,
        collName,
        {maxDocsPerBatch: maxDocsPerBatch, validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
        true /* awaitCompletion */);

    // Verify primary logs an error for invalid BSON while the secondary logs an error for data
    // inconsistency.
    checkHealthLog(primary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidHashQuery, 1);

    // Verify that the primary and secondary do not have other error/warning logs.
    checkHealthLog(primary.getDB("local").system.healthlog, errAndWarningQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, errAndWarningQuery, 1);
}

function testSecondaryInvalidBson() {
    jsTestLog("Testing when the secondary has invalid BSON but the primary does not.");

    clearHealthLog(replSet);

    primaryDb[collName].drop();
    assert.commandWorked(primaryDb.createCollection(collName));
    replSet.awaitReplication();

    // Insert a normal document on the primary.
    assert.commandWorked(primaryDb.runCommand({godinsert: collName, obj: doc1}));

    // Insert an invalid document on the secondary.
    insertCorruptDocument(secondaryDb, collName);

    runDbCheck(
        replSet,
        primaryDb,
        collName,
        {maxDocsPerBatch: maxDocsPerBatch, validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
        true /* awaitCompletion */);

    // Verify secondary logs an error for invalid BSON and data inconsistency.
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidBSONQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, invalidHashQuery, 1);

    // Verify that the primary and secondary do not have other error/warning logs.
    checkHealthLog(primary.getDB("local").system.healthlog, errAndWarningQuery, 0);
    checkHealthLog(secondary.getDB("local").system.healthlog, errAndWarningQuery, 2);
}

function testMultipleBatches() {
    jsTestLog("Testing that invalid BSON check works across multiple batches.");

    clearHealthLog(replSet);

    // Primary contains 10 valid documents, secondary contains 9 valid and 1 invalid document.
    resetAndInsert(replSet, primaryDb, collName, nDocs - 1);
    assert.commandWorked(primaryDb.runCommand({godinsert: collName, obj: {_id: 0, a: nDocs}}));
    insertCorruptDocument(secondaryDb, collName);

    runDbCheck(
        replSet,
        primaryDb,
        collName,
        {maxDocsPerBatch: maxDocsPerBatch, validateMode: "dataConsistencyAndMissingIndexKeysCheck"},
        true /* awaitCompletion */);

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

function testInvalidUuid() {
    jsTestLog(
        "Testing that a BSON document that is structurally valid but invalid in other ways (such as by having UUID that have incorrect lengths) is included in hashing but logs a warning");
    clearHealthLog(replSet);

    primaryDb[collName].drop();
    assert.commandWorked(primaryDb.createCollection(collName));
    replSet.awaitReplication();

    // Insert 2 documents with invalid UUID (length is 4 or 20 instead of 16).
    assert.commandWorked(primaryDb[collName].insert({u: HexData(4, "deadbeef")}));
    assert.commandWorked(
        primaryDb[collName].insert({u: HexData(20, "deadbeefdeadbeefdeadbeefdeadbeef")}));
    replSet.awaitReplication();

    runDbCheck(replSet,
               primaryDb,
               collName,
               {
                   maxDocsPerBatch: maxDocsPerBatch,
                   validateMode: "dataConsistencyAndMissingIndexKeysCheck",
                   bsonValidateMode: "kExtended"
               },
               true /* awaitCompletion */);

    // Verify both primary and secondary log a warning for invalid BSON (k).
    checkHealthLog(primary.getDB("local").system.healthlog, BSONWarningQuery, 2);
    checkHealthLog(secondary.getDB("local").system.healthlog, BSONWarningQuery, 2);

    // Verify that the primary and secondary do not have other error/warning logs.
    checkHealthLog(primary.getDB("local").system.healthlog, successfulBatchQuery, 1);
    checkHealthLog(secondary.getDB("local").system.healthlog, successfulBatchQuery, 1);
    checkHealthLog(primary.getDB("local").system.healthlog, errAndWarningQuery, 2);
    checkHealthLog(secondary.getDB("local").system.healthlog, errAndWarningQuery, 2);
}

testKDefaultBSONValidation();
testPrimaryInvalidBson();
testSecondaryInvalidBson();
testMultipleBatches();
testInvalidUuid();

replSet.stopSet();
