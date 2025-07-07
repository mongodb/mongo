/**
 * Check the startShardDraining and removeShard interoperability
 * @tags: [
 * requires_fcv_82
 * ]
 */

import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({shards: 2, other: {enableBalancer: true}});
const config = st.s0.getDB('config');

// Add sharded collections
assert.commandWorked(
    st.s.adminCommand({enableSharding: 'TestDB', primaryShard: st.shard1.shardName}));
assert.commandWorked(st.s.adminCommand({shardCollection: 'TestDB.Coll', key: {_id: 1}}));
st.s.getDB('TestDB').Coll.insert({_id: -1, value: 'Negative value'});
st.s.getDB('TestDB').Coll.insert({_id: 1, value: 'Positive value'});

// Add unsharded collections
assert.commandWorked(st.s.getDB("TestDB").CollUnsharded.insert({_id: 1, value: "Pos"}));

// Start draining
assert.commandWorked(st.s.adminCommand({startShardDraining: st.shard1.shardName}));

// Check that removeShard returns 'ongoing'
const removeStatus_ongoing = st.s.adminCommand({removeShard: st.shard1.shardName});
assert.eq('ongoing', removeStatus_ongoing.state);

// Move the unsharded collection off st.shard1.shardName
assert.commandWorked(st.s.adminCommand({movePrimary: "TestDB", to: st.shard0.shardName}));

// Wait for the shard to be completely drained, then removeShard must remove the shard.
assert.soon(() => {
    const removeStatus_completed = st.s.adminCommand({removeShard: st.shard1.shardName});
    return 'completed' == removeStatus_completed.state;
}, "removeShard did not return 'completed' status within the timeout.");

st.stop();
