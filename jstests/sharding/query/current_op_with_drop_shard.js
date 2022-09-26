// Tests that currentOp is resilient to drop shard.
(function() {
'use strict';
load('jstests/sharding/libs/remove_shard_util.js');

// TODO SERVER-50144 Remove this and allow orphan checking.
// This test calls removeShard which can leave docs in config.rangeDeletions in state "pending",
// therefore preventing orphans from being cleaned up.
TestData.skipCheckOrphans = true;

const st = new ShardingTest({shards: 2, rs: {nodes: 1}});

// We need the balancer to remove a shard.
st.startBalancer();

const mongosDB = st.s.getDB(jsTestName());
const shardName = st.shard0.shardName;

removeShard(st, shardName);

assert.commandWorked(mongosDB.currentOp());

st.stop();
})();
