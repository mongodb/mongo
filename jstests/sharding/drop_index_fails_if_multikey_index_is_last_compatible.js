/*
 * Tests that the dropIndex command fails to drop the shard key index if the last remaining
 * compatible index is multikey
 */

(function() {
"use strict";
const st = new ShardingTest({mongos: 1, config: 1, shards: 1, rs: {nodes: 1}});

assert.commandWorked(st.s.adminCommand({enableSharding: "test"}));
assert.commandWorked(st.s.adminCommand({shardCollection: "test.test", key: {a: 1}}));
assert.commandWorked(st.s.getCollection("test.test").insert({a: 1, b: [1, 2]}));
assert.commandWorked(st.s.getCollection("test.test").createIndex({a: 1, b: 1}));
assert.commandFailedWithCode(st.s.getCollection("test.test").dropIndex({a: 1}),
                             ErrorCodes.CannotDropShardKeyIndex);
st.stop();
})();
