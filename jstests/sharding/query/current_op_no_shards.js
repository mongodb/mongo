/**
 * Test that running a $currentOp aggregation on a cluster with no shards returns an empty result
 * set, and does not cause the mongoS floating point failure described in SERVER-30084.
 *
 * Requires no shards so there can't be a config shard.
 * @tags: [config_shard_incompatible]
 */
(function() {
const st = new ShardingTest({shards: 0});

const adminDB = st.s.getDB("admin");

assert.commandWorked(adminDB.runCommand({aggregate: 1, pipeline: [{$currentOp: {}}], cursor: {}}));
assert.commandWorked(adminDB.currentOp());

assert.eq(adminDB.aggregate([{$currentOp: {}}]).itcount(), 0);
assert.eq(adminDB.currentOp().inprog.length, 0);

st.stop();
})();
