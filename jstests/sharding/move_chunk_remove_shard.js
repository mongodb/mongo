/**
 * Tests the scenario where a chunk is being moved to a shard that is about to be removed.
 *
 * SERVER-32553 `removeShard` command is not idempotent for the purposes of the
 * sharding continuous config stepdown suite.
 * @tags: [
 *   does_not_support_stepdowns,
 * ]
 */
(function() {
"use strict";

load('jstests/libs/chunk_manipulation_util.js');
load('jstests/sharding/libs/remove_shard_util.js');
load('jstests/libs/fail_point_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

// For startParallelOps to write its state
let staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));

pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

configureFailPointForRS(
    st.configRS.nodes, 'overrideBalanceRoundInterval', {intervalMs: 200}, 'alwaysOn');

let joinMoveChunk = moveChunkParallel(staticMongod,
                                      st.s.host,
                                      {x: 0},
                                      null,
                                      'test.user',
                                      st.shard1.shardName,
                                      false /**parallel should expect failure */);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

removeShard(st, st.shard1.shardName);

unpauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

// moveChunk will fail because the destination shard no longer exists.
joinMoveChunk();

// All shard0 should now own all chunks
st.s.getDB('config').chunks.find().forEach(function(chunk) {
    assert.eq(st.shard0.shardName, chunk.shard, tojson(chunk));
});

st.stop();

MongoRunner.stopMongod(staticMongod);
})();
