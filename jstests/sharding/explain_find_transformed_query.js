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

import {
    getEngine,
    getPlanCacheKeyFromExplain,
    getPlanCacheShapeHashFromExplain,
    getPlanCacheShapeHashFromObject,
    getQueryPlanner,
    getWinningPlan,
    getWinningSBEPlan
} from "jstests/libs/analyze_plan.js";
import {assertDropAndRecreateCollection} from "jstests/libs/collection_drop_recreate.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
});

st.s.adminCommand({enableSharding: "test"});
const db = st.getDB("test");
const coll = assertDropAndRecreateCollection(db, jsTestName());

// We need two indexes so that the multi-planner is executed.
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.createIndex({a: -1, b: 1}));
const findCmd = {
    find: coll.getName(),
    filter: {a: {$lte: 5}},
    sort: {b: 1},
    skip: 1,
    limit: 2
};
assert.commandWorked(db.runCommand(findCmd));
const explain =
    assert.commandWorked(db.runCommand({explain: findCmd, verbosity: "executionStats"}));

// Assert that the plan cache shape hash from the explain is the same as the one in the plan cache.
const planCacheKey = getPlanCacheKeyFromExplain(explain);
const planCacheEntry =
    coll.aggregate([{$planCacheStats: {}}, {$match: {planCacheKey}}]).toArray().at(0);
assert.neq(planCacheEntry, undefined);
assert.eq(getPlanCacheShapeHashFromObject(planCacheEntry),
          getPlanCacheShapeHashFromExplain(explain));

// Assert that the top stage of the winning plan in the explain is the same as the top stage
// in the executed cached plan.
const queryPlanner = getQueryPlanner(explain);
const engine = getEngine(explain);
switch (engine) {
    case "classic": {
        assert.eq(getWinningPlan(queryPlanner).stage, planCacheEntry.cachedPlan.stage);
        break;
    }
    case "sbe": {
        assert.eq(getWinningSBEPlan(queryPlanner).stages, planCacheEntry.cachedPlan.stages);
        break;
    }
    default: {
        assert(false, `Unknown engine ${engine}`);
        break;
    }
}
st.stop();
