// Tests metadata notifications of change streams on sharded collections.
// Legacy getMore fails after dropping the database that the original cursor is on.
// @tags: [requires_find_command]
(function() {
    "use strict";

    load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.
    load('jstests/replsets/libs/two_phase_drops.js');  // For TwoPhaseDropCollectionTest.

    // For supportsMajorityReadConcern.
    load('jstests/multiVersion/libs/causal_consistency_helpers.js');

    if (!supportsMajorityReadConcern()) {
        jsTestLog("Skipping test since storage engine doesn't support majority read concern.");
        return;
    }

    const st = new ShardingTest({
        shards: 2,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
        }
    });

    const merizosDB = st.s0.getDB(jsTestName());
    const merizosColl = merizosDB[jsTestName()];

    assert.commandWorked(merizosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is st.shard0.shardName.
    assert.commandWorked(merizosDB.adminCommand({enableSharding: merizosDB.getName()}));
    st.ensurePrimaryShard(merizosDB.getName(), st.rs0.getURL());

    // Shard the test collection on a field called 'shardKey'.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {shardKey: 1}}));

    // Split the collection into 2 chunks: [MinKey, 0), [0, MaxKey].
    assert.commandWorked(
        merizosDB.adminCommand({split: merizosColl.getFullName(), middle: {shardKey: 0}}));

    // Move the [0, MaxKey] chunk to st.shard1.shardName.
    assert.commandWorked(merizosDB.adminCommand(
        {moveChunk: merizosColl.getFullName(), find: {shardKey: 1}, to: st.rs1.getURL()}));

    // Write a document to each chunk.
    assert.writeOK(merizosColl.insert({shardKey: -1, _id: -1}, {writeConcern: {w: "majority"}}));
    assert.writeOK(merizosColl.insert({shardKey: 1, _id: 1}, {writeConcern: {w: "majority"}}));

    let changeStream = merizosColl.watch();

    // We awaited the replication of the first writes, so the change stream shouldn't return them.
    assert.writeOK(merizosColl.update({shardKey: -1, _id: -1}, {$set: {updated: true}}));
    assert.writeOK(merizosColl.update({shardKey: 1, _id: 1}, {$set: {updated: true}}));
    assert.writeOK(merizosColl.insert({shardKey: 2, _id: 2}));

    // Drop the collection and test that we return a "drop" entry, followed by an "invalidate"
    // entry.
    merizosColl.drop();

    // Test that we see the two writes that happened before the collection drop.
    assert.soon(() => changeStream.hasNext());
    let next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey.shardKey, -1);
    const resumeTokenFromFirstUpdate = next._id;

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey.shardKey, 1);

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey, {_id: 2});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: merizosDB.getName(), coll: merizosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // With an explicit collation, test that we can resume from before the collection drop.
    changeStream = merizosColl.watch(
        [], {resumeAfter: resumeTokenFromFirstUpdate, collation: {locale: "simple"}});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "update");
    assert.eq(next.documentKey, {shardKey: 1, _id: 1});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "insert");
    assert.eq(next.documentKey, {shardKey: 2, _id: 2});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: merizosDB.getName(), coll: merizosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    // Test that we can resume the change stream without specifying an explicit collation.
    assert.commandWorked(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        cursor: {}
    }));

    // Recreate and shard the collection.
    assert.commandWorked(merizosDB.createCollection(merizosColl.getName()));

    // Shard the test collection on shardKey.
    assert.commandWorked(
        merizosDB.adminCommand({shardCollection: merizosColl.getFullName(), key: {shardKey: 1}}));

    // Test that resuming the change stream on the recreated collection succeeds, since we will not
    // attempt to inherit the collection's default collation and can therefore ignore the new UUID.
    assert.commandWorked(merizosDB.runCommand({
        aggregate: merizosColl.getName(),
        pipeline: [{$changeStream: {resumeAfter: resumeTokenFromFirstUpdate}}],
        cursor: {}
    }));

    // Recreate the collection as unsharded and open a change stream on it.
    assertDropAndRecreateCollection(merizosDB, merizosColl.getName());

    changeStream = merizosColl.watch();

    // Drop the database and verify that the stream returns a collection drop followed by an
    // invalidate.
    assert.commandWorked(merizosDB.dropDatabase());

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "drop");
    assert.eq(next.ns, {db: merizosDB.getName(), coll: merizosColl.getName()});

    assert.soon(() => changeStream.hasNext());
    next = changeStream.next();
    assert.eq(next.operationType, "invalidate");
    assert(changeStream.isExhausted());

    st.stop();
})();
