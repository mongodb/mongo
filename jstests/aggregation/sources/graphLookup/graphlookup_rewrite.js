// Tests that stage rewrite optimizations for $graphLookup work correctly.
//
// This test makes assumptions about how the explain output will be formatted.
// @tags: [
//  assumes_unsharded_collection,
//  do_not_wrap_aggregations_in_facets,
//  requires_pipeline_optimization,
// ]
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');
load("jstests/libs/analyze_plan.js");
load("jstests/libs/fixture_helpers.js");

const coll = db.graphlookup_rewrite;
coll.drop();

assert.commandWorked(coll.insertMany([
    {"from": "a", "foo": 1},
    {"from": "b", "to": "a", "foo": 2},
    {"from": "c", "to": "b", "foo": 3},
    {"from": "d", "to": "b", "foo": 4},
    {"from": "e", "to": "c", "foo": 5},
    {"from": "f", "to": "d", "foo": 6}
]));

const admin = db.getSiblingDB("admin");

const setPipelineOptimizationMode = (mode) => {
    FixtureHelpers.runCommandOnEachPrimary(
        {db: admin, cmdObj: {configureFailPoint: "disablePipelineOptimization", mode}});
};

// Get initial optimization mode.
const pipelineOptParameter = assert.commandWorked(
    db.adminCommand({getParameter: 1, "failpoint.disablePipelineOptimization": 1}));
const oldMode =
    pipelineOptParameter["failpoint.disablePipelineOptimization"].mode ? 'alwaysOn' : 'off';

function assertStagesAndOutput(
    {pipeline = [], expectedStages = [], optimizedAwayStages = [], fieldsToSkip = [], msg = ""}) {
    setPipelineOptimizationMode("off");

    const explain = coll.explain().aggregate(pipeline);
    const output = coll.aggregate(pipeline).toArray();

    for (const stage of expectedStages) {
        assert(aggPlanHasStage(explain, stage),
               `${msg}: missing stage ${stage}: ${tojson(explain)}`);
    }
    for (const stage of optimizedAwayStages) {
        assert(!aggPlanHasStage(explain, stage),
               `${msg}: stage ${stage} not optimized away: ${tojson(explain)}`);
    }

    setPipelineOptimizationMode("alwaysOn");

    const expectedOutput = coll.aggregate(pipeline).toArray();
    assert(orderedArrayEq(output, expectedOutput, true, fieldsToSkip), msg);
}

const graphLookup = {
    $graphLookup: {
        from: "graphlookup_rewrite",
        startWith: "$from",
        connectFromField: "from",
        connectToField: "to",
        as: "out"
    }
};

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {foo: 1}}],
    expectedStages: ["SORT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$sort"],
    msg: "$graphLookup should swap with $sort if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$limit: 100}],
    expectedStages: ["LIMIT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$limit"],
    msg: "$graphLookup should swap with $limit if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$skip: 100}],
    expectedStages: ["SKIP", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["$skip"],
    msg: "$graphLookup should swap with $skip if there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {foo: 1}}, {$limit: 100}],
    expectedStages: ["SORT", "COLLSCAN", "$graphLookup"],
    optimizedAwayStages: ["LIMIT", "$limit"],
    msg: "$graphLookup should swap with $limit and $sort, and $sort should absorb $limit if " +
        "there is no internal $unwind"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$sort: {out: 1}}],
    expectedStages: ["COLLSCAN", "$graphLookup", "$sort"],
    msg: "$graphLookup should not swap with $sort if sort uses fields created by $graphLookup"
});

assertStagesAndOutput({
    pipeline: [graphLookup, {$unwind: "$out"}, {$sort: {foo: 1}}],
    expectedStages: ["COLLSCAN", "$graphLookup", "$sort"],
    msg: "$graphLookup with an internal $unwind should not swap with $sort",
    fieldsToSkip: ["out"]
});

// Reset optimization mode.
setPipelineOptimizationMode(oldMode);
})();
