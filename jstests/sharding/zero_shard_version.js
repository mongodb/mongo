/**
 * Tests the setShardVersion logic on the this shard side, specifically when comparing
 * against a major version of zero or incompatible epochs.
 */
(function() {
'use strict';

var st = new ShardingTest({shards: 2, mongos: 2});

var testDB_s0 = st.s.getDB('test');
assert.commandWorked(testDB_s0.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard1.shardName);
assert.commandWorked(testDB_s0.adminCommand({shardCollection: 'test.user', key: {x: 1}}));

var checkShardMajorVersion = function(conn, expectedMajorVersion) {
    const shardVersion =
        assert.commandWorked(conn.adminCommand({getShardVersion: 'test.user'})).global;
    assert.eq(shardVersion.getTime(),
              expectedMajorVersion,
              "Node " + conn + " expected to have major version " + expectedMajorVersion +
                  " but has version " + tojson(shardVersion));
};

// Routing information:
//   - mongos0: 1|0|a
//   - mongos1: UNKNOWN

// Shard information:
//   - shard0: UNKNOWN
//   - shard1: 1|0|a, [-inf, inf)

///////////////////////////////////////////////////////
// Test shard with empty chunk

var testDB_s1 = st.s1.getDB('test');
assert.commandWorked(testDB_s1.user.insert({x: 1}));
assert.commandWorked(
    testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard0.shardName}));

st.configRS.awaitLastOpCommitted();

// Routing information:
//   - mongos0: 1|0|a
//   - mongos1: 2|0|a

// Shard information:
//   - shard0: 2|0|a, [-inf, inf)
//   - shard1: 0|0|a

checkShardMajorVersion(st.rs0.getPrimary(), 2);
checkShardMajorVersion(st.rs1.getPrimary(), 0);

// mongos0 still thinks that { x: 1 } belong to st.shard1.shardName, but should be able to
// refresh it's metadata correctly.
assert.neq(null, testDB_s0.user.findOne({x: 1}));

// Routing information:
//   - mongos0: 2|0|a
//   - mongos1: 2|0|a

// Shard information:
//   - shard0: 2|0|a, [-inf, inf)
//   - shard1: 0|0|a

///////////////////////////////////////////////////////
// Test unsharded collection
// mongos versions: s0, s2, s3: 2|0|a

testDB_s1.user.drop();
assert.commandWorked(testDB_s1.user.insert({x: 10}));

// Routing information:
//   - mongos0: 2|0|a
//   - mongos1: 0|0|b

// Shard information:
//   - shard0: UNKNOWN
//   - shard1: 0|0|b

checkShardMajorVersion(st.rs1.getPrimary(), 0);

// mongos0 still thinks { x: 10 } belong to st.shard0.shardName, but since coll is dropped,
// query should be routed to primary shard.
assert.neq(null, testDB_s0.user.findOne({x: 10}));

checkShardMajorVersion(st.rs0.getPrimary(), 0);
checkShardMajorVersion(st.rs1.getPrimary(), 0);

// Routing information:
//   - mongos0: 0|0|b
//   - mongos1: 0|0|b

// Shard information:
//   - shard0: 0|0|b
//   - shard1: 0|0|b

///////////////////////////////////////////////////////
// Test 2 shards with 1 chunk
// mongos versions: s0: 0|0|0, s2, s3: 2|0|a

testDB_s1.user.drop();
testDB_s1.adminCommand({shardCollection: 'test.user', key: {x: 1}});
testDB_s1.adminCommand({split: 'test.user', middle: {x: 0}});

// Routing information:
//   - mongos0: 0|0|b
//   - mongos1: 1|2|c

// Shard information:
//   - shard0: UNKNOWN
//   - shard1: 1|2|c, [-inf, 0), [0, inf)

testDB_s1.user.insert({x: 1});
testDB_s1.user.insert({x: -11});
assert.commandWorked(
    testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: -1}, to: st.shard0.shardName}));

st.configRS.awaitLastOpCommitted();

// Routing information:
//   - mongos0: 0|0|b
//   - mongos1: 2|0|c

// Shard information:
//   - shard0: 2|0|c, [-inf, 0)
//   - shard1: 2|1|c, [0, inf)

checkShardMajorVersion(st.rs0.getPrimary(), 2);
checkShardMajorVersion(st.rs1.getPrimary(), 2);

// Routing information:
//   - mongos0: 0|0|b
//   - mongos1: 2|0|c

// Shard information:
//   - shard0: 2|0|c, [-inf, 0)
//   - shard1: 2|1|c, [0, inf)

///////////////////////////////////////////////////////
// Test mongos thinks unsharded when it's actually sharded

// Set mongos0 to version 0|0|0
testDB_s0.user.drop();

assert.eq(null, testDB_s0.user.findOne({x: 1}));

// Needs to also set mongos1 to version 0|0|0, otherwise it'll complain that collection is
// already sharded.
assert.eq(null, testDB_s1.user.findOne({x: 1}));
assert.commandWorked(testDB_s1.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
testDB_s1.user.insert({x: 1});

assert.commandWorked(
    testDB_s1.adminCommand({moveChunk: 'test.user', find: {x: 0}, to: st.shard0.shardName}));

st.configRS.awaitLastOpCommitted();

// Routing information:
//   - mongos0: 0|0|b
//   - mongos1: 2|0|d

// Shard information:
//   - shard0: 2|0|d, [-inf, inf)
//   - shard1: 0|0|d

checkShardMajorVersion(st.rs0.getPrimary(), 2);
checkShardMajorVersion(st.rs1.getPrimary(), 0);

st.stop();
})();
