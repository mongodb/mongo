// Tests the behavior of looking up the post image for change streams on collections which are
// sharded with a key which is just the "_id" field.
// @tags: [uses_change_streams]
(function() {
    "use strict";

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
            enableMajorityReadConcern: '',
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
        }
    });

    const merizosDB = st.s0.getDB(jsTestName());
    const merizosColl = merizosDB['coll'];

    assert.commandWorked(merizosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    // Shard the test collection on _id.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {_id: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey).
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: 0}}));

    // Move the [0, MaxKey) chunk to st.shard1.shardName.
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {_id: 1}, to: st.rs1.getURL()}));

    // Write a document to each chunk.
    assert.writeOK(merizosColl.insert({_id: -1}));
    assert.writeOK(merizosColl.insert({_id: 1}));

    const changeStream = merizosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);

    // Do some writes.
    assert.writeOK(merizosColl.insert({_id: 1000}));
    assert.writeOK(merizosColl.insert({_id: -1000}));
    assert.writeOK(merizosColl.update({_id: 1000}, {$set: {updatedCount: 1}}));
    assert.writeOK(merizosColl.update({_id: -1000}, {$set: {updatedCount: 1}}));

    for (let nextId of[1000, -1000]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.documentKey, {_id: nextId});
    }

    for (let nextId of[1000, -1000]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "update");
        // Only the "_id" field is present in next.documentKey because the shard key is the _id.
        assert.eq(next.documentKey, {_id: nextId});
        assert.docEq(next.fullDocument, {_id: nextId, updatedCount: 1});
    }

    // Test that the change stream can still see the updated post image, even if a chunk is
    // migrated.
    assert.writeOK(merizosColl.update({_id: 1000}, {$set: {updatedCount: 2}}));
    assert.writeOK(merizosColl.update({_id: -1000}, {$set: {updatedCount: 2}}));

    // Split the [0, MaxKey) chunk into 2: [0, 500), [500, MaxKey).
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {_id: 500}}));
    // Move the [500, MaxKey) chunk back to st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {_id: 1000}, to: st.rs0.getURL()}));

    for (let nextId of[1000, -1000]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, {_id: nextId});
        assert.docEq(next.fullDocument, {_id: nextId, updatedCount: 2});
    }

    st.stop();
})();
