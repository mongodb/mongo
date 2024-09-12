/**
 * In a sharded cluster, we send modified queries to the shards. The tests below make sure that we
 * are also sending the modified command to the shards when we're running a find command with an
 * explain.
 * @tags: [
 *   assumes_read_concern_local,
 *   does_not_support_transactions,
 *   # This test attempts to perform queries with plan cache filters set up. The former operation
 *   # may be routed to a secondary in the replica set, whereas the latter must be routed to the
 *   # primary.
 *   assumes_read_concern_unchanged,
 *   assumes_read_preference_unchanged,
 *   assumes_unsharded_collection,
 *   does_not_support_stepdowns,
 *   # Plan cache state is node-local and will not get migrated alongside user data
 *   assumes_balancer_off,
 * ]
 */

import {getQueryPlanner, getWinningPlan} from "jstests/libs/analyze_plan.js";
import {checkSbeFullFeatureFlagEnabled} from "jstests/libs/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

st.s.adminCommand({enableSharding: "test"});
const db = st.getDB("test");
const dbAtShard0 = st.shard0.getDB(jsTestName());
const dbAtShard1 = st.shard1.getDB(jsTestName());

const collName = "jstests_explain_find";
const t = db[collName];
t.drop();

// We need two indexes so that the multi-planner is executed.
assert.commandWorked(t.createIndex({a: 1}));
assert.commandWorked(t.createIndex({a: -1, b: 1}));

const find = assert.commandWorked(
    db.runCommand({find: collName, filter: {a: {$lte: 5}}, sort: {b: 1}, skip: 1, limit: 2}));

const stats = assert.commandWorked(db.runCommand({
    explain: {find: collName, filter: {a: {$lte: 5}}, sort: {b: 1}, skip: 1, limit: 2},
    verbosity: "executionStats"
}));

// Assert that the query hash from the explain is the same as the one in the plan cache.
const shardExplains = stats.queryPlanner.winningPlan.shards;
const planCacheKey = shardExplains[0].planCacheKey;
const planCacheExecutedPlan =
    t.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray()[0];
const queryHashPlanCache = planCacheExecutedPlan.queryHash;

assert(shardExplains.every(s => s.queryHash === queryHashPlanCache), shardExplains);

// Assert that the top stage of the winning plan in the explain is the same as the top stage
// in the executed cached plan.
const queryPlanner = getQueryPlanner(stats);
const winningPlanExplain = getWinningPlan(queryPlanner);
if (!checkSbeFullFeatureFlagEnabled(db)) {
    assert.eq(winningPlanExplain.stage, planCacheExecutedPlan.cachedPlan.stage);
} else {
    assert.eq(queryPlanner.winningPlan.slotBasedPlan.stages,
              planCacheExecutedPlan.cachedPlan.stages);
}

st.stop();
