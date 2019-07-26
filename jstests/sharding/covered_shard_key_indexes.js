//
// Tests that indexes containing the shard key can be used without fetching the document for
// particular queries
//

load("jstests/libs/analyze_plan.js");

(function() {
'use strict';

var st = new ShardingTest({shards: 1});
var coll = st.s0.getCollection("foo.bar");

assert.commandWorked(st.s0.adminCommand({enableSharding: coll.getDB() + ""}));

jsTest.log('Tests with _id : 1 shard key');
coll.drop();
assert.commandWorked(st.s0.adminCommand({shardCollection: coll + "", key: {_id: 1}}));
st.printShardingStatus();

assert.commandWorked(
    st.shard0.adminCommand({setParameter: 1, logComponentVerbosity: {query: {verbosity: 5}}}));

// Insert some data
assert.writeOK(coll.insert({_id: true, a: true, b: true}));

// Index without shard key query - not covered
assert.commandWorked(coll.ensureIndex({a: 1}));
assert.eq(1, coll.find({a: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(1, coll.find({a: true}, {_id: 1, a: 1}).explain(true).executionStats.totalDocsExamined);

// Index with shard key query - covered when projecting
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.ensureIndex({a: 1, _id: 1}));
assert.eq(1, coll.find({a: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(0, coll.find({a: true}, {_id: 1, a: 1}).explain(true).executionStats.totalDocsExamined);

// Compound index with shard key query - covered when projecting
assert.commandWorked(coll.dropIndexes());
assert.commandWorked(coll.ensureIndex({a: 1, b: 1, _id: 1}));
assert.eq(1, coll.find({a: true, b: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(
    0,
    coll.find({a: true, b: true}, {_id: 1, a: 1}).explain(true).executionStats.totalDocsExamined);

jsTest.log('Tests with _id : hashed shard key');
coll.drop();
assert.commandWorked(st.s0.adminCommand({shardCollection: coll + "", key: {_id: "hashed"}}));
st.printShardingStatus();

// Insert some data
assert.writeOK(coll.insert({_id: true, a: true, b: true}));

// Index without shard key query - not covered
assert.commandWorked(coll.ensureIndex({a: 1}));
assert.eq(1, coll.find({a: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(1, coll.find({a: true}, {_id: 0, a: 1}).explain(true).executionStats.totalDocsExamined);

// Index with shard key query - can't be covered since hashed index
assert.commandWorked(coll.dropIndex({a: 1}));
assert.eq(1, coll.find({_id: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(1, coll.find({_id: true}, {_id: 0}).explain(true).executionStats.totalDocsExamined);

jsTest.log('Tests with compound shard key');
coll.drop();
assert.commandWorked(st.s0.adminCommand({shardCollection: coll + "", key: {a: 1, b: 1}}));
st.printShardingStatus();

// Insert some data
assert.writeOK(coll.insert({_id: true, a: true, b: true, c: true, d: true}));

// Index without shard key query - not covered
assert.commandWorked(coll.ensureIndex({c: 1}));
assert.eq(1, coll.find({c: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(1,
          coll.find({c: true}, {_id: 0, a: 1, b: 1, c: 1})
              .explain(true)
              .executionStats.totalDocsExamined);

// Index with shard key query - covered when projecting
assert.commandWorked(coll.dropIndex({c: 1}));
assert.commandWorked(coll.ensureIndex({c: 1, b: 1, a: 1}));
assert.eq(1, coll.find({c: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(0,
          coll.find({c: true}, {_id: 0, a: 1, b: 1, c: 1})
              .explain(true)
              .executionStats.totalDocsExamined);

// Compound index with shard key query - covered when projecting
assert.commandWorked(coll.dropIndex({c: 1, b: 1, a: 1}));
assert.commandWorked(coll.ensureIndex({c: 1, d: 1, a: 1, b: 1, _id: 1}));
assert.eq(1, coll.find({c: true, d: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(0,
          coll.find({c: true, d: true}, {a: 1, b: 1, c: 1, d: 1})
              .explain(true)
              .executionStats.totalDocsExamined);

jsTest.log('Tests with nested shard key');
coll.drop();
assert.commandWorked(st.s0.adminCommand({shardCollection: coll + "", key: {'a.b': 1}}));
st.printShardingStatus();

// Insert some data
assert.writeOK(coll.insert({_id: true, a: {b: true}, c: true}));

// Index without shard key query - not covered
assert.commandWorked(coll.ensureIndex({c: 1}));
assert.eq(1, coll.find({c: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(
    1,
    coll.find({c: true}, {_id: 0, 'a.b': 1, c: 1}).explain(true).executionStats.totalDocsExamined);

// Index with shard key query - can be covered given the appropriate projection.
assert.commandWorked(coll.dropIndex({c: 1}));
assert.commandWorked(coll.ensureIndex({c: 1, 'a.b': 1}));
assert.eq(1, coll.find({c: true}).explain(true).executionStats.totalDocsExamined);
assert.eq(
    0,
    coll.find({c: true}, {_id: 0, 'a.b': 1, c: 1}).explain(true).executionStats.totalDocsExamined);

jsTest.log('Tests with bad data with no shard key');
coll.drop();
assert.commandWorked(st.s0.adminCommand({shardCollection: coll + "", key: {a: 1}}));
st.printShardingStatus();

// Insert some bad data manually on the shard
assert.writeOK(st.shard0.getCollection(coll.toString()).insert({_id: "bad data", c: true}));

// Index without shard key query - not covered but succeeds
assert.commandWorked(coll.ensureIndex({c: 1}));
var explain = coll.find({c: true}).explain(true).executionStats;
assert.eq(0, explain.nReturned);
assert.eq(1, explain.totalDocsExamined);
assert.eq(1, getChunkSkips(explain.executionStages.shards[0].executionStages));

// Index with shard key query - covered and succeeds and returns result
//
// NOTE: This is weird and only a result of the fact that we don't have a dedicated "does not
// exist" value for indexes
assert.commandWorked(coll.ensureIndex({c: 1, a: 1}));
var explain = coll.find({c: true}, {_id: 0, a: 1, c: 1}).explain(true).executionStats;
assert.eq(1, explain.nReturned);
assert.eq(0, explain.totalDocsExamined);
assert.eq(0, getChunkSkips(explain.executionStages.shards[0].executionStages));

st.stop();
})();
