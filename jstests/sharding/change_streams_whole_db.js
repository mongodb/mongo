// Tests the behavior of a change stream on a whole database in a sharded cluster.
(function() {
    "use strict";

    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
    load('jstests/aggregation/extras/utils.js');       // For assertErrorCode().
    load('jstests/libs/change_stream_util.js');        // For ChangeStreamTest.

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        }
    });

    const mongosDB = st.s0.getDB("test");

    // TODO SERVER-34138 will add support for opening a change stream before a database exists.
    assert.commandFailedWithCode(
        mongosDB.runCommand(
            {aggregate: 1, pipeline: [{$changeStream: {}}], cursor: {batchSize: 1}}),
        ErrorCodes.NamespaceNotFound);

    const mongosColl = mongosDB[jsTestName()];
    mongosDB.createCollection(jsTestName());

    let cst = new ChangeStreamTest(mongosDB);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns operations on the unsharded test collection.
    assert.writeOK(mongosColl.insert({_id: 0}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0},
        ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Create a new sharded collection.
    mongosDB.createCollection(jsTestName() + "_sharded_on_x");
    const mongosCollShardedOnX = mongosDB[jsTestName() + "_sharded_on_x"];

    // Shard, split, and move one chunk to shard1.
    st.shardColl(mongosCollShardedOnX.getName(), {x: 1}, {x: 0}, {x: 1}, mongosDB.getName());

    // Write a document to each chunk.
    assert.writeOK(mongosCollShardedOnX.insert({_id: 0, x: -1}));
    assert.writeOK(mongosCollShardedOnX.insert({_id: 1, x: 1}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 0, x: -1},
          fullDocument: {_id: 0, x: -1},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1, x: 1},
          fullDocument: {_id: 1, x: 1},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedOnX.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Now send inserts to both the sharded and unsharded collections, and verify that the change
    // streams returns them in order.
    assert.writeOK(mongosCollShardedOnX.insert({_id: 2, x: 2}));
    assert.writeOK(mongosColl.insert({_id: 1}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 2, x: 2},
          fullDocument: {_id: 2, x: 2},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1},
          fullDocument: {_id: 1},
          ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Create a third sharded collection with a compound shard key.
    mongosDB.createCollection(jsTestName() + "_sharded_compound");
    const mongosCollShardedCompound = mongosDB[jsTestName() + "_sharded_compound"];

    // Shard, split, and move one chunk to shard1.
    st.shardColl(mongosCollShardedCompound.getName(),
                 {y: 1, x: 1},
                 {y: 1, x: MinKey},
                 {y: 1, x: MinKey},
                 mongosDB.getName());

    // Write a document to each chunk.
    assert.writeOK(mongosCollShardedCompound.insert({_id: 0, y: -1, x: 0}));
    assert.writeOK(mongosCollShardedCompound.insert({_id: 1, y: 1, x: 0}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 0, y: -1, x: 0},
          fullDocument: {_id: 0, y: -1, x: 0},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedCompound.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1, y: 1, x: 0},
          fullDocument: {_id: 1, y: 1, x: 0},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedCompound.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Send inserts to all 3 collections and verify that the results contain the correct
    // documentKeys and are in the correct order.
    assert.writeOK(mongosCollShardedOnX.insert({_id: 3, x: 3}));
    assert.writeOK(mongosColl.insert({_id: 3}));
    assert.writeOK(mongosCollShardedCompound.insert({_id: 2, x: 0, y: -2}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 3, x: 3},
          fullDocument: {_id: 3, x: 3},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 3},
          fullDocument: {_id: 3},
          ns: {db: mongosDB.getName(), coll: mongosColl.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 2, x: 0, y: -2},
          fullDocument: {_id: 2, x: 0, y: -2},
          ns: {db: mongosDB.getName(), coll: mongosCollShardedCompound.getName()},
          operationType: "insert",
        },
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    cst.cleanUp();

    st.stop();
})();
