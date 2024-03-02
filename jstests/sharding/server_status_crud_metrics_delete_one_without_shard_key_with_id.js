/**
 * Tests for the 'metrics.query' section of the mongoS serverStatus response dealing with delete
 * operations for deletes with _id specified but not the shard key.
 */

const st = new ShardingTest(
    {shards: 2, mongosOptions: {setParameter: {featureFlagUpdateOneWithIdWithoutShardKey: true}}});
const testDB = st.s.getDB("test");
const testColl = testDB.coll;

assert.commandWorked(
    st.s0.adminCommand({enableSharding: testDB.getName(), primaryShard: st.shard0.shardName}));

// Shard testColl on {x:1}, split it at {x:0}, and move chunk {x:1} to shard1.
st.shardColl(testColl, {x: 1}, {x: 0}, {x: 1});

// Insert one document on each shard.
assert.commandWorked(testColl.insert({x: 1, _id: 1}));
assert.commandWorked(testColl.insert({x: -1, _id: 0}));

assert.commandWorked(testColl.deleteOne({_id: 1}));
assert.commandWorked(testColl.deleteOne({_id: 0}));

let mongosServerStatus = testDB.adminCommand({serverStatus: 1});

// Verify that the above delete incremented the metric counter.
assert.eq(2, mongosServerStatus.metrics.query.deleteOneWithoutShardKeyWithIdCount);

st.stop();
