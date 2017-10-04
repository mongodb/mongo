// Tests that change stream returns a special entry and close the cursor when it's migrating
// a chunk to a new shard.
// TODO: SERVER-30834 the mongos should internally swallow and automatically retry the 'retryNeeded'
// entries, so the client shouldn't see any invalidations.
(function() {
    'use strict';

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const rsNodeOptions = {
        enableMajorityReadConcern: '',
        // Use a higher frequency for periodic noops to speed up the test.
        setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
    };
    const st =
        new ShardingTest({shards: 2, mongos: 1, rs: {nodes: 1}, other: {rsOptions: rsNodeOptions}});

    const mongos = st.s;
    const mongosColl = mongos.getCollection('test.foo');
    const mongosDB = mongos.getDB("test");

    // Enable sharding to inform mongos of the database, allowing us to open a cursor.
    assert.commandWorked(mongos.adminCommand({enableSharding: mongosDB.getName()}));

    // Make sure all chunks start on shard 0.
    st.ensurePrimaryShard(mongosDB.getName(), st.shard0.shardName);

    // Open a change stream cursor before the collection is sharded.
    const changeStream = mongosColl.aggregate([{$changeStream: {}}]);
    assert(!changeStream.hasNext(), "Do not expect any results yet");

    // Once we have a cursor, actually shard the collection.
    assert.commandWorked(
        mongos.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Insert two documents.
    assert.writeOK(mongosColl.insert({_id: 0}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 20}, {writeConcern: {w: "majority"}}));

    // Split the collection into two chunks: [MinKey, 10) and [10, MaxKey].
    assert.commandWorked(mongos.adminCommand({split: mongosColl.getFullName(), middle: {_id: 10}}));

    // Migrate the [10, MaxKey] chunk to shard1.
    assert.commandWorked(mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: 20},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    for (let id of[0, 20]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.documentKey, {_id: id});
    }
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "retryNeeded");
    const retryResumeToken = next._id;

    // A change stream only gets closed on the first chunk migration to a new shard. Test that
    // another chunk split and migration does not invalidate the cursor.
    const resumedCursor = mongosColl.aggregate([{$changeStream: {resumeAfter: retryResumeToken}}]);

    // Insert into both the chunks.
    assert.writeOK(mongosColl.insert({_id: 1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 21}, {writeConcern: {w: "majority"}}));

    // Split again, and move a second chunk to the first shard. The new chunks are:
    // [MinKey, 0), [0, 10), and [10, MaxKey].
    assert.commandWorked(mongos.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: 5},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    // Insert again, into all three chunks.
    assert.writeOK(mongosColl.insert({_id: -2}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 2}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 22}, {writeConcern: {w: "majority"}}));

    // Make sure we can see all the inserts, without any 'retryNeeded' entries.
    for (let nextExpectedId of[1, 21, -2, 2, 22]) {
        assert.soon(() => resumedCursor.hasNext());
        assert.eq(resumedCursor.next().documentKey, {_id: nextExpectedId});
    }

    // Verify the original cursor has been closed since the first migration, and that it can't see
    // any new inserts.
    assert(!changeStream.hasNext());

    // Test that migrating the last chunk to shard 1 (meaning all chunks are now on the same shard)
    // will not invalidate the change stream.

    // Insert into all three chunks.
    assert.writeOK(mongosColl.insert({_id: -3}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 3}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 23}, {writeConcern: {w: "majority"}}));

    // Move the last chunk, [MinKey, 0), to shard 1.
    assert.commandWorked(mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: -5},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    // Insert again, into all three chunks.
    assert.writeOK(mongosColl.insert({_id: -4}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 4}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 24}, {writeConcern: {w: "majority"}}));

    // Make sure we can see all the inserts, without any 'retryNeeded' entries.
    assert.soon(() => resumedCursor.hasNext());
    for (let nextExpectedId of[-3, 3, 23, -4, 4, 24]) {
        assert.soon(() => resumedCursor.hasNext());
        assert.eq(resumedCursor.next().documentKey, {_id: nextExpectedId});
    }

    // Now test that adding a new shard and migrating a chunk to it will again invalidate the
    // cursor.
    const newShard = new ReplSetTest({name: "newShard", nodes: 1, nodeOptions: rsNodeOptions});
    newShard.startSet({shardsvr: ''});
    newShard.initiate();
    assert.commandWorked(mongos.adminCommand({addShard: newShard.getURL(), name: "newShard"}));

    // At this point, there haven't been any migrations to that shard, so we should still be able to
    // use the change stream.
    assert.writeOK(mongosColl.insert({_id: -5}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 5}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 25}, {writeConcern: {w: "majority"}}));

    for (let nextExpectedId of[-5, 5, 25]) {
        assert.soon(() => resumedCursor.hasNext());
        assert.eq(resumedCursor.next().documentKey, {_id: nextExpectedId});
    }

    // Now migrate a chunk to the new shard and verify the stream is closed.
    assert.commandWorked(mongos.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {_id: 20},
        to: "newShard",
        _waitForDelete: true
    }));
    assert.writeOK(mongosColl.insert({_id: -6}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 6}, {writeConcern: {w: "majority"}}));
    assert.writeOK(mongosColl.insert({_id: 26}, {writeConcern: {w: "majority"}}));

    // We again need to wait for the noop writer on shard 0 to ensure we can return the new results
    // (in this case the 'retryNeeded' entry) from shard 1.
    assert.soon(() => resumedCursor.hasNext());
    assert.eq(resumedCursor.next().operationType, "retryNeeded");
    assert(!resumedCursor.hasNext());

    st.stop();
    newShard.stopSet();
})();
