/*
 * Test that CBR correctly costs plans that include a shard filter stage.
 * @tags: [requires_fcv_90]
 */

import {getAllPlans} from "jstests/libs/query/analyze_plan.js";
import {assertPlanCosted} from "jstests/libs/query/cbr_utils.js";
import {ShardingTest} from "jstests/libs/shardingtest.js";

const st = new ShardingTest({
    shards: 2,
    mongos: 1,
    rs: {setParameter: {featureFlagCostBasedRanker: true, internalQueryCBRCEMode: "heuristicCE"}},
});

const db = st.getDB("test");
const collName = jsTestName();
const coll = db[collName];
coll.drop();

assert.commandWorked(
    st.s.adminCommand({shardCollection: coll.getFullName(), key: {_id: "hashed"}}),
);

assert.commandWorked(coll.insert({a: 1, b: 2}));
assert.commandWorked(coll.createIndexes([{a: 1}, {b: 1}]));

// CBR must cost all plans even when a SHARDING_FILTER stage is present; it should not fall back
// to multiplanning.
const explain = coll.find({a: 1, b: 1}).explain();
getAllPlans(explain).forEach(assertPlanCosted);

st.stop();
