// This tests sharding an existing collection that both shards are aware of (SERVER-2828)
(function() {
'use strict';

var st = new ShardingTest({shards: 2, mongos: 2});

assert.commandWorked(st.s0.adminCommand({enablesharding: "test"}));
st.ensurePrimaryShard('test', st.shard1.shardName);

// "test.foo" - sharded (by mongos 0)
assert.commandWorked(st.s0.adminCommand({shardcollection: "test.foo", key: {num: 1}}));

// "test.existing" - unsharded
assert.writeOK(st.s0.getDB('test').existing.insert({_id: 1}));
assert.eq(1, st.s0.getDB('test').existing.count({_id: 1}));
assert.eq(1, st.s1.getDB('test').existing.count({_id: 1}));

// "test.existing" - unsharded to sharded (by mongos 1)
assert.commandWorked(st.s1.adminCommand({shardcollection: "test.existing", key: {_id: 1}}));
assert.commandWorked(st.s1.adminCommand({split: "test.existing", middle: {_id: 5}}));
assert.commandWorked(
    st.s1.adminCommand({moveChunk: "test.existing", find: {_id: 1}, to: st.shard0.shardName}));

assert.eq(1, st.s0.getDB('test').existing.count({_id: 1}));  // SERVER-2828
assert.eq(1, st.s1.getDB('test').existing.count({_id: 1}));

// Test stats
assert.writeOK(st.s0.getDB('test').existing2.insert({_id: 1}));
assert.eq(1, st.s0.getDB('test').existing2.count({_id: 1}));
assert.eq(1, st.s1.getDB('test').existing2.count({_id: 1}));

assert.commandWorked(st.s1.adminCommand({shardcollection: "test.existing2", key: {_id: 1}}));
assert.eq(true, st.s1.getDB('test').existing2.stats().sharded);

assert.commandWorked(st.s1.adminCommand({split: "test.existing2", middle: {_id: 5}}));
{
    var res = st.s0.getDB('test').existing2.stats();
    printjson(res);
    assert.eq(true, res.sharded);  // SERVER-2828
}

// Test admin commands
assert.writeOK(st.s0.getDB('test').existing3.insert({_id: 1}));
assert.eq(1, st.s0.getDB('test').existing3.count({_id: 1}));
assert.eq(1, st.s1.getDB('test').existing3.count({_id: 1}));

assert.writeOK(st.s1.adminCommand({shardcollection: "test.existing3", key: {_id: 1}}));
assert.commandWorked(st.s1.adminCommand({split: "test.existing3", middle: {_id: 5}}));
assert.commandWorked(
    st.s0.adminCommand({moveChunk: "test.existing3", find: {_id: 1}, to: st.shard0.shardName}));

st.stop();
})();
