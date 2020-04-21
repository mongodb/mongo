/**
 * Basic test to demonstrate that the range deleter section in shardingStatistics is displayed
 * correctly.
 */
(function() {
"use strict";

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.foo', key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'test.foo', middle: {x: 0}}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.bar', key: {x: 1}}));

let testDB = st.s.getDB('test');

let bulk = testDB.foo.initializeUnorderedBulkOp();
for (let x = -5; x < 5; x++) {
    bulk.insert({x: x});
}
assert.commandWorked(bulk.execute());

bulk = testDB.bar.initializeUnorderedBulkOp();
for (let x = 0; x < 5; x++) {
    bulk.insert({x: x});
}
assert.commandWorked(bulk.execute());

// Keep the cursors open so range deleter tasks would block on these cursors.
let fooNegativeCursor = testDB.foo.find({x: {$lt: 0}}).batchSize(2);
fooNegativeCursor.next();

let fooPositiveCursor = testDB.foo.find({x: {$gte: 0}}).batchSize(2);
fooPositiveCursor.next();

let barCursor = testDB.bar.find().batchSize(2);
barCursor.next();

assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.foo', find: {x: 0}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.foo', find: {x: -1}, to: st.shard1.shardName}));
assert.commandWorked(
    st.s.adminCommand({moveChunk: 'test.bar', find: {x: 0}, to: st.shard1.shardName}));

let shardDB = st.rs0.getPrimary().getDB('test');
let serverStatusRes = shardDB.runCommand({serverStatus: 1});
assert.eq(3, serverStatusRes.shardingStatistics.rangeDeleterTasks);

// Close the cursors so the range deleter tasks can proceed and so there won't be tasks that
// can't make progress when the check orphan hooks runs.
fooNegativeCursor.close();
fooPositiveCursor.close();
barCursor.close();

st.stop();
})();
