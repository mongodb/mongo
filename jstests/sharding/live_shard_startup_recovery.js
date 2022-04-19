/**
 * Tests that sharding state is properly initialized on shard members that undergo startup recovery.
 *
 * @tags: [requires_persistence]
 */

(function() {
"use strict";

load("jstests/sharding/libs/sharding_state_test.js");

const st = new ShardingTest({config: 1, shards: {rs0: {nodes: 1}}});
const rs = st.rs0;
let primary = rs.getPrimary();

primary = ShardingStateTest.putNodeInStartupRecovery({replSet: rs, node: primary});

jsTestLog("Ensuring node is up as a primary and checking sharding state");
ShardingStateTest.failoverToMember(rs, primary);
ShardingStateTest.checkShardingState(st);

st.stop();
})();
