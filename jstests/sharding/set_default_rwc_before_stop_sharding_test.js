/**
 * Tests that none of the operations in the ShardingTest shutdown consistency checks are affected by
 * the cluster wide default read and write concern.
 */
(function() {
"use strict";

const st = new ShardingTest({shards: 1, rs: {nodes: 1, enableMajorityReadConcern: "false"}});

// Create a sharded collection so the index and uuid hooks have something to check.
assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
assert.commandWorked(st.s.adminCommand({shardCollection: "test.foo", key: {_id: 1}}));

// Deliberately set an unsatisfiable default read and write concern so any operations run in the
// shutdown hooks will fail if they inherit either.
assert.commandWorked(st.s.adminCommand({
    setDefaultRWConcern: 1,
    defaultWriteConcern: {w: 42},
    defaultReadConcern: {level: "majority"}
}));

st.stop();
})();
