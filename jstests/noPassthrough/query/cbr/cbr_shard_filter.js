/*
 * Test that CBR correctly costs plans that include a shard filter stage.
 * @tags: [requires_fcv_90]
 */

import {
    getAllPlans,
    getEngine,
    getRejectedPlans,
    getWinningPlanFromExplain,
} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted, assertPlanNotCosted} from "jstests/libs/query/cbr_utils.js";
import {isDeferredGetExecutorEnabled} from "jstests/libs/query/sbe_util.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

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

// TODO SERVER-130179: The deferred engine choice path passes an empty EstimateMap to the plan
// executor, so costEstimate is absent from explain output even with internalQueryPlanRanker set to
// costBased. Skip when that feature flag is on until the C++ path is fixed.
if (
    db.adminCommand({getParameter: 1, featureFlagGetExecutorDeferredEngineChoice: 1})
        .featureFlagGetExecutorDeferredEngineChoice?.value
) {
    jsTest.log.info(
        "Skipping: featureFlagGetExecutorDeferredEngineChoice bypasses CBR cost estimation in explain",
    );
    st.stop();
    quit();
}

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

const explain = coll.find({a: 1, b: 1}).explain();

if (getEngine(explain) === "classic") {
    // CBR must cost all plans even when a SHARDING_FILTER stage is present; it
    // should not fall back to multiplanning.
    getAllPlans(explain).forEach(assertPlanCosted);
} else {
    const winningPlan = getWinningPlanFromExplain(explain, true /* SBE plan */);
    // TODO SERVER-129522. Winning plan's explain should also populate cost info in SBE.
    assertPlanNotCosted(winningPlan);
    const rejected = getRejectedPlans(explain);
    assert.gt(rejected.length, 0, `Expected at least one rejected plan: ${tojson(explain)}`);
    // TODO SERVER-117707. Remove this conditional once CBR fully supports SBE.
    if (isDeferredGetExecutorEnabled(db)) {
        rejected.forEach(assertPlanCosted);
    } else {
        rejected.forEach(assertPlanNotCosted);
    }
}

st.stop();
