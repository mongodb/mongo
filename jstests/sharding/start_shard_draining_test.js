/**
 * Test that startShardDraining works correctly.
 * @tags: [
 * requires_fcv_82
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

function checkDrainingCorrectAndIllegalOperations() {
    jsTestLog(
        "Check that the draining flag is set correctly when calling startShardDraining and that the edge cases fail with the correct exception");

    const st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
    const config = st.s.getDB('config');

    // Start draining
    assert.commandWorked(st.s.adminCommand({startShardDraining: st.shard1.shardName}));

    // Check that draining has started successfully
    const drainingShards = config.shards.find({'draining': true}).toArray();

    // Check only one shard is draining
    assert.eq(1, drainingShards.length);

    // Check that the shard draining is shard1.shardName
    assert.eq(st.shard1.shardName, drainingShards[0]._id);

    // Check that command returns OK if called on an already draining shard
    assert.commandWorked(st.s.adminCommand({startShardDraining: st.shard1.shardName}));

    // Can't drain all shards in the cluster
    assert.commandFailedWithCode(st.s.adminCommand({startShardDraining: st.shard0.shardName}),
                                 ErrorCodes.IllegalOperation);

    // Can't drain non-existent shard
    assert.commandFailedWithCode(st.s.adminCommand({startShardDraining: "shard1"}),
                                 ErrorCodes.ShardNotFound);

    // Remove st.shard1.shardName
    assert.commandWorked(st.s.adminCommand({removeShard: st.shard1.shardName}));

    // Can't drain the last shard in the cluster
    assert.commandFailedWithCode(st.s.adminCommand({startShardDraining: st.shard0.shardName}),
                                 ErrorCodes.IllegalOperation);

    st.stop();
}

function drainConfigShard() {
    jsTestLog("Check that the config shard cannot be drained with startShardDraining");

    const st = new ShardingTest({shards: 2, other: {enableBalancer: true, configShard: true}});
    const config = st.s.getDB('config');

    // Can't drain config shard
    assert.commandFailedWithCode(st.s.adminCommand({startShardDraining: st.shard0.shardName}),
                                 ErrorCodes.IllegalOperation);

    st.stop();
}

checkDrainingCorrectAndIllegalOperations();
drainConfigShard();
