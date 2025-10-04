// Tests that a $changeStream pipeline is split rather than forwarded even in the case where the
// cluster only has a single shard, and that it can therefore successfully look up a document in a
// sharded collection.
// @tags: [
//   # If a rollback is triggered during a stepdown, the change stream cursor can become invalid.
//   does_not_support_stepdowns,
//   requires_majority_read_concern,
//   uses_change_streams,
// ]
// Create a cluster with only 1 shard.
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 1,
    rs: {nodes: 1, setParameter: {periodicNoopIntervalSecs: 1, writePeriodicNoops: true}},
});

const mongosDB = st.s0.getDB(jsTestName());
const mongosColl = mongosDB[jsTestName()];

// Enable sharding, shard on _id, and insert a test document which will be updated later.
assert.commandWorked(mongosDB.adminCommand({enableSharding: mongosDB.getName()}));
assert.commandWorked(mongosDB.adminCommand({shardCollection: mongosColl.getFullName(), key: {_id: 1}}));
assert.commandWorked(mongosColl.insert({_id: 1}));

// Verify that the pipeline splits and merges on router despite only targeting a single shard.
const explainPlan = assert.commandWorked(
    mongosColl.explain().aggregate([{$changeStream: {fullDocument: "updateLookup"}}]),
);
assert.neq(explainPlan.splitPipeline, null);
assert.eq(explainPlan.mergeType, st.getMergeType(mongosDB));

// Open a $changeStream on the collection with 'updateLookup' and update the test doc.
const stream = mongosColl.watch([], {fullDocument: "updateLookup"});
const wholeDbStream = mongosDB.watch([], {fullDocument: "updateLookup"});

mongosColl.update({_id: 1}, {$set: {updated: true}});

// Verify that the document is successfully retrieved from the single-collection and whole-db
// change streams.
assert.soon(() => stream.hasNext());
assert.docEq({_id: 1, updated: true}, stream.next().fullDocument);

assert.soon(() => wholeDbStream.hasNext());
assert.docEq({_id: 1, updated: true}, wholeDbStream.next().fullDocument);

stream.close();
wholeDbStream.close();

st.stop();
