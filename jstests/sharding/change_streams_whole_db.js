// Tests the behavior of a change stream on a whole database in a sharded cluster.
// @tags: [uses_change_streams]
(function() {
    "use strict";

    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.
    load('jstests/aggregation/extras/utils.js');       // For assertErrorCode().
    load('jstests/libs/change_stream_util.js');        // For ChangeStreamTest.
    load("jstests/libs/collection_drop_recreate.js");  // For assertDropCollection.

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

    const merizosDB = st.s0.getDB("test");
    const merizosColl = merizosDB[jsTestName()];

    let cst = new ChangeStreamTest(merizosDB);
    let cursor = cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: 1});

    // Test that if there are no changes, we return an empty batch.
    assert.eq(0, cursor.firstBatch.length, "Cursor had changes: " + tojson(cursor));

    // Test that the change stream returns operations on the unsharded test collection.
    assert.writeOK(merizosColl.insert({_id: 0}));
    let expected = {
        documentKey: {_id: 0},
        fullDocument: {_id: 0},
        ns: {db: merizosDB.getName(), coll: merizosColl.getName()},
        operationType: "insert",
    };
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: [expected]});

    // Create a new sharded collection.
    merizosDB.createCollection(jsTestName() + "_sharded_on_x");
    const merizosCollShardedOnX = merizosDB[jsTestName() + "_sharded_on_x"];

    // Shard, split, and move one chunk to shard1.
    st.shardColl(merizosCollShardedOnX.getName(), {x: 1}, {x: 0}, {x: 1}, merizosDB.getName());

    // Write a document to each chunk.
    assert.writeOK(merizosCollShardedOnX.insert({_id: 0, x: -1}));
    assert.writeOK(merizosCollShardedOnX.insert({_id: 1, x: 1}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 0, x: -1},
          fullDocument: {_id: 0, x: -1},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1, x: 1},
          fullDocument: {_id: 1, x: 1},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedOnX.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Now send inserts to both the sharded and unsharded collections, and verify that the change
    // streams returns them in order.
    assert.writeOK(merizosCollShardedOnX.insert({_id: 2, x: 2}));
    assert.writeOK(merizosColl.insert({_id: 1}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 2, x: 2},
          fullDocument: {_id: 2, x: 2},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1},
          fullDocument: {_id: 1},
          ns: {db: merizosDB.getName(), coll: merizosColl.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Create a third sharded collection with a compound shard key.
    merizosDB.createCollection(jsTestName() + "_sharded_compound");
    const merizosCollShardedCompound = merizosDB[jsTestName() + "_sharded_compound"];

    // Shard, split, and move one chunk to shard1.
    st.shardColl(merizosCollShardedCompound.getName(),
                 {y: 1, x: 1},
                 {y: 1, x: MinKey},
                 {y: 1, x: MinKey},
                 merizosDB.getName());

    // Write a document to each chunk.
    assert.writeOK(merizosCollShardedCompound.insert({_id: 0, y: -1, x: 0}));
    assert.writeOK(merizosCollShardedCompound.insert({_id: 1, y: 1, x: 0}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 0, y: -1, x: 0},
          fullDocument: {_id: 0, y: -1, x: 0},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedCompound.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 1, y: 1, x: 0},
          fullDocument: {_id: 1, y: 1, x: 0},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedCompound.getName()},
          operationType: "insert",
        }
    ];
    cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});

    // Send inserts to all 3 collections and verify that the results contain the correct
    // documentKeys and are in the correct order.
    assert.writeOK(merizosCollShardedOnX.insert({_id: 3, x: 3}));
    assert.writeOK(merizosColl.insert({_id: 3}));
    assert.writeOK(merizosCollShardedCompound.insert({_id: 2, x: 0, y: -2}));

    // Verify that the change stream returns both inserts.
    expected = [
        {
          documentKey: {_id: 3, x: 3},
          fullDocument: {_id: 3, x: 3},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedOnX.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 3},
          fullDocument: {_id: 3},
          ns: {db: merizosDB.getName(), coll: merizosColl.getName()},
          operationType: "insert",
        },
        {
          documentKey: {_id: 2, x: 0, y: -2},
          fullDocument: {_id: 2, x: 0, y: -2},
          ns: {db: merizosDB.getName(), coll: merizosCollShardedCompound.getName()},
          operationType: "insert",
        },
    ];

    const results = cst.assertNextChangesEqual({cursor: cursor, expectedChanges: expected});
    // Store the resume token of the first insert to use after dropping the collection.
    const resumeTokenBeforeDrop = results[0]._id;

    // Write one more document to the collection that will be dropped, to be returned after
    // resuming.
    assert.writeOK(merizosCollShardedOnX.insert({_id: 4, x: 4}));

    // Drop the collection, invalidating the open change stream.
    assertDropCollection(merizosDB, merizosCollShardedOnX.getName());

    // Resume the change stream from before the collection drop, and verify that the documentKey
    // field contains the extracted shard key from the resume token.
    cursor = cst.startWatchingChanges({
        pipeline: [
            {$changeStream: {resumeAfter: resumeTokenBeforeDrop}},
            {$match: {"ns.coll": merizosCollShardedOnX.getName()}}
        ],
        collection: 1
    });
    cst.assertNextChangesEqual({
        cursor: cursor,
        expectedChanges: [
            {
              documentKey: {_id: 4, x: 4},
              fullDocument: {_id: 4, x: 4},
              ns: {db: merizosDB.getName(), coll: merizosCollShardedOnX.getName()},
              operationType: "insert",
            },
        ]
    });

    cst.cleanUp();

    st.stop();
})();
