// Tests that the post image update lookup will use the simple collation to do shard targeting, but
// use the collection's default collation once it gets to the shards.
//
// Collation is only supported with the find command, not with op query.
// @tags: [requires_find_command, requires_majority_read_concern]
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
        config: 1,
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

    const caseInsensitive = {locale: "en_US", strength: 2};
    assert.commandWorked(
        mongosDB.runCommand({create: mongosColl.getName(), collation: caseInsensitive}));

    // Shard the test collection on 'shardKey'. The shard key must use the simple collation.
    assert.commandWorked(mongosDB.adminCommand({
        shardCollection: mongosColl.getFullName(),
        key: {shardKey: 1},
        collation: {locale: "simple"}
    }));

    // Split the collection into 2 chunks: [MinKey, "aBC"), ["aBC", MaxKey). Note that there will be
    // documents in each chunk that will have the same shard key according to the collection's
    // default collation, but not according to the simple collation (e.g. "abc" and "ABC").
    assert.commandWorked(
        mongosDB.adminCommand({split: mongosColl.getFullName(), middle: {shardKey: "aBC"}}));

    // Move the [MinKey, 'aBC') chunk to shard0001.
    assert.commandWorked(mongosDB.adminCommand(
        {moveChunk: mongosColl.getFullName(), find: {shardKey: "ABC"}, to: st.rs1.getURL()}));

    // Make sure that "ABC" and "abc" go to different shards - we rely on that to make sure the _ids
    // are unique on each shard.
    assert.lte(bsonWoCompare({shardKey: "ABC"}, {shardKey: "aBC"}), -1);
    assert.gte(bsonWoCompare({shardKey: "abc"}, {shardKey: "aBC"}), 1);

    // Write some documents to each chunk. Note that the _id is purposefully not unique, since we
    // know the update lookup will use both the _id and the shard key, and we want to make sure it
    // is only targeting a single shard. Also note that _id is a string, since we want to make sure
    // the _id index can only be used if we are using the collection's default collation.
    assert.writeOK(mongosColl.insert({_id: "abc_1", shardKey: "ABC"}));
    assert.writeOK(mongosColl.insert({_id: "abc_2", shardKey: "ABC"}));
    assert.writeOK(mongosColl.insert({_id: "abc_1", shardKey: "abc"}));
    assert.writeOK(mongosColl.insert({_id: "abc_2", shardKey: "abc"}));

    // Verify that the post-change lookup uses the simple collation to target to a single shard,
    // then uses the collection-default collation to perform the lookup on the shard.
    const changeStream = mongosColl.aggregate([{$changeStream: {fullDocument: "updateLookup"}}]);

    // Be sure to include the collation in the updates so that each can be targeted to exactly one
    // shard - this is important to ensure each update only updates one document (since with the
    // default collation their documentKeys are identical). If each operation updates only one, the
    // clusterTime sent from mongos will ensure that each corresponding oplog entry has a distinct
    // timestamp and so will appear in the change stream in the order we expect.
    let updateResult = mongosColl.updateOne({shardKey: "abc", _id: "abc_1"},
                                            {$set: {updatedCount: 1}},
                                            {collation: {locale: "simple"}});
    assert.eq(1, updateResult.modifiedCount);
    updateResult = mongosColl.updateOne({shardKey: "ABC", _id: "abc_1"},
                                        {$set: {updatedCount: 1}},
                                        {collation: {locale: "simple"}});
    assert.eq(1, updateResult.modifiedCount);

    function numIdIndexUsages(host) {
        return host.getCollection(mongosColl.getFullName())
            .aggregate([{$indexStats: {}}, {$match: {name: "_id_"}}])
            .toArray()[0]
            .accesses.ops;
    }
    let idIndexUsagesPreIteration = {
        shard0: numIdIndexUsages(st.rs0.getPrimary()),
        shard1: numIdIndexUsages(st.rs1.getPrimary())
    };

    for (let nextDocKey of[{shardKey: "abc", _id: "abc_1"}, {shardKey: "ABC", _id: "abc_1"}]) {
        assert.soon(() => changeStream.hasNext());
        let next = changeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, nextDocKey, tojson(next));
        assert.docEq(next.fullDocument, Object.merge(nextDocKey, {updatedCount: 1}));
    }
    assert.eq(numIdIndexUsages(st.rs0.getPrimary()), idIndexUsagesPreIteration.shard0 + 1);
    assert.eq(numIdIndexUsages(st.rs1.getPrimary()), idIndexUsagesPreIteration.shard1 + 1);

    changeStream.close();

    // Now test that a change stream with a non-default collation will still use the simple
    // collation to target the update lookup, and the collection-default collation to do the update
    // lookup on the shard.

    // Strength 1 will consider "ç" equal to "c" and "C".
    const strengthOneCollation = {locale: "en_US", strength: 1};

    // Insert some documents that might be confused with existing documents under the change
    // stream's collation, but should not be confused during the update lookup.
    assert.writeOK(mongosColl.insert({_id: "abç_1", shardKey: "ABÇ"}));
    assert.writeOK(mongosColl.insert({_id: "abç_2", shardKey: "ABÇ"}));
    assert.writeOK(mongosColl.insert({_id: "abç_1", shardKey: "abç"}));
    assert.writeOK(mongosColl.insert({_id: "abç_2", shardKey: "abç"}));

    assert.eq(mongosColl.find({shardKey: "abc"}).collation(strengthOneCollation).itcount(), 8);

    const strengthOneChangeStream = mongosColl.aggregate(
        [
          {$changeStream: {fullDocument: "updateLookup"}},
          {$match: {"fullDocument.shardKey": "abc"}}
        ],
        {collation: strengthOneCollation});

    updateResult = mongosColl.updateOne({shardKey: "ABC", _id: "abc_1"},
                                        {$set: {updatedCount: 2}},
                                        {collation: {locale: "simple"}});
    assert.eq(1, updateResult.modifiedCount);
    updateResult = mongosColl.updateOne({shardKey: "abc", _id: "abc_1"},
                                        {$set: {updatedCount: 2}},
                                        {collation: {locale: "simple"}});
    assert.eq(1, updateResult.modifiedCount);

    idIndexUsagesPreIteration = {
        shard0: numIdIndexUsages(st.rs0.getPrimary()),
        shard1: numIdIndexUsages(st.rs1.getPrimary())
    };
    for (let nextDocKey of[{shardKey: "ABC", _id: "abc_1"}, {shardKey: "abc", _id: "abc_1"}]) {
        assert.soon(() => strengthOneChangeStream.hasNext());
        let next = strengthOneChangeStream.next();
        assert.eq(next.operationType, "update");
        assert.eq(next.documentKey, nextDocKey, tojson(next));
        assert.docEq(next.fullDocument, Object.merge(nextDocKey, {updatedCount: 2}));
    }
    assert.eq(numIdIndexUsages(st.rs0.getPrimary()), idIndexUsagesPreIteration.shard0 + 1);
    assert.eq(numIdIndexUsages(st.rs1.getPrimary()), idIndexUsagesPreIteration.shard1 + 1);

    strengthOneChangeStream.close();

    st.stop();
}());
