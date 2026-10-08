/*
 * Test that CBR correctly costs plans that include a shard filter stage.
 * @tags: [
 *   requires_fcv_90,
 *   # Starts a two-shard cluster.
 *   resource_intensive,
 * ]
 */

import {
    getAllPlans,
    getEngine,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";
import {hasCostBasedGroup} from "jstests/libs/query/explain_v3_helpers.js";
import {isDeferredGetExecutorEnabled} from "jstests/libs/query/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";
import {setParameterOnAllNonConfigNodes} from "jstests/noPassthrough/libs/server_parameter_helpers.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {
        setParameter: {
            featureFlagCostBasedRanker: true,
            internalQueryPlanRanker: "costBased",
            internalQueryCBRCEMode: "heuristicCE",
        },
    },
});

const db = st.getDB("test");

const collName = jsTestName();
const coll = db[collName];
coll.drop();

// Split at {_id: 10} and move the upper chunk so each shard owns half the documents. An empty
// shard would report a zero (Metadata-sourced) cardinality estimate, which makes CBR abandon
// costing the plan and fall back to multiplanning (SERVER-133362), invalidating the assertions
// below.
st.shardColl(coll, {_id: 1} /* key */, {_id: 10} /* split */, {_id: 10} /* move */);
assert.commandWorked(coll.insert(Array.from({length: 20}, (_, i) => ({_id: i, a: 1, b: 2}))));
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

const deferredEngineOn = isDeferredGetExecutorEnabled(db);

for (const frameworkControl of ["forceClassicEngine", "trySbeEngine"]) {
    setParameterOnAllNonConfigNodes(st.s, "internalQueryFrameworkControl", frameworkControl);
    const explain = coll.find({a: 1, b: 1}).explain();

    if (getEngine(explain) === "classic") {
        // CBR must cost all plans even when a SHARDING_FILTER stage is present; it
        // should not fall back to multiplanning.
        getAllPlans(explain).forEach(assertPlanCosted);
    } else {
        const winningPlan = getWinningPlanFromExplain(explain);
        const rejected = getRejectedPlans(explain);
        assert.gt(rejected.length, 0, `Expected at least one rejected plan: ${tojson(explain)}`);
        // TODO SERVER-117707. Remove this conditional once CBR fully supports SBE.
        if (deferredEngineOn) {
            assertPlanCosted(winningPlan);
            rejected.forEach(assertPlanCosted);
        } else {
            assertPlanNotCosted(winningPlan);
            rejected.forEach(assertPlanNotCosted);
        }
    }

    // Without deferred engine choice, SBE V3 explain does not carry the cost estimates.
    // TODO SERVER-136210 Remove this check once V3 supports deferred engine off.
    if (deferredEngineOn) {
        const explain = assert.commandWorked(
            db.runCommand({
                explain: {find: collName, filter: {a: 1, b: 1}},
                verbosity: "plannerStats",
            }),
        );
        const plans = explain.queryPlanner.winningPlan.shards.flatMap((shard) => shard.plans);
        assert.gt(plans.length, 1, `Expected at least one rejected plan: ${tojson(explain)}`);
        plans.forEach((plan) =>
            assert(hasCostBasedGroup(plan), `Expected V3 plan to be costed: ${tojson(plan)}`),
        );
    }
}

st.stop();
