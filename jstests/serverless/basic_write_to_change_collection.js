// Tests that entries are written to the change collection for collection create, drop and document
// modification operations.
// @tags: [
//   featureFlagMongoStore,
//   requires_fcv_61,
// ]
(function() {
"use strict";

load("jstests/serverless/libs/change_collection_util.js");  // For verifyChangeCollectionEntries.

const replSetTest = new ReplSetTest({nodes: 2});

// TODO SERVER-67267 add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
// 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
replSetTest.startSet(
    {setParameter: "failpoint.forceEnableChangeCollectionsMode=" + tojson({mode: "alwaysOn"})});

replSetTest.initiate();

const primary = replSetTest.getPrimary();
const secondary = replSetTest.getSecondary();
const testDb = primary.getDB("test");

// Performs writes on the specified collection.
function performWrites(coll) {
    const docIds = [1, 2, 3, 4, 5];
    docIds.forEach(docId => assert.commandWorked(coll.insert({_id: docId})));
    docIds.forEach(
        docId => assert.commandWorked(coll.update({_id: docId}, {$set: {annotate: "updated"}})));
}

// Test the change collection entries with the oplog by performing some basic writes.
(function testBasicWritesInChangeCollection() {
    const oplogColl = primary.getDB("local").oplog.rs;
    const startOplogTimestamp = oplogColl.find().toArray().at(-1).ts;
    assert(startOplogTimestamp != undefined);

    performWrites(testDb.stock);
    assert(testDb.stock.drop());

    const endOplogTimestamp = oplogColl.find().toArray().at(-1).ts;
    assert(endOplogTimestamp !== undefined);
    assert(timestampCmp(endOplogTimestamp, startOplogTimestamp) > 0);

    // Wait for the replication to finish.
    replSetTest.awaitReplication();

    // Verify that the change collection entries are the same as the oplog in the primary and the
    // secondary node.
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp);
    verifyChangeCollectionEntries(secondary, startOplogTimestamp, endOplogTimestamp);
})();

// Test the change collection entries with the oplog by performing writes in a transaction.
(function testWritesinChangeCollectionWithTrasactions() {
    const oplogColl = primary.getDB("local").oplog.rs;
    const startOplogTimestamp = oplogColl.find().toArray().at(-1).ts;
    assert(startOplogTimestamp != undefined);

    const session = testDb.getMongo().startSession();
    const sessionDb = session.getDatabase(testDb.getName());
    session.startTransaction();
    performWrites(sessionDb.test);
    session.commitTransaction_forTesting();

    const endOplogTimestamp = oplogColl.find().toArray().at(-1).ts;
    assert(endOplogTimestamp != undefined);
    assert(timestampCmp(endOplogTimestamp, startOplogTimestamp) > 0);

    // Wait for the replication to finish.
    replSetTest.awaitReplication();

    // Verify that the change collection entries are the same as the oplog in the primary and the
    // secondary node for the applyOps.
    verifyChangeCollectionEntries(primary, startOplogTimestamp, endOplogTimestamp);
    verifyChangeCollectionEntries(secondary, startOplogTimestamp, endOplogTimestamp);
})();

replSetTest.stopSet();
}());
