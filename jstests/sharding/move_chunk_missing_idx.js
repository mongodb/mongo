/**
 * Test that migration should fail if the destination shard does not
 * have the index and is not empty.
 */

// This test runs dropIndex on one of the shards.
TestData.skipCheckingIndexesConsistentAcrossCluster = true;

var st = new ShardingTest({shards: 2});

var testDB = st.s.getDB('test');

testDB.adminCommand({enableSharding: 'test'});
st.ensurePrimaryShard(testDB.toString(), st.shard1.shardName);
testDB.adminCommand({shardCollection: 'test.user', key: {x: 1}});

// Test procedure:
// 1. Create index (index should now be in primary shard).
// 2. Split chunk into 3 parts.
// 3. Move 1 chunk to 2nd shard - should have no issues

testDB.user.createIndex({a: 1, b: 1});

testDB.adminCommand({split: 'test.user', middle: {x: 0}});
testDB.adminCommand({split: 'test.user', middle: {x: 10}});

// Collection does not exist, no chunk, index missing case at destination case.
assert.commandWorked(
    testDB.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard0.shardName}));

// Drop index since last moveChunk created this.
st.rs0.getPrimary().getDB('test').user.dropIndex({a: 1, b: 1});

// Collection exist but empty, index missing at destination case.
assert.commandWorked(
    testDB.adminCommand({moveChunk: 'test.user', find: {x: 10}, to: st.shard0.shardName}));

// Drop index since last moveChunk created this.
st.rs0.getPrimary().getDB('test').user.dropIndex({a: 1, b: 1});

// Collection not empty, index missing at destination case.
testDB.user.insert({x: 10});
assert.commandFailed(
    testDB.adminCommand({moveChunk: 'test.user', find: {x: -10}, to: st.shard0.shardName}));

st.stop();
