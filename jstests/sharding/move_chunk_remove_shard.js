/**
 * Tests the scenario where a chunk is being moved to a shard that is about to be removed.
 */
(function() {
"use strict";

load('./jstests/libs/chunk_manipulation_util.js');

// For startParallelOps to write its state
let staticMongod = MongoRunner.runMongod({});

let st = new ShardingTest({shards: 2});

assert.commandWorked(st.s.adminCommand({enableSharding: 'test'}));
st.ensurePrimaryShard('test', st.shard0.shardName);
assert.commandWorked(st.s.adminCommand({shardCollection: 'test.user', key: {x: 1}}));
assert.commandWorked(st.s.adminCommand({split: 'test.user', middle: {x: 0}}));

pauseMoveChunkAtStep(st.shard0, moveChunkStepNames.reachedSteadyState);

st._configServers.forEach((conn) => {
    conn.adminCommand({
        configureFailPoint: 'overrideBalanceRoundInterval',
        mode: 'alwaysOn',
        data: {intervalMs: 200}
    });
});

let joinMoveChunk = moveChunkParallel(staticMongod,
                                      st.s.host,
                                      {x: 0},
                                      null,
                                      'test.user',
                                      st.shard1.shardName,
                                      false /**parallel should expect failure */);

waitForMoveChunkStep(st.shard0, moveChunkStepNames.reachedSteadyState);

assert.soon(function() {
    let res = assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));
    return res.state == 'completed';
});

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
