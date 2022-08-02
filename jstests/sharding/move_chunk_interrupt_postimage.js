/**
 * Tests that chunk migration interruption before processing post/pre image oplog entry leads
 * to consistent config.transactions data across primary and secondaries (SERVER-67492)
 */

(function() {
"use strict";

load("jstests/sharding/libs/create_sharded_collection_util.js");
load("jstests/libs/fail_point_util.js");
load('jstests/libs/parallel_shell_helpers.js');

const st = new ShardingTest({mongos: 1, config: 1, shards: 2, rs: {nodes: 2}});
const interruptBeforeProcessingPrePostImageOriginatingOpFP =
    configureFailPoint(st.rs1.getPrimary(), "interruptBeforeProcessingPrePostImageOriginatingOp");

const collection = st.s.getCollection("test.mycoll");
CreateShardedCollectionUtil.shardCollectionWithChunks(collection, {x: 1}, [
    {min: {x: MinKey}, max: {x: 0}, shard: st.shard0.shardName},
    {min: {x: 0}, max: {x: 100}, shard: st.shard0.shardName},
    {min: {x: 100}, max: {x: MaxKey}, shard: st.shard1.shardName},
]);

assert.commandWorked(collection.insert({_id: 0, x: 10}));
assert.commandWorked(collection.runCommand("findAndModify", {
    query: {x: 10},
    update: {$set: {y: 2}},
    new: true,
    txnNumber: NumberLong(1),
}));

const res = st.s.adminCommand({
    moveChunk: collection.getFullName(),
    find: {x: 10},
    to: st.shard1.shardName,
});
assert.commandFailedWithCode(res, ErrorCodes.CommandFailed);
interruptBeforeProcessingPrePostImageOriginatingOpFP.wait();

interruptBeforeProcessingPrePostImageOriginatingOpFP.off();
st.stop();
})();
