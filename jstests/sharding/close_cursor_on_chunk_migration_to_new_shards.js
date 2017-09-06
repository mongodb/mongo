// Tests that change stream returns a special entry and close the cursor when it's migrating
// a chunk to a new shard.
(function() {
    'use strict';

    // For supportsMajorityReadConcern().
    load("jstests/multiVersion/libs/causal_consistency_helpers.js");

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        mongos: 1,
        rs: {nodes: 1},
        other: {rsOptions: {enableMajorityReadConcern: ""}}
    });

    const mongos = st.s;
    const admin = mongos.getDB('admin');
    const coll = mongos.getCollection('foo.bar');
    const dbOnShard = st.rs0.getPrimary().getDB('foo');

    // Shard collection.
    assert.commandWorked(mongos.adminCommand({enableSharding: coll.getDB().getName()}));

    // Just to be sure what primary we start from.
    st.ensurePrimaryShard(coll.getDB().getName(), st.shard0.shardName);
    assert.commandWorked(mongos.adminCommand({shardCollection: coll.getFullName(), key: {_id: 1}}));

    st.rs0.awaitReplication();
    let res = assert.commandWorked(dbOnShard.runCommand(
        {aggregate: coll.getName(), pipeline: [{$changeStream: {}}], cursor: {}}));
    assert.neq(res.cursor.id, 0);
    assert.eq(res.cursor.firstBatch.length, 0);

    // Insert two documents.
    assert.writeOK(coll.insert({_id: 0}, {writeConcern: {w: "majority"}}));
    assert.writeOK(coll.insert({_id: 20}, {writeConcern: {w: "majority"}}));
    mongos.adminCommand({split: coll.getFullName(), middle: {_id: 10}});

    // Migrate the first chunk to shard1.
    assert.commandWorked(mongos.adminCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 0},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    res = assert.commandWorked(
        dbOnShard.runCommand({getMore: res.cursor.id, collection: coll.getName()}));
    assert.eq(res.cursor.nextBatch.length, 3);
    assert.eq(res.cursor.nextBatch[0].operationType, "insert");
    assert.eq(res.cursor.nextBatch[1].operationType, "insert");
    assert.eq(res.cursor.nextBatch[2].operationType, "retryNeeded");
    const resumeToken = res.cursor.nextBatch[2]._id;
    // Verify the cursor has been closed since the chunk migrated is the first chunk on shard1.
    assert.eq(res.cursor.id, 0);

    // Change stream only gets closed on the first chunk migration to a new shard.
    // Verify the second chunk migration doesn't close cursors.
    assert.writeOK(coll.insert({_id: 30}, {writeConcern: {w: "majority"}}));
    // Migrate the second chunk to shard1.
    assert.commandWorked(mongos.adminCommand({
        moveChunk: coll.getFullName(),
        find: {_id: 20},
        to: st.shard1.shardName,
        _waitForDelete: true
    }));

    res = assert.commandWorked(dbOnShard.runCommand({
        aggregate: coll.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeToken}}],
        cursor: {}
    }));
    assert.eq(res.cursor.firstBatch.length, 1);
    assert.eq(res.cursor.firstBatch[0].operationType, "insert");
    // Verify the cursor is not closed.
    assert.neq(res.cursor.id, 0);

    st.stop();
})();
