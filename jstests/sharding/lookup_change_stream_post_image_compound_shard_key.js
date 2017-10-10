// Tests the behavior of looking up the post image for change streams on collections which are
// sharded with a compound shard key.
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

    const mongosDB = st.s0.getDB(jsTestName());
    const mongosColl = mongosDB['coll'];

    assert.commandWorked(mongosDB.dropDatabase());

    // Enable sharding on the test DB and ensure its primary is shard0000.
    assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
    st.ensurePrimaryShard(mongosDB.getName(), st.rs0.getURL());

    // Shard the test collection with a compound shard key: a, b, c. Then split it into two chunks,
    // and put one chunk on each shard.
    assert.commandWorked(mongosDB.adminCommand(
        {shardCollection: mongosColl.getFullName(), key: {a: 1, b: 1, c: 1}}));

    // Split the collection into 2 chunks:
    // [{a: MinKey, b: MinKey, c: MinKey}, {a: 1,      b: MinKey, c: MinKey})
    // and
    // [{a: 1,      b: MinKey, c: MinKey}, {a: MaxKey, b: MaxKey, c: MaxKey}).
    assert.commandWorked(mongosDB.adminCommand(
        {split: mongosColl.getFullName(), middle: {a: 1, b: MinKey, c: MinKey}}));

    // Move the upper chunk to shard 1.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {a: 1, b: MinKey, c: MinKey},
        to: st.rs1.getURL()
    }));

    const changeStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);

    const nDocs = 6;
    const bValues = ["one", "two", "three", "four", "five", "six"];

    // This shard key function results in 1/3rd of documents on shard0 and 2/3rds on shard1.
    function shardKeyFromId(id) {
        return {a: id % 3, b: bValues[id], c: id % 2};
    }

    // Do some writes.
    for (let id = 0; id < nDocs; ++id) {
        const documentKey = Object.merge({_id: id}, shardKeyFromId(id));
        assert.writeOK(mongosColl.insert(documentKey));
        assert.writeOK(mongosColl.update(documentKey, {$set: {updatedCount: 1}}));
    }

    for (let id = 0; id < nDocs; ++id) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "insert");
        assert.eq(next.documentKey, Object.merge(shardKeyFromId(id), {_id: id}));

        assert.soon(() => changeStream.hasNext());
        next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, Object.merge(shardKeyFromId(id), {_id: id}));
        assert.docEq(next.fullDocument,
                     Object.merge(shardKeyFromId(id), {_id: id, updatedCount: 1}));
    }

    // Test that the change stream can still see the updated post image, even if a chunk is
    // migrated.
    for (let id = 0; id < nDocs; ++id) {
        const documentKey = Object.merge({_id: id}, shardKeyFromId(id));
        assert.writeOK(mongosColl.update(documentKey, {$set: {updatedCount: 2}}));
    }

    // Move the upper chunk back to shard 0.
    assert.commandWorked(mongosDB.adminCommand({
        moveChunk: mongosColl.getFullName(),
        find: {a: 1, b: MinKey, c: MinKey},
        to: st.rs0.getURL()
    }));

    for (let id = 0; id < nDocs; ++id) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, Object.merge(shardKeyFromId(id), {_id: id}));
        assert.docEq(next.fullDocument,
                     Object.merge(shardKeyFromId(id), {_id: id, updatedCount: 2}));
    }

    st.stop();
})();
