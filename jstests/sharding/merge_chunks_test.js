//
// Tests that merging chunks via mongos works/doesn't work with different chunk configurations
//
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {findChunksUtil} from "jstests/sharding/libs/find_chunks_util.js";

let st = new ShardingTest({shards: 2, mongos: 2});

let mongos = st.s0;
let staleMongos = st.s1;
let admin = mongos.getDB("admin");
const kDbName = "foo";

assert.commandWorked(admin.runCommand({enableSharding: kDbName, primaryShard: st.shard0.name}));
let coll = mongos.getCollection(kDbName + ".bar");
assert.commandWorked(admin.runCommand({shardCollection: coll + "", key: {_id: 1}}));

// Create ranges MIN->0,0->10,(hole),20->40,40->50,50->90,(hole),100->110,110->MAX on first
// shard
jsTest.log("Creating ranges...");

assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 0}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 10}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 20}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 40}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 50}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 90}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 100}}));
assert.commandWorked(admin.runCommand({split: coll + "", middle: {_id: 110}}));

assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 10}, to: st.shard1.shardName}));
assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 90}, to: st.shard1.shardName}));

st.printShardingStatus();

// Insert some data into each of the consolidated ranges
assert.commandWorked(coll.insert({_id: 0}));
assert.commandWorked(coll.insert({_id: 10}));
assert.commandWorked(coll.insert({_id: 40}));
assert.commandWorked(coll.insert({_id: 110}));

let staleCollection = staleMongos.getCollection(coll + "");

jsTest.log("Trying merges that should fail...");

// S0: min->0, 0->10, 20->40, 40->50, 50->90, 100->110, 110->max
// S1: 10->20, 90->100

// Make sure merging non-exact chunks is invalid
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 5}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 5}, {_id: 10}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 15}, {_id: 50}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 55}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 105}, {_id: MaxKey}]}));

// Make sure merging over holes is invalid
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 0}, {_id: 40}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 40}, {_id: 110}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 50}, {_id: 110}]}));

// Make sure merging between shards is invalid
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 0}, {_id: 20}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 10}, {_id: 40}]}));
assert.commandFailed(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 40}, {_id: 100}]}));
assert.eq(4, staleCollection.find().itcount());

jsTest.log("Trying merges that should succeed...");

// Merging single chunks should be treated as a no-op
// (or fail because 'the range specifies one single chunk' in multiversion test environments)
try {
    assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 0}]}));
} catch (e) {
    if (!e.message.match(/could not merge chunks, collection .* already contains chunk for/)) {
        throw e;
    }
}
try {
    assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 40}]}));
} catch (e) {
    if (!e.message.match(/could not merge chunks, collection .* already contains chunk for/)) {
        throw e;
    }
}
try {
    assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 110}, {_id: MaxKey}]}));
} catch (e) {
    if (!e.message.match(/could not merge chunks, collection .* already contains chunk for/)) {
        throw e;
    }
}
// Make sure merge including the MinKey works
assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 10}]}));
assert.eq(4, staleCollection.find().itcount());
// S0: min->10, 20->40, 40->50, 50->90, 100->110, 110->max
// S1: 10->20, 90->100

// Make sure merging three chunks in the middle works
assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 20}, {_id: 90}]}));
assert.eq(4, staleCollection.find().itcount());
// S0: min->10, 20->90, 100->110, 110->max
// S1: 10->20, 90->100

// Make sure merge including the MaxKey works
assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 100}, {_id: MaxKey}]}));
assert.eq(4, staleCollection.find().itcount());
// S0: min->10, 20->90, 100->max
// S1: 10->20, 90->100

// Make sure merging chunks after a chunk has been moved out of a shard succeeds
assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 110}, to: st.shard1.shardName}));
assert.commandWorked(admin.runCommand({moveChunk: coll + "", find: {_id: 10}, to: st.shard0.shardName}));
assert.eq(4, staleCollection.find().itcount());
// S0: min->10, 10->20, 20->90
// S1: 90->100, 100->max

assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: 90}, {_id: MaxKey}]}));
assert.eq(4, staleCollection.find().itcount());
// S0: min->10, 10->20, 20->90
// S1: 90->max

// Make sure merge on the other shard after a chunk has been merged succeeds
assert.commandWorked(admin.runCommand({mergeChunks: coll + "", bounds: [{_id: MinKey}, {_id: 90}]}));
// S0: min->90
// S1: 90->max

st.printShardingStatus(true);

assert.eq(2, findChunksUtil.findChunksByNs(st.s0.getDB("config"), "foo.bar").itcount());
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB("config"), "foo.bar", {
            "min._id": MinKey,
            "max._id": 90,
            shard: st.shard0.shardName,
        })
        .itcount(),
);
assert.eq(
    1,
    findChunksUtil
        .findChunksByNs(st.s0.getDB("config"), "foo.bar", {
            "min._id": 90,
            "max._id": MaxKey,
            shard: st.shard1.shardName,
        })
        .itcount(),
);

st.stop();
