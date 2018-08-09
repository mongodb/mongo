// This test ensures that change streams on sharded collections start in sync with each other.
//
// As detailed in SERVER-31685, since shard cursors are not established simultaneously, it is
// possible that a sharded change stream could be established on shard 0, then write 'A' to shard 0
// could occur, followed by write 'B' to shard 1, and then the change stream could be established on
// shard 1, then some third write 'C' could occur.  This test ensures that in that case, both 'A'
// and 'B' will be seen in the changestream before 'C'.
// @tags: [requires_majority_read_concern]
(function() {
    "use strict";

    load('jstests/aggregation/extras/utils.js');  // For assertErrorCode().
    load('jstests/libs/change_stream_util.js');   // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        mongos: 2,
        useBridge: true,
        rs: {
            nodes: 1,
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    function checkStream() {
        db = db.getSiblingDB(jsTestName());
        let coll = db[jsTestName()];
        let changeStream = coll.aggregate([{$changeStream: {}}, {$project: {_id: 0}}]);

        assert.soon(() => changeStream.hasNext());
        assert.docEq(changeStream.next(), {
            documentKey: {_id: -1000},
            fullDocument: {_id: -1000},
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "insert",
        });

        assert.soon(() => changeStream.hasNext());
        assert.docEq(changeStream.next(), {
            documentKey: {_id: 1001},
            fullDocument: {_id: 1001},
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "insert",
        });

        assert.soon(() => changeStream.hasNext());
        assert.docEq(changeStream.next(), {
            documentKey: {_id: -1002},
            fullDocument: {_id: -1002},
            ns: {db: db.getName(), coll: coll.getName()},
            operationType: "insert",
        });
        changeStream.close();
    }

    // Start the $changeStream with shard 1 unavailable on the second mongos (s1).  We will be
    // writing through the first mongos (s0), which will remain connected to all shards.
    st.rs1.getPrimary().disconnect(st.s1);
    let waitForShell = startParallelShell(checkStream, st.s1.port);

    // Wait for the aggregate cursor to appear in currentOp on the current shard.
    function waitForShardCursor(rs) {
        assert.soon(
            () => st.rs0.getPrimary()
                      .getDB('admin')
                      .aggregate(
                          [{"$listLocalCursors": {}}, {"$match": {ns: mongosColl.getFullName()}}])
                      .itcount() === 1);
    }
    // Make sure the shard 0 $changeStream cursor is established before doing the first writes.
    waitForShardCursor(st.rs0);

    assert.writeOK(mongosColl.insert({_id: -1000}, {writeConcern: {w: "majority"}}));

    // This write to shard 1 occurs before the $changeStream cursor on shard 1 is open, because the
    // mongos where the $changeStream is running is disconnected from shard 1.
    assert.writeOK(mongosColl.insert({_id: 1001}, {writeConcern: {w: "majority"}}));

    jsTestLog("Reconnecting");
    st.rs1.getPrimary().reconnect(st.s1);
    waitForShardCursor(st.rs1);

    assert.writeOK(mongosColl.insert({_id: -1002}, {writeConcern: {w: "majority"}}));
    waitForShell();
    st.stop();
})();
