// Test the awaitData flag for the find/getMore commands.
//
// @tags: [
//   # This test attempts to perform a getMore command and find it using the currentOp command. The
//   # former operation may be routed to a secondary in the replica set, whereas the latter must be
//   # routed to the primary.
//   assumes_read_preference_unchanged,
//   requires_capped,
//   requires_getmore,
//   uses_multiple_connections,
//   uses_parallel_shell,
// ]

(function() {
'use strict';

load("jstests/libs/fixture_helpers.js");
load('jstests/libs/discover_topology.js');  // For Topology and DiscoverTopology.

var cmdRes;
var cursorId;
var defaultBatchSize = 101;
var collName = 'await_data';
var coll = db[collName];

// Create a non-capped collection with 10 documents.
coll.drop();
for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// Find with tailable flag set should fail for a non-capped collection.
cmdRes = db.runCommand({find: collName, tailable: true});
assert.commandFailed(cmdRes);

// Should also fail in the non-capped case if both the tailable and awaitData flags are set.
cmdRes = db.runCommand({find: collName, tailable: true, awaitData: true});
assert.commandFailed(cmdRes);

// With a non-existent collection, should succeed but return no data and a closed cursor.
coll.drop();
cmdRes = assert.commandWorked(db.runCommand({find: collName, tailable: true}));
assert.eq(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.firstBatch.length, 0);

// Create a capped collection with 10 documents.
assert.commandWorked(db.createCollection(collName, {capped: true, size: 2048}));
for (var i = 0; i < 10; i++) {
    assert.commandWorked(coll.insert({a: i}));
}

// GetMore should succeed if query has awaitData but no maxTimeMS is supplied.
cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Should also succeed if maxTimeMS is supplied on the original find.
const sixtyMinutes = 60 * 60 * 1000;
cmdRes = db.runCommand(
    {find: collName, batchSize: 2, awaitData: true, tailable: true, maxTimeMS: sixtyMinutes});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);
cmdRes = db.runCommand({getMore: cmdRes.cursor.id, collection: collName});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Check that we can set up a tailable cursor over the capped collection.
cmdRes = db.runCommand({find: collName, batchSize: 5, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 5);

// Check that tailing the capped collection with awaitData eventually ends up returning an empty
// batch after hitting the timeout.
cmdRes = db.runCommand({find: collName, batchSize: 2, awaitData: true, tailable: true});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 2);

// Issue getMore until we get an empty batch of results.
cmdRes = db.runCommand({
    getMore: cmdRes.cursor.id,
    collection: coll.getName(),
    batchSize: NumberInt(2),
    maxTimeMS: 4000
});
assert.commandWorked(cmdRes);
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());

// Keep issuing getMore until we get an empty batch after the timeout expires.
while (cmdRes.cursor.nextBatch.length > 0) {
    var now = new Date();
    cmdRes = db.runCommand({
        getMore: cmdRes.cursor.id,
        collection: coll.getName(),
        batchSize: NumberInt(2),
        maxTimeMS: 4000
    });
    assert.commandWorked(cmdRes);
    assert.gt(cmdRes.cursor.id, NumberLong(0));
    assert.eq(cmdRes.cursor.ns, coll.getFullName());
}
assert.gte((new Date()) - now, 2000);

// Repeat the test, this time tailing the oplog rather than a user-created capped collection.
// The oplog tailing in not possible on mongos.
if (FixtureHelpers.isReplSet(db)) {
    var localDB = db.getSiblingDB("local");
    var oplogColl = localDB.oplog.rs;

    cmdRes = localDB.runCommand(
        {find: oplogColl.getName(), batchSize: 2, awaitData: true, tailable: true});
    assert.commandWorked(cmdRes);
    if (cmdRes.cursor.id > NumberLong(0)) {
        assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
        assert.eq(cmdRes.cursor.firstBatch.length, 2);

        cmdRes = localDB.runCommand(
            {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 1000});
        assert.commandWorked(cmdRes);
        assert.gt(cmdRes.cursor.id, NumberLong(0));
        assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());

        while (cmdRes.cursor.nextBatch.length > 0) {
            now = new Date();
            cmdRes = localDB.runCommand(
                {getMore: cmdRes.cursor.id, collection: oplogColl.getName(), maxTimeMS: 4000});
            assert.commandWorked(cmdRes);
            assert.gt(cmdRes.cursor.id, NumberLong(0));
            assert.eq(cmdRes.cursor.ns, oplogColl.getFullName());
        }
        assert.gte((new Date()) - now, 2000);
    }
}

// Test filtered inserts while writing to a capped collection.
// Find with a filter which doesn't match any documents in the collection.
cmdRes = assert.commandWorked(db.runCommand({
    find: collName,
    batchSize: 2,
    filter: {x: 1},
    awaitData: true,
    tailable: true,
    comment: "uniquifier_comment"
}));
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.firstBatch.length, 0);

// The code below will test for the tailable, awaitData cursor being awaken once a matching document
// is inserted and expects the document to be returned in the next batch. However, the logic without
// waiting for the timeout in order to receive the data only works if the read concern is not set to
// majority. The reason for this is that the primary node will notify the waiting cursor on document
// being inserted on its node and not on the majority of nodes. However, since the read concern is
// set to majority, the awaken cursor won't find the newly inserted document, as at that time it is
// present on the primary only. Therefore it will be waiting till the timeout. In order to avoid the
// waiting we stop running this test if read concern majority.
const topology = DiscoverTopology.findConnectedNodes(db.getMongo());
if (topology.type !== Topology.kStandalone) {
    const readConcern =
        assert.commandWorked(db.adminCommand({getDefaultRWConcern: 1})).defaultReadConcern;
    if (readConcern.level == "majority" || TestData.defaultReadConcernLevel === "majority") {
        return;
    }
}

// Test that a getMore command on a tailable, awaitData cursor does not return a new batch to
// the user if a document was inserted, but it did not match the filter.
let insertshell = startParallelShell(() => {
    // Signal to the original shell that the parallel shell has successfully started.
    assert.commandWorked(db.await_data.insert({_id: "signal parent shell"}));

    // Wait for the parent shell to start watching for the next document.
    jsTestLog("Checking getMore is being blocked...");
    assert.soon(() => db.currentOp({
                            op: "getmore",
                            "cursor.originatingCommand.comment": "uniquifier_comment"
                        }).inprog.length == 1,
                () => tojson(db.currentOp().inprog));

    // Now write a non-matching document to the collection.
    assert.commandWorked(db.await_data.insert({_id: "no match", x: 0}));

    // Make sure the getMore has not ended after a while.
    sleep(2000);
    jsTestLog("Checking getMore is still being blocked...");
    assert.eq(
        db.currentOp({op: "getmore", "cursor.originatingCommand.comment": "uniquifier_comment"})
            .inprog.length,
        1,
        tojson(db.currentOp().inprog));

    // Now write a matching document to wake it up.
    jsTestLog("Sending signal to getMore...");
    assert.commandWorked(db.await_data.insert({_id: "match", x: 1}));
    jsTestLog("Insertion shell finished successfully.");
});

// Wait until we receive confirmation that the parallel shell has started.
assert.soon(() => db.await_data.findOne({_id: "signal parent shell"}) !== null);

// Now issue a getMore which will match the parallel shell's currentOp filter, signalling it to
// write a non-matching document into the collection. Confirm that we do not receive this
// document and that we subsequently time out.
cmdRes = db.runCommand(
    {getMore: cmdRes.cursor.id, collection: collName, maxTimeMS: ReplSetTest.kDefaultTimeoutMS});
assert.commandWorked(cmdRes);
jsTestLog("Waiting insertion shell to terminate...");
assert.eq(insertshell(), 0);
jsTestLog("Insertion shell terminated.");
assert.gt(cmdRes.cursor.id, NumberLong(0));
assert.eq(cmdRes.cursor.ns, coll.getFullName());
assert.eq(cmdRes.cursor.nextBatch.length,
          1,
          'Collection documents: ' + tojson(db.await_data.find({}).toArray()));
assert.docEq(cmdRes.cursor.nextBatch[0], {_id: "match", x: 1});
})();
