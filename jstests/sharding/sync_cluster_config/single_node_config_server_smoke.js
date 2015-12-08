// This test does the minimal validation that basic sharding operations work with single-node
// config servers.

(function() {

'use strict';

// Starting a new ShardingTest already exercises the addShard logic
var st = new ShardingTest({ config: 1, mongos: 1, shards: 2, other: { sync: true } });

// Enable sharding
assert.commandWorked(st.s.adminCommand({ enableSharding: 'TestDB' }));
st.s.adminCommand({ movePrimary: 'TestDB', to: 'shard0000' });

// Shard collection
assert.commandWorked(st.s.adminCommand({ shardCollection: 'TestDB.TestColl', key: { Key: 1 } }));

var TestColl = st.s.getDB('TestDB').TestColl;
TestColl.insert({ Key: 1, Value: "Positive Key" });
TestColl.insert({ Key: -1, Value: "Negative Key" });
assert.eq(2, TestColl.count({}));

// Split + move chunk
assert.commandWorked(st.s.adminCommand({ split: 'TestDB.TestColl', find: { Key: 0 } }));
assert.commandWorked(st.s.adminCommand({ moveChunk: 'TestDB.TestColl',
                                         find: { Key: 0 },
                                         to: 'shard0001' }));

// Ensure documents are found
assert.eq(2, TestColl.count({}));
assert.eq(1, TestColl.find({ Key: 1 }).itcount());
assert.eq(1, TestColl.find({ Key: -1 }).itcount());

st.stop();

})();
