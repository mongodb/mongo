// Tests the behavior of removing a shard while a change stream is open.
(function() {
    "use strict";

    load('jstests/aggregation/extras/utils.js');  // For assertErrorCode().
    load('jstests/libs/change_stream_util.js');   // For ChangeStreamTest.

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    // This test only works on storage engines that support committed reads, skip it if the
    // configured engine doesn't support it.
    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    // Use a ShardingTest with 3 shards to ensure there can still be at least 2 left after removing
    // one. This will ensure the change stream is still merging on mongos after the removal, and
    // cannot forward the entire pipeline to the shards.
    const st = new ShardingTest({
        shards: 3,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Use the noop writer with a higher frequency for periodic noops to speed up the test.
            setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB[jsTestName()];

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 3 chunks: [MinKey, 0), [0, 1000), and [1000, MaxKey].
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 0}}));
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {_id: 1000}}));

    // Move the [0, 1000) chunk to shard 1.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 0}, to: st.rs1.getURL()}));

    // Move the [1000, MaxKey] chunk to shard 2.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {_id: 1000}, to: st.rs2.getURL()}));

    // Use a small batch size to enable us to spread the iteration of the shard's cursor over
    // multiple getMores.
    const batchSize = 2;
    let changeStream = mongosColl.aggregate([{$changeStream: {}}], {batchSize: batchSize});

    // Write some documents for the change stream to consume. Be sure to write enough to each shard
    // that we can't consume them all in one batch.
    for (let i = 0; i < 2 * batchSize; ++i) {
        assert.writeOK(mongosColl.insert({_id: -1 - i}, {writeConcern: {w: "majority"}}));
        assert.writeOK(mongosColl.insert({_id: i}, {writeConcern: {w: "majority"}}));
        assert.writeOK(mongosColl.insert({_id: 1000 + i}, {writeConcern: {w: "majority"}}));
    }

    // Remove shard 2.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        to: st.rs0.getURL(),
        find: {_id: 1000},
        _waitForDelete: true
    }));
    let removeStatus;
    assert.soon(function() {
        removeStatus = assert.commandWorked(mongosDB.adminCommand({removeShard: st.rs2.getURL()}));
        return removeStatus.state === "completed";
    }, () => `Shard removal timed out, most recent removeShard response: ${tojson(removeStatus)}`);

    // The shard removal will not invalidate any cursors, so we still expect to be able to see
    // changes as long as the shard is still running.
    let resumeTokenFromShard2;
    for (let nextChangeId of[-1, 0, 1000]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.documentKey._id, nextChangeId);
        if (nextChangeId === 1000) {
            resumeTokenFromShard2 = next._id;
        }
    }

    // Now actually stop the removed shard, eventually the change stream should get an error.
    st.rs2.stopSet();
    assert.soon(function() {
        try {
            // We should encounter an error before running out of changes.
            assert.soon(() => changeStream.hasNext());
            assert.eq(changeStream.next().operationType, "insert");
        } catch (error) {
            return true;
        }
        return false;
    }, "Expected change stream to error due to missing host");

    // Test that it is not possible to resume a change stream with a resume token from the removed
    // shard, which will not exist anymore.

    ChangeStreamTest.assertChangeStreamThrowsCode({
        collection: mongosColl,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromShard2}}],
        expectedCode: 40585
    });

    // Now do the same test, only this time there will be only one shard remaining after removing
    // shard 1.

    changeStream = mongosColl.aggregate([{$changeStream: {}}], {batchSize: batchSize});

    // Insert more than one batch of changes on each (remaining) shard.
    const nDocsInEachChunkAfterFirstRemoval = 2 * batchSize;
    for (let i = 0; i < 2 * batchSize; ++i) {
        assert.writeOK(mongosColl.insert({_id: -1 - nDocsInEachChunkAfterFirstRemoval - i}));
        assert.writeOK(mongosColl.insert({_id: nDocsInEachChunkAfterFirstRemoval + i}));
    }

    // Remove shard 1.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        to: st.rs0.getURL(),
        find: {_id: 0},
        _waitForDelete: true
    }));
    assert.soon(function() {
        removeStatus = assert.commandWorked(mongosDB.adminCommand({removeShard: st.rs1.getURL()}));
        return removeStatus.state === "completed";
    }, () => `Shard removal timed out, most recent removeShard response: ${tojson(removeStatus)}`);

    // The shard removal will not invalidate any cursors, so we still expect to be able to see
    // changes as long as the shard is still running.
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey._id, -1 - nDocsInEachChunkAfterFirstRemoval);

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey._id, nDocsInEachChunkAfterFirstRemoval);
    const resumeTokenFromShard1 = next._id;

    // Stop the removed shard, eventually the change stream should get an error.
    st.rs1.stopSet();
    assert.soon(function() {
        try {
            // We should encounter an error before running out of changes.
            assert.soon(() => changeStream.hasNext());
            assert.eq(changeStream.next().operationType, "insert");
        } catch (error) {
            return true;
        }
        return false;
    }, "Expected change stream to error due to missing host");

    ChangeStreamTest.assertChangeStreamThrowsCode({
        collection: mongosColl,
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromShard1}}],
        expectedCode: 40585
    });

    st.stop();
})();
