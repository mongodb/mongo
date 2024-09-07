// Tests if explain output for index scans and index seeks stages contains the indexName in the
// executionStats output.
//
// @tags: [
//   assumes_against_mongod_not_mongos,
//   # The SBE plan cache was first enabled in 6.3.
//   requires_fcv_63,
//   featureFlagSbeFull,
// ]

import {getPlanStages, getQueryPlanner, isIxscan} from "jstests/libs/analyze_plan.js";

function assertStageContainsIndexName(stage) {
    assert(stage.hasOwnProperty("indexName"));
    assert.eq(stage["indexName"], "a_1", stage);
}

const coll = db.sbe_ixscan_explain;
coll.drop();
assert.commandWorked(coll.createIndex({a: 1}));

assert.commandWorked(coll.insertMany([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 2, b: 1, c: 2},
    {_id: 2, a: 3, b: 1, c: 3},
    {_id: 3, a: 4, b: 2, c: 4}
]));

let explain = coll.find({a: 3}).hint({a: 1}).explain("executionStats");
let queryPlanner = getQueryPlanner(explain);
assert(isIxscan(db, queryPlanner.winningPlan));
// Ensure the query is run on sbe engine.
assert('slotBasedPlan' in queryPlanner.winningPlan);

let ixscanStages = getPlanStages(explain.executionStats.executionStages, "ixseek");
assert(ixscanStages.length !== 0);
for (let ixscanStage of ixscanStages) {
    assertStageContainsIndexName(ixscanStage);
}
