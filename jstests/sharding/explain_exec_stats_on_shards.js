// Tests for the mongos explain command to ensure that the 'executionStats' section of the explain
// output is populated correctly for each shard.
// @tags: [requires_fcv_44]
(function() {
'use strict';

// Verifies that the explain output for the given shard contains all expected fields and that
// field values match the values specified in the 'expected*' arguments. This function also
// updates the 'totals' object which holds accumulated values for certain fields from each
// shard.
function verifyExecStatsOnShard({
    explain,
    expectedShardName,
    expectedNReturned,
    expectedKeysExamined,
    expectedDocsExamined,
    totals
}) {
    assert(explain.executionSuccess, tojson(explain));
    assert.eq(explain.shardName, expectedShardName, tojson(explain));
    assert.eq(explain.nReturned, expectedNReturned, tojson(explain));
    assert.gte(explain.executionTimeMillis, 0, tojson(explain));
    assert.eq(explain.totalKeysExamined, expectedKeysExamined, tojson(explain));
    assert.eq(explain.totalDocsExamined, expectedDocsExamined, tojson(explain));
    assert(explain.hasOwnProperty("executionStages"), tojson(explain));

    totals.nReturned += explain.nReturned;
    totals.executionTimeMillis += explain.executionTimeMillis;
    totals.keysExamined += explain.totalKeysExamined;
    totals.docsExamined += explain.totalDocsExamined;
}

// Create a cluster with 2 shards.
const numShards = 2;
const st = new ShardingTest({shards: numShards});
const db = st.s.getDB(`${jsTest.name()}_db`);

// Enable sharding on the database and use shard0 as the primary shard.
assert.commandWorked(db.adminCommand({enableSharding: db.getName()}));
st.ensurePrimaryShard(db.getName(), st.shard0.shardName);

// Test that the explain's 'executionStats' section includes all relevant fields for each shard
// when the 'explain' command is executed against a sharded collection.
(function testExplainExecutionStatsWithShardedCollection() {
    // Set up a collection, shard on {a:1}, split at {a:4}, and move the {a:4} chunk to shard1.
    const shardedColl = db.getCollection(`${jsTest.name()}_sharded`);
    shardedColl.drop();
    st.shardColl(shardedColl, {a: 1}, {a: 4}, {a: 4});

    // Put documents on each shard.
    const numDocs = 10;
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(shardedColl.insert({_id: i, a: i}));
    }
    assert.eq(shardedColl.find().itcount(), numDocs);

    // Explain the find command and check that all expected explain fields are present in the
    // output.
    const explain = shardedColl.find({}).explain("executionStats");
    assert(explain.hasOwnProperty("executionStats"), tojson(explain));
    assert(explain.executionStats.hasOwnProperty("executionStages"), tojson(explain));
    assert(explain.executionStats.executionStages.hasOwnProperty("shards"), tojson(explain));
    assert.eq(explain.executionStats.executionStages.shards.length, numShards);

    // Verify execution stats on each shards and accumulate totals.
    const totals = {nReturned: 0, executionTimeMillis: 0, keysExamined: 0, docsExamined: 0};
    const executionStages = explain.executionStats.executionStages;
    // Sort the shards array by the 'shardName' to guarantee consistent results, as explain
    // outputs from the shards may arrive in an arbitrary order.
    executionStages.shards.sort((a, b) => a.shardName.localeCompare(b.shardName));
    verifyExecStatsOnShard({
        explain: executionStages.shards[0],
        expectedShardName: st.shard0.shardName,
        expectedNReturned: 4,
        expectedKeysExamined: 0,
        expectedDocsExamined: 4,
        totals: totals
    });
    verifyExecStatsOnShard({
        explain: executionStages.shards[1],
        expectedShardName: st.shard1.shardName,
        expectedNReturned: 6,
        expectedKeysExamined: 0,
        expectedDocsExamined: 6,
        totals: totals
    });

    // Ensure that execution stats accumulated across all shards matches the values in the
    // top-level 'executionStages' section of the explain output.
    assert.eq(executionStages.nReturned, totals.nReturned, tojson(explain));
    assert.eq(executionStages.totalChildMillis, totals.executionTimeMillis, tojson(explain));
    assert.eq(executionStages.totalKeysExamined, totals.keysExamined, tojson(explain));
    assert.eq(executionStages.totalDocsExamined, totals.docsExamined, tojson(explain));
})();

// Test that the explain's 'executionStats' section includes all relevant fields when the
// 'explain' command is executed against an unsharded collection.
(function testExplainExecutionStatsWithUnshardedCollection() {
    // Setup an unsharded collection.
    const unshardedColl = db.getCollection(`${jsTest.name()}_unsharded`);
    unshardedColl.drop();
    assert.commandWorked(unshardedColl.ensureIndex({a: 1}));

    // Add documents to the collection.
    const numDocs = 10;
    for (let i = 0; i < numDocs; i++) {
        assert.commandWorked(unshardedColl.insert({_id: i, a: i}));
    }
    assert.eq(unshardedColl.count(), numDocs);

    // Explain the find command and check that all expected explain fields are present in the
    // output.
    const explain = unshardedColl.find({}).explain("executionStats");
    assert(explain.hasOwnProperty("executionStats"), tojson(explain));
    assert(explain.executionStats.hasOwnProperty("executionStages"), tojson(explain));
    assert(explain.executionStats.executionStages.hasOwnProperty("shards"), tojson(explain));
    assert.eq(explain.executionStats.executionStages.shards.length, 1);

    // Verify execution stats on the primary shard and accumulate totals.
    const totals = {nReturned: 0, executionTimeMillis: 0, keysExamined: 0, docsExamined: 0};
    const executionStages = explain.executionStats.executionStages;
    verifyExecStatsOnShard({
        explain: executionStages.shards[0],
        expectedShardName: st.shard0.shardName,
        expectedNReturned: 10,
        expectedKeysExamined: 0,
        expectedDocsExamined: 10,
        totals: totals
    });

    // Ensure that execution stats on the primary shard matches the values in the top-level
    // 'executionStages' section of the explain output.
    assert.eq(executionStages.nReturned, totals.nReturned, tojson(explain));
    assert.eq(executionStages.totalChildMillis, totals.executionTimeMillis, tojson(explain));
    assert.eq(executionStages.totalKeysExamined, totals.keysExamined, tojson(explain));
    assert.eq(executionStages.totalDocsExamined, totals.docsExamined, tojson(explain));
})();

st.stop();
})();
