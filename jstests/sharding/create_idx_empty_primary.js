/**
 * Test to make sure that the createIndex command gets sent to shards that own chunks.
 */
(function() {
'use strict';

var st = new ShardingTest({shards: 2});
assert.commandWorked(st.s.adminCommand({enablesharding: 'test'}));
st.ensurePrimaryShard('test', st.shard1.shardName);

var testDB = st.s.getDB('test');
assert.commandWorked(testDB.adminCommand({shardcollection: 'test.user', key: {_id: 1}}));

// Move only chunk out of primary shard.
assert.commandWorked(
    testDB.adminCommand({movechunk: 'test.user', find: {_id: 0}, to: st.shard0.shardName}));

assert.commandWorked(testDB.user.insert({_id: 0}));

var res = testDB.user.ensureIndex({i: 1});
assert.commandWorked(res);

var indexes = testDB.user.getIndexes();
assert.eq(2, indexes.length);

indexes = st.rs0.getPrimary().getDB('test').user.getIndexes();
assert.eq(2, indexes.length);

indexes = st.rs1.getPrimary().getDB('test').user.getIndexes();
assert.eq(1, indexes.length);

st.stop();
})();
