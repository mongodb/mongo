// Tests that entries are written to the change collection for collection create, drop and document
// modification operations.
// @tags: [
//   multiversion_incompatible,
//   featureFlagMongoStore,
//   requires_fcv_61,
// ]
(function() {
"use strict";

const replSetTest = new ReplSetTest({nodes: 2});

// TODO SERVER-67267 add 'featureFlagServerlessChangeStreams', 'multitenancySupport' and
// 'serverless' flags and remove 'failpoint.forceEnableChangeCollectionsMode'.
replSetTest.startSet(
    {setParameter: "failpoint.forceEnableChangeCollectionsMode=" + tojson({mode: "alwaysOn"})});

replSetTest.initiate();

const primary = replSetTest.getPrimary();
const secondary = replSetTest.getSecondary();
const testDb = primary.getDB("test");

// Verifies that the oplog and change collection entries are the same for the specified start and
// end duration of the oplog timestamp.
function verifyChangeCollectionEntries(connection, startOplogTimestamp, endOplogTimestamp) {
    const oplogColl = connection.getDB("local").oplog.rs;
    const changeColl = connection.getDB("config").system.change_collection;

    // Fetch all oplog and change collection entries for the duration: [startOplogTimestamp,
    // endOplogTimestamp].
    const oplogEntries =
        oplogColl.find({$and: [{ts: {$gte: startOplogTimestamp}}, {ts: {$lte: endOplogTimestamp}}]})
            .toArray();
    const changeCollectionEntries =
        changeColl
            .find({$and: [{_id: {$gte: startOplogTimestamp}}, {_id: {$lte: endOplogTimestamp}}]})
            .toArray();

    assert.eq(
        oplogEntries.length,
        changeCollectionEntries.length,
        "Number of entries in the oplog and the change collection is not the same. Oplog has total " +
            oplogEntries.length + " entries , change collection has total " +
            changeCollectionEntries.length + " entries");

    for (let idx = 0; idx < oplogEntries.length; idx++) {
        const oplogEntry = oplogEntries[idx];
        const changeCollectionEntry = changeCollectionEntries[idx];

        // Remove the '_id' field from the change collection as oplog does not have it.
        assert(changeCollectionEntry.hasOwnProperty("_id"));
        assert.eq(timestampCmp(changeCollectionEntry._id, oplogEntry.ts),
                  0,
                  "Change collection '_id' field: " + tojson(changeCollectionEntry._id) +
                      " is not same as the oplog 'ts' field: " + tojson(oplogEntry.ts));
        delete changeCollectionEntry["_id"];

        // Verify that the oplog and change collecton entry (after removing the '_id') field are
        // the same.
        assert.eq(
            oplogEntry,
            changeCollectionEntry,
            "Oplog and change collection entries are not same. Oplog entry: " + tojson(oplogEntry) +
                ", change collection entry: " + tojson(changeCollectionEntry));
    }
}

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
