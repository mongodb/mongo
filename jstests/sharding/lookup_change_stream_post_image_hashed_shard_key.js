// Tests the behavior of looking up the post image for change streams on collections which are
// sharded with a hashed shard key.
// @tags: [requires_majority_read_concern]
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
        enableBalancer: false,
        rs: {
            nodes: 1,
            enableMajorityReadConcern: '',
            // Use a higher frequency for periodic noops to speed up the test.
            setParameter: {writePeriodicNoops: true, periodicNoopIntervalSecs: 1}
        }
    });

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB['coll'];

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection on the field "shardKey", and split it into two chunks.
    assert.commandWorked(mongosDB.adminCommand({
        shardCollection: mongosColl.getFullName(),
        numInitialChunks: 2,
        key: {shardKey: "hashed"}
    }));

    // Make sure the negative chunk is on shard 0.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        bounds: [{shardKey: MinKey}, {shardKey: NumberLong("0")}],
        to: st.rs0.getURL()
    }));

    // Make sure the positive chunk is on shard 1.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        bounds: [{shardKey: NumberLong("0")}, {shardKey: MaxKey}],
        to: st.rs1.getURL()
    }));

    const changeStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);

    // Write enough documents that we likely have some on each shard.
    const nDocs = 1000;
    for (let id = 0; id < nDocs; ++id) {
        assert.writeOK(mongosColl.insert({_id: id, shardKey: id}));
        assert.writeOK(mongosColl.update({shardKey: id}, {$set: {updatedCount: 1}}));
    }

    for (let id = 0; id < nDocs; ++id) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.documentKey, {shardKey: id, _id: id});

        assert.soon(() => changeStream.hasNext());
        next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, {shardKey: id, _id: id});
        assert.docEq(next.fullDocument, {_id: id, shardKey: id, updatedCount: 1});
    }

    st.stop();
})();
