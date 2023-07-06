/**
 * Utilities for the find_and_modify_without_two_phase.js and delete_without_two_phase.js tests.
 */

/**
 * Runs an explain on the `cmdObj` and checks that the explain targets the shard given be
 * `expectedShardName`.
 */
export function assertExplainTargetsCorrectShard(db, cmdObj, expectedShardName) {
    var res = db.runCommand({explain: cmdObj});
    assert.eq(res.queryPlanner.winningPlan.shards.length, 1);
    assert.eq(res.queryPlanner.winningPlan.shards[0].shardName, expectedShardName);
}

/**
 * Performs a split given by the `splitDoc`, and then moves the chunk containg `moveShard0Doc` to
 * shard0 and the chunk containing `moveShard1Doc` to shard1.
 */
export function splitAndMoveChunks(st, splitDoc, moveShard0Doc, moveShard1Doc) {
    assert.commandWorked(st.s0.adminCommand({split: "test.sharded_coll", middle: splitDoc}));

    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: "test.sharded_coll", find: moveShard0Doc, to: st.shard0.shardName}));
    assert.commandWorked(st.s0.adminCommand(
        {moveChunk: "test.sharded_coll", find: moveShard1Doc, to: st.shard1.shardName}));
}
