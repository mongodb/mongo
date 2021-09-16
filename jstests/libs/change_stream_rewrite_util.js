/**
 * Helper functions that are used in change streams rewrite test cases.
 */

load("jstests/libs/collection_drop_recreate.js");  // For assertDropAndRecreateCollection.

// Verifies the number of change streams events returned from a particular shard.
function assertNumChangeStreamDocsReturnedFromShard(stats, shardName, expectedTotalReturned) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    const stages = stats.shards[shardName].stages;
    const lastStage = stages[stages.length - 1];
    assert.eq(lastStage.nReturned, expectedTotalReturned, stages);
}

// Verifies the number of oplog events read by a particular shard.
function assertNumMatchingOplogEventsForShard(stats, shardName, expectedTotalReturned) {
    assert(stats.shards.hasOwnProperty(shardName), stats);
    assert.eq(Object.keys(stats.shards[shardName].stages[0])[0], "$cursor", stats);
    const executionStats = stats.shards[shardName].stages[0].$cursor.executionStats;
    assert.eq(executionStats.nReturned, expectedTotalReturned, executionStats);
}

// Returns a newly created sharded collection sharded by caller provided shard key.
function createShardedCollection(shardingTest, shardKey, dbName, collName, splitAt) {
    const db = shardingTest.s.getDB(dbName);
    assertDropAndRecreateCollection(db, collName);

    const coll = db.getCollection(collName);
    assert.commandWorked(coll.createIndex({[shardKey]: 1}));

    shardingTest.ensurePrimaryShard(dbName, shardingTest.shard0.shardName);

    // Shard the test collection and split it into two chunks: one that contains all {shardKey: <lt
    // splitAt>} documents and one that contains all {shardKey: <gte splitAt>} documents.
    shardingTest.shardColl(
        collName,
        {[shardKey]: 1} /* shard key */,
        {[shardKey]: splitAt} /* split at */,
        {[shardKey]: splitAt} /* move the chunk containing {shardKey: splitAt} to its own shard */,
        dbName,
        true);
    return coll;
}