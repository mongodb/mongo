// Tests that an aggregation pipeline can be optimized away and the query can be answered using
// just the query layer if the pipeline has only one $cursor source, or if the pipeline can be
// collapsed into a single $cursor source pipeline. The resulting cursor in this case will look
// like what the client would have gotten from find command.
//
// Relies on the pipeline stages to be collapsed into a single $cursor stage, so pipelines cannot be
// wrapped into a facet stage to not prevent this optimization. Also, this test is not prepared to
// handle explain output for sharded collections.
// This test makes assumptions about how the explain output will be formatted, so cannot be run when
// pipeline optimization is disabled.
// @tags: [
//   assumes_unsharded_collection,
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
//   requires_profiling,
// ]
(function() {
"use strict";

load("jstests/concurrency/fsm_workload_helpers/server_types.js");  // For isWiredTiger.
load("jstests/libs/analyze_plan.js");     // For 'aggPlanHasStage' and other explain helpers.
load("jstests/libs/fixture_helpers.js");  // For 'isMongos' and 'isSharded'.
load("jstests/libs/sbe_util.js");         // For checkSBEEnabled.

const coll = db.optimize_away_pipeline;
coll.drop();
assert.commandWorked(coll.insert({_id: 1, x: 10}));
assert.commandWorked(coll.insert({_id: 2, x: 20}));
assert.commandWorked(coll.insert({_id: 3, x: 30}));

// Asserts that the give pipeline has *not* been optimized away and the request is answered
// using the aggregation module. There should be pipeline stages present in the explain output.
// The functions also asserts that a query stage passed in the 'stage' argument is present in
// the explain output. If 'expectedResult' is provided, the pipeline is executed and the
// returned result as validated agains the expected result without respecting the order of the
// documents. If 'preserveResultOrder' is 'true' - the order is respected.
//
// If 'optimizedAwayStages' is non-null, then it should contain a list of agg plan stages that
// should *not* be present in the pipeline, since their execution was pushed down into the query
// layer. The test will verify that this pushdown is reflected in explain output.
function assertPipelineUsesAggregation({
    pipeline = [],
    pipelineOptions = {},
    expectedStages = null,
    expectedResult = null,
    preserveResultOrder = false,
    optimizedAwayStages = null
} = {}) {
    const explainOutput = coll.explain().aggregate(pipeline, pipelineOptions);

    assert(isAggregationPlan(explainOutput), explainOutput);
    assert(!isQueryPlan(explainOutput), explainOutput);

    if (optimizedAwayStages) {
        for (let stage of optimizedAwayStages) {
            assert(!aggPlanHasStage(explainOutput, stage), explainOutput);
        }
    }

    let cursor = getAggPlanStage(explainOutput, "$cursor");
    if (cursor) {
        cursor = cursor.$cursor;
    } else {
        cursor = getAggPlanStage(explainOutput, "$geoNearCursor").$geoNearCursor;
    }

    assert(cursor, explainOutput);
    assert(cursor.queryPlanner.optimizedPipeline === undefined, explainOutput);

    if (expectedStages) {
        for (let expectedStage of expectedStages) {
            assert(aggPlanHasStage(explainOutput, expectedStage), explainOutput);
        }
    }

    if (expectedResult) {
        const actualResult = coll.aggregate(pipeline, pipelineOptions).toArray();
        if (preserveResultOrder) {
            assert.docEq(actualResult, expectedResult);
        } else {
            assert.sameMembers(actualResult, expectedResult);
        }
    }

    return explainOutput;
}

// Asserts that the give pipeline has been optimized away and the request is answered using
// just the query module. There should be no pipeline stages present in the explain output.
// The functions also asserts that a query stage passed in the 'stage' argument is present in
// the explain output. If 'expectedResult' is provided, the pipeline is executed and the
// returned result as validated agains the expected result without respecting the order of the
// documents. If 'preserveResultOrder' is 'true' - the order is respected.
function assertPipelineDoesNotUseAggregation({
    pipeline = [],
    pipelineOptions = {},
    expectedStages = null,
    expectedResult = null,
    preserveResultOrder = false
} = {}) {
    const explainOutput = coll.explain().aggregate(pipeline, pipelineOptions);

    assert(!isAggregationPlan(explainOutput), explainOutput);
    assert(isQueryPlan(explainOutput), explainOutput);
    if (explainOutput.hasOwnProperty("shards")) {
        Object.keys(explainOutput.shards)
            .forEach((shard) =>
                         assert(explainOutput.shards[shard].queryPlanner.optimizedPipeline === true,
                                explainOutput));
    } else {
        assert(explainOutput.queryPlanner.optimizedPipeline === true, explainOutput);
    }

    if (expectedStages) {
        for (let expectedStage of expectedStages) {
            assert(planHasStage(db, explainOutput, expectedStage), explainOutput);
        }
    }

    if (expectedResult) {
        const actualResult = coll.aggregate(pipeline, pipelineOptions).toArray();
        if (preserveResultOrder) {
            assert.docEq(actualResult, expectedResult);
        } else {
            assert.sameMembers(actualResult, expectedResult);
        }
    }

    return explainOutput;
}

// Test that getMore works with the optimized query.
function testGetMore({command = null, expectedResult = null} = {}) {
    const documents =
        new DBCommandCursor(db, assert.commandWorked(db.runCommand(command)), 1 /* batchsize */)
            .toArray();
    assert.sameMembers(documents, expectedResult);
}

const groupPushdownEnabled = checkSBEEnabled(db);

// Calls 'assertPushdownEnabled' if groupPushdownEnabled is 'true'. Otherwise, it calls
// 'assertPushdownDisabled'.
function assertPipelineIfGroupPushdown(assertPushdownEnabled, assertPushdownDisabled) {
    return groupPushdownEnabled ? assertPushdownEnabled() : assertPushdownDisabled();
}

let explainOutput;

// Basic pipelines.

// Test basic scenarios when a pipeline has a single $cursor stage or can be collapsed into a
// single cursor stage.
assertPipelineDoesNotUseAggregation({
    pipeline: [],
    expectedStages: ["COLLSCAN"],
    expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
});
assertPipelineDoesNotUseAggregation(
    {pipeline: [{$match: {x: 20}}], expectedStage: "COLLSCAN", expectedResult: [{_id: 2, x: 20}]});

// Pipelines with a collation.

// Test a simple pipeline with a case-insensitive collation.
assert.commandWorked(coll.insert({_id: 4, x: 40, b: "abc"}));
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {b: "ABC"}}],
    pipelineOptions: {collation: {locale: "en_US", strength: 2}},
    expectedStages: ["COLLSCAN"],
    expectedResult: [{_id: 4, x: 40, b: "abc"}]
});
assert.commandWorked(coll.deleteOne({_id: 4}));

// Pipelines with covered queries.

// We can collapse a covered query into a single $cursor when $project and $sort are present and
// the latter is near the front of the pipeline. Skip this test in sharded modes as we cannot
// correctly handle explain output in plan analyzer helper functions.
assert.commandWorked(coll.createIndex({x: 1}));
assertPipelineDoesNotUseAggregation({
    pipeline: [{$sort: {x: 1}}, {$project: {x: 1, _id: 0}}],
    expectedStages: ["IXSCAN"],
    expectedResult: [{x: 10}, {x: 20}, {x: 30}],
    preserveResultOrder: true
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$sort: {x: 1}}, {$project: {x: 1, _id: 0}}],
    expectedStages: ["IXSCAN"],
    expectedResult: [{x: 20}, {x: 30}],
    preserveResultOrder: true
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$sort: {x: 1}}, {$limit: 1}, {$project: {x: 1, _id: 0}}],
    expectedStages: ["IXSCAN"],
    expectedResult: [{x: 20}]
});
// However, when the $project is computed, pushing it down into the find() layer would sometimes
// have the effect of reordering it before the $sort and $limit. This can cause a valid query to
// throw an error, as in SERVER-54128.
assertPipelineUsesAggregation({
    pipeline: [
        {$match: {x: {$gte: 20}}},
        {$sort: {x: 1}},
        {$limit: 1},
        {$project: {x: {$substr: ["$y", 0, 1]}, _id: 0}}
    ],
    expectedStages: ["IXSCAN"],
    expectedResult: [{x: ""}]
});
assert.commandWorked(coll.dropIndexes());

assert.commandWorked(coll.insert({_id: 4, x: 40, a: {b: "ab1"}}));
assertPipelineDoesNotUseAggregation({
    pipeline: [{$project: {x: 1, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
    expectedResult: [{x: 10}, {x: 20}, {x: 30}, {x: 40}]
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: 20}}, {$project: {x: 1, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
    expectedResult: [{x: 20}]
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$project: {x: 1, "a.b": 1, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT"],
    expectedResult: [{x: 10}, {x: 20}, {x: 30}, {x: 40, a: {b: "ab1"}}]
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: 40}}, {$project: {"a.b": 1, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT"],
    expectedResult: [{a: {b: "ab1"}}]
});
// We can collapse a $project stage if it has a complex pipeline expression.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$project: {x: {$substr: ["$y", 0, 1]}, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT"]
});
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: 20}}, {$project: {x: {$substr: ["$y", 0, 1]}, _id: 0}}],
    expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT"]
});
assert.commandWorked(coll.deleteOne({_id: 4}));

assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$skip: 1}],
    expectedStages: ["COLLSCAN", "SKIP"],
    expectedResult: [{_id: 3, x: 30}]
});

// Pipelines which cannot be optimized away.

// We cannot optimize away a pipeline if there are stages which have no equivalent in the
// find command.
assertPipelineUsesAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$count: "count"}],
    expectedStages: ["COLLSCAN"],
    expectedResult: [{count: 2}]
});

assertPipelineIfGroupPushdown(
    function() {
        return assertPipelineDoesNotUseAggregation({
            pipeline: [{$match: {x: {$gte: 20}}}, {$group: {_id: "null", s: {$sum: "$x"}}}],
            expectedStages: ["COLLSCAN", "GROUP"],
            expectedResult: [{_id: "null", s: 50}],
        });
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: [{$match: {x: {$gte: 20}}}, {$group: {_id: "null", s: {$sum: "$x"}}}],
            expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
            expectedResult: [{_id: "null", s: 50}],
        });
    });

// Test that we can optimize away a pipeline with a $text search predicate.
assert.commandWorked(coll.createIndex({y: "text"}));
assertPipelineDoesNotUseAggregation(
    {pipeline: [{$match: {$text: {$search: "abc"}}}], expectedStages: ["IXSCAN"]});
// Test that $match, $sort, and $project all get answered by the PlanStage layer for a $text query.
assertPipelineDoesNotUseAggregation({
    pipeline:
        [{$match: {$text: {$search: "abc"}}}, {$sort: {sortField: 1}}, {$project: {a: 1, b: 1}}],
    expectedStages: ["TEXT_MATCH", "SORT", "PROJECTION_SIMPLE"],
    optimizedAwayStages: ["$match", "$sort", "$project"]
});
assert.commandWorked(coll.dropIndexes());

// We cannot optimize away geo near queries.
assert.commandWorked(coll.createIndex({"y": "2d"}));
assertPipelineUsesAggregation({
    pipeline: [{$geoNear: {near: [0, 0], distanceField: "y", spherical: true}}],
    expectedStages: ["GEO_NEAR_2D"],
});
assert.commandWorked(coll.dropIndexes());

// Test cases around pushdown of $limit.
assert.commandWorked(coll.createIndex({x: 1}));

// A lone $limit pipeline can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$limit: 1}],
    expectedStages: ["COLLSCAN", "LIMIT"],
});

// $match followed by $limit can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: 20}}, {$limit: 1}],
    expectedStages: ["IXSCAN", "LIMIT"],
    expectedResult: [{_id: 2, x: 20}],
});

// $limit followed by $match cannot be fully optimized away. The $limit is pushed down, but the
// $match is executed in the agg layer.
assertPipelineUsesAggregation({
    pipeline: [{$limit: 1}, {$match: {x: 20}}],
    expectedStages: ["COLLSCAN", "LIMIT"],
    optimizedAwayStages: ["$limit"],
});

// $match, $project, $limit can be optimized away when the projection is covered.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$project: {_id: 0, x: 1}}, {$limit: 1}],
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
    expectedResult: [{x: 20}],
});

// $match, $project, and $limit can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$project: {_id: 0, x: 1, y: 1}}, {$limit: 1}],
    expectedStages: ["IXSCAN", "FETCH", "LIMIT", "PROJECTION_SIMPLE"],
    expectedResult: [{x: 20}],
    optimizedAwayStages: ["$limit", "$project"],
});

// $match, $project, $limit, $sort cannot be optimized away because the $limit comes before the
// $sort.
assertPipelineUsesAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$project: {_id: 0, x: 1}}, {$limit: 1}, {$sort: {x: 1}}],
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
    expectedResult: [{x: 20}],
    optimizedAwayStages: ["$project", "$limit"],
});

// $match, $sort, $limit can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 20}}}, {$sort: {x: -1}}, {$limit: 2}],
    expectedStages: ["IXSCAN", "LIMIT"],
    expectedResult: [{_id: 3, x: 30}, {_id: 2, x: 20}],
});

// $match, $sort, $limit, $project can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline:
        [{$match: {x: {$gte: 20}}}, {$sort: {x: -1}}, {$limit: 2}, {$project: {_id: 0, x: 1}}],
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
    expectedResult: [{x: 30}, {x: 20}],
});

// $match, $sort, $project, $limit can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline:
        [{$match: {x: {$gte: 20}}}, {$sort: {x: -1}}, {$project: {_id: 0, x: 1}}, {$limit: 2}],
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
    expectedResult: [{x: 30}, {x: 20}],
});

// $match, $sort, $limit, $project can be optimized away, where limits must swap and combine to
// enable pushdown.
assertPipelineDoesNotUseAggregation({
    pipeline: [
        {$match: {x: {$gte: 20}}},
        {$sort: {x: -1}},
        {$limit: 3},
        {$project: {_id: 0, x: 1}},
        {$limit: 2}
    ],
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
    expectedResult: [{x: 30}, {x: 20}],
});

let pipeline = [{$sort: {x: 1}}, {$limit: 2}, {$group: {_id: null, s: {$sum: "$x"}}}];
assertPipelineIfGroupPushdown(
    function() {
        return assertPipelineDoesNotUseAggregation({
            pipeline: pipeline,
            expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT", "GROUP"],
            expectedResult: [{_id: null, s: 30}],
        });
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
            expectedResult: [{_id: null, s: 30}],
            optimizedAwayStages: ["$sort", "$limit"],
        });
    });

// Test that $limit can be pushed down before a group, but it prohibits the DISTINCT_SCAN
// optimization.
assertPipelineUsesAggregation({
    pipeline: [{$group: {_id: "$x"}}],
    expectedStages: ["DISTINCT_SCAN", "PROJECTION_COVERED"],
    expectedResult: [{_id: 10}, {_id: 20}, {_id: 30}],
});
assertPipelineUsesAggregation({
    pipeline: [{$sort: {x: 1}}, {$group: {_id: "$x"}}],
    expectedStages: ["DISTINCT_SCAN", "PROJECTION_COVERED"],
    expectedResult: [{_id: 10}, {_id: 20}, {_id: 30}],
    optimizedAwayStages: ["$sort"],
});

pipeline = [{$limit: 2}, {$group: {_id: "$x"}}];
assertPipelineIfGroupPushdown(
    function() {
        return assertPipelineDoesNotUseAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "LIMIT", "GROUP"],
        });
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "LIMIT"],
            optimizedAwayStages: ["$limit"],
        });
    });

pipeline = [{$sort: {x: 1}}, {$limit: 2}, {$group: {_id: "$x"}}];
assertPipelineIfGroupPushdown(
    function() {
        return assertPipelineDoesNotUseAggregation({
            pipeline: pipeline,
            expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT", "GROUP"],
        });
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
            optimizedAwayStages: ["$sort", "$limit"],
        });
    });

// $limit after a group has no effect on our ability to produce a DISTINCT_SCAN plan.
assertPipelineUsesAggregation({
    pipeline: [{$group: {_id: "$x"}}, {$sort: {_id: 1}}, {$limit: 2}],
    expectedStages: ["DISTINCT_SCAN", "PROJECTION_COVERED"],
    expectedResult: [{_id: 10}, {_id: 20}],
});

// For $limit, $project, $limit, we can optimize away both $limit stages.
pipeline = [{$match: {x: {$gte: 0}}}, {$limit: 2}, {$project: {_id: 0, x: 1}}, {$limit: 1}];
assertPipelineDoesNotUseAggregation({
    pipeline: pipeline,
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT"],
});
// Make sure that we end up using the smaller limit of 1.
let explain = coll.explain().aggregate(pipeline);
let limitStage = getAggPlanStage(explain, "LIMIT");
assert.neq(null, limitStage, explain);
assert.eq(1, limitStage.limitAmount, explain);

// We can optimize away interleaved $limit and $skip after a project.
pipeline = [
    {$match: {x: {$gte: 0}}},
    {$project: {_id: 0, x: 1}},
    {$skip: 20},
    {$limit: 15},
    {$skip: 10},
    {$limit: 7}
];
assertPipelineDoesNotUseAggregation({
    pipeline: pipeline,
    expectedStages: ["IXSCAN", "PROJECTION_COVERED", "LIMIT", "SKIP"],
    optimizedAwayStages: ["$match", "$limit", "$skip"],
});
explain = coll.explain().aggregate(pipeline);

let skipStage = getAggPlanStage(explain, "SKIP");
assert.neq(null, skipStage, explain);
assert.eq(30, skipStage.skipAmount, explain);

limitStage = getAggPlanStage(explain, "LIMIT");
assert.neq(null, limitStage, explain);
assert.eq(5, limitStage.limitAmount, explain);

assert.commandWorked(coll.dropIndexes());

// $sort can be optimized away even if there is no index to provide the sort.
assertPipelineDoesNotUseAggregation({
    pipeline: [
        {$sort: {x: -1}},
    ],
    expectedStages: ["COLLSCAN", "SORT"],
    expectedResult: [{_id: 3, x: 30}, {_id: 2, x: 20}, {_id: 1, x: 10}],
});

// $match, $sort, $limit can be optimized away even if there is no index to provide the sort.
assertPipelineDoesNotUseAggregation({
    pipeline: [{$match: {x: {$gte: 0}}}, {$sort: {x: -1}}, {$limit: 1}],
    expectedStages: ["COLLSCAN", "SORT"],
    expectedResult: [{_id: 3, x: 30}],
});

// $match, $sort, $project, $limit can be optimized away.
assertPipelineDoesNotUseAggregation({
    pipeline:
        [{$match: {x: {$gte: 20}}}, {$sort: {x: -1}}, {$project: {_id: 0, x: 1}}, {$limit: 2}],
    expectedStages: ["COLLSCAN", "SORT", "PROJECTION_SIMPLE"],
    expectedResult: [{x: 30}, {x: 20}],
});

// Test a case where there is a projection that can be covered by an index, but a blocking sort is
// still required. In this case, the entire pipeline can be optimized away.
assert.commandWorked(coll.createIndex({y: 1, x: 1}));
assertPipelineDoesNotUseAggregation({
    pipeline: [
        {$match: {y: {$gt: 0}, x: {$gte: 20}}},
        {$sort: {x: -1}},
        {$project: {_id: 0, y: 1, x: 1}},
        {$limit: 2}
    ],
    expectedStages: ["IXSCAN", "SORT", "PROJECTION_COVERED"],
    expectedResult: [],
});
assert.commandWorked(coll.dropIndexes());

// Test that even if we don't have a projection stage at the front of the pipeline but there is a
// finite dependency set, a projection representing this dependency set is pushed down.
pipeline = [{$group: {_id: "$a", b: {$sum: "$b"}}}];
assertPipelineIfGroupPushdown(
    function() {
        return assertPipelineDoesNotUseAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "GROUP"],
        });
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
        });
    });
pipeline = [{$group: {_id: "$a", b: {$sum: "$b"}}}, {$group: {_id: "$c", x: {$sum: "$b"}}}];
assertPipelineIfGroupPushdown(
    function() {
        const explain = coll.explain().aggregate(pipeline);
        // Both $group must be pushed down.
        assert.eq(getPlanStages(explain, "GROUP").length, 2);
        // PROJECTION_SIMPLE must be optimized away.
        assert.eq(getPlanStages(explain, "PROJECTION_SIMPLE").length, 0);
        // At bottom, there must be a COLLSCAN.
        assert.eq(getPlanStages(explain, "COLLSCAN").length, 1);
    },
    function() {
        return assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
        });
    });

function assertTransformByShape(expected, actual, message) {
    assert.eq(Object.keys(expected).sort(), Object.keys(actual).sort(), message);
    for (let key in expected) {
        assert.eq(expected[key], actual[key]);
    }
}

assertPipelineIfGroupPushdown(
    // When $group pushdown is enabled, $group will be lowered and the PROJECTION_SIMPLE will be
    // erased.
    function() {
        explain = coll.explain().aggregate(pipeline);
        let projStage = getAggPlanStage(explain, "PROJECTION_SIMPLE");
        assert.eq(null, projStage, explain);
    },
    // When $group pushdown is disabled, $group will not be lowered and the PROJECTION_SIMPLE will
    // be preserved.
    function() {
        explain = coll.explain().aggregate(pipeline);
        let projStage = getAggPlanStage(explain, "PROJECTION_SIMPLE");
        assert.neq(null, projStage, explain);
        assertTransformByShape({a: 1, b: 1, _id: 0}, projStage.transformBy, explain);
    });

// Similar as above, but with $addFields stage at the front of the pipeline.
pipeline = [{$addFields: {z: "abc"}}, {$group: {_id: "$a", b: {$sum: "$b"}}}];
assertPipelineUsesAggregation({
    pipeline: pipeline,
    expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE"],
});
explain = coll.explain().aggregate(pipeline);
let projStage = getAggPlanStage(explain, "PROJECTION_SIMPLE");
assert.neq(null, projStage, explain);
assertTransformByShape({a: 1, b: 1, _id: 0}, projStage.transformBy, explain);

// Asserts that, if group pushdown is enabled, we can remove a redundant projection stage before a
// group.
function assertProjectionCanBeRemovedBeforeGroup(pipeline, projectionType = "PROJECTION_SIMPLE") {
    assertPipelineIfGroupPushdown(
        // The projection and group should both be pushed down, and we expect to optimize away the
        // projection after realizing that it will not affect the output of the group.
        function() {
            let explain = assertPipelineDoesNotUseAggregation(
                {pipeline: pipeline, expectedStages: ["COLLSCAN", "GROUP"]});
            assert(!planHasStage(db, explain, projectionType), explain);
        },
        // If group pushdown is not enabled we still expect the projection to be pushed down.
        function() {
            assertPipelineUsesAggregation({
                pipeline: pipeline,
                expectedStages: ["COLLSCAN", projectionType, "$group"],
            });
        });
}

// Asserts that a projection stage is not optimized out of a pipeline with a projection and a group
// stage.
function assertProjectionIsNotRemoved(pipeline, projectionType = "PROJECTION_SIMPLE") {
    assertPipelineIfGroupPushdown(
        // The projection and group should both be pushed down, and we expect NOT to optimize away
        // the projection.
        function() {
            assertPipelineDoesNotUseAggregation(
                {pipeline: pipeline, expectedStages: ["COLLSCAN", projectionType, "GROUP"]});
        },
        // If group pushdown is not enabled we still expect the projection to be pushed down.
        function() {
            assertPipelineUsesAggregation({
                pipeline: pipeline,
                expectedStages: ["COLLSCAN", projectionType, "$group"],
            });
        });
}

// Test that an inclusion projection is optimized away if it is redundant/unnecessary.
assertProjectionCanBeRemovedBeforeGroup(
    [{$project: {a: 1, b: 1}}, {$group: {_id: "$a", s: {$sum: "$b"}}}]);

assertProjectionCanBeRemovedBeforeGroup(
    [{$project: {'a.b': 1, 'b.c': 1}}, {$group: {_id: "$a.b", s: {$sum: "$b.c"}}}],
    "PROJECTION_DEFAULT");

// Test that an inclusion projection is NOT optimized away if it is NOT redundant. This one fails to
// include a dependency of the $group and so will have an impact on the query results.
assertProjectionIsNotRemoved([{$project: {a: 1}}, {$group: {_id: "$a", s: {$sum: "$b"}}}]);
// Test similar cases with dotted paths.
assertProjectionIsNotRemoved([{$project: {'a.b': 1}}, {$group: {_id: "$a.b", s: {$sum: "$b"}}}],
                             "PROJECTION_DEFAULT");
assertProjectionIsNotRemoved([{$project: {'a.b': 1}}, {$group: {_id: "$a.b", s: {$sum: "$a.c"}}}],
                             "PROJECTION_DEFAULT");

// TODO SERVER-66061 This one could be removed, but is left for future work.
assertProjectionIsNotRemoved(
    [{$project: {a: 1, b: 1}}, {$group: {_id: "$a.b", s: {$sum: "$b.c"}}}]);

// Spinoff on the one above: Without supporting this kind of prefixing analysis, we can confuse
// ourselves with our dependency analysis. If the $group depends on both "path" and "path.subpath"
// then it will generate a $project on only "path" to express its dependency set. We then fail to
// optimize that out.
pipeline = [{$group: {_id: "$a.b", s: {$first: "$a"}}}];
// TODO SERVER-XYZ Assert this can be optimized out.
// assertProjectionCanBeRemovedBeforeGroup(pipeline, "PROJECTION_DEFAULT");
// assertProjectionCanBeRemovedBeforeGroup(pipeline, "PROJECTION_SIMPLE");
assertProjectionIsNotRemoved(pipeline);

// We generate a projection stage from dependency analysis, even if the pipeline begins with an
// exclusion projection.
pipeline = [{$project: {c: 0}}, {$group: {_id: "$a", b: {$sum: "$b"}}}];
assertPipelineUsesAggregation({
    pipeline: pipeline,
    expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE", "$project"],
});
explain = coll.explain().aggregate(pipeline);
projStage = getAggPlanStage(explain, "PROJECTION_SIMPLE");
assert.neq(null, projStage, explain);
assertTransformByShape({a: 1, b: 1, _id: 0}, projStage.transformBy, explain);

// Similar as above, but with a field 'a' presented both in the finite dependency set, and in the
// exclusion projection at the front of the pipeline.
pipeline = [{$project: {a: 0}}, {$group: {_id: "$a", b: {$sum: "$b"}}}];
assertPipelineUsesAggregation({
    pipeline: pipeline,
    expectedStages: ["COLLSCAN", "PROJECTION_SIMPLE", "$project"],
});
explain = coll.explain().aggregate(pipeline);
projStage = getAggPlanStage(explain, "PROJECTION_SIMPLE");
assert.neq(null, projStage, explain);
assertTransformByShape({a: 1, b: 1, _id: 0}, projStage.transformBy, explain);

// Test that an exclusion projection at the front of the pipeline is not pushed down, if there no
// finite dependency set.
pipeline = [{$project: {x: 0}}];
assertPipelineUsesAggregation({pipeline: pipeline, expectedStages: ["COLLSCAN"]});
explain = coll.explain().aggregate(pipeline);
assert(!planHasStage(db, explain, "PROJECTION_SIMPLE"), explain);
assert(!planHasStage(db, explain, "PROJECTION_DEFAULT"), explain);

// Test that a computed projection at the front of the pipeline is pushed down, even if there's no
// finite dependency set.
pipeline = [{$project: {x: {$add: ["$a", 1]}}}];
assertPipelineDoesNotUseAggregation(
    {pipeline: pipeline, expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT"]});

pipeline = [{$project: {a: {$add: ["$a", 1]}}}, {$group: {_id: "$a", s: {$sum: "$b"}}}];
assertPipelineIfGroupPushdown(
    // Test that a computed projection at the front of the pipeline is pushed down when there's a
    // finite dependency set. Additionally, the group pushdown shouldn't erase the computed
    // projection.
    function() {
        explain = coll.explain().aggregate(pipeline);
        assertPipelineDoesNotUseAggregation(
            {pipeline: pipeline, expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT", "GROUP"]});
    },
    // Test that a computed projection at the front of the pipeline is pushed down when there's a
    // finite dependency set.
    function() {
        explain = coll.explain().aggregate(pipeline);
        assertPipelineUsesAggregation({
            pipeline: pipeline,
            expectedStages: ["COLLSCAN", "PROJECTION_DEFAULT", "$group"],
        });
    });

// getMore cases.

// Test getMore on a collection with an optimized away pipeline.
testGetMore({
    command: {aggregate: coll.getName(), pipeline: [], cursor: {batchSize: 1}},
    expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
});
testGetMore({
    command:
        {aggregate: coll.getName(), pipeline: [{$match: {x: {$gte: 20}}}], cursor: {batchSize: 1}},
    expectedResult: [{_id: 2, x: 20}, {_id: 3, x: 30}]
});
testGetMore({
    command: {
        aggregate: coll.getName(),
        pipeline: [{$match: {x: {$gte: 20}}}, {$project: {x: 1, _id: 0}}],
        cursor: {batchSize: 1}
    },
    expectedResult: [{x: 20}, {x: 30}]
});
// Test getMore on a view with an optimized away pipeline. Since views cannot be created when
// imlicit sharded collection mode is on, this test will be run only on a non-sharded
// collection.
let view;
if (!FixtureHelpers.isSharded(coll)) {
    view = db.optimize_away_pipeline_view;
    view.drop();
    assert.commandWorked(db.createView(view.getName(), coll.getName(), []));
    testGetMore({
        command: {find: view.getName(), filter: {}, batchSize: 1},
        expectedResult: [{_id: 1, x: 10}, {_id: 2, x: 20}, {_id: 3, x: 30}]
    });
}
// Test getMore puts a correct namespace into profile data for a colletion with optimized away
// pipeline. Cannot be run on mongos as profiling can be enabled only on mongod. Also profiling
// is supported on WiredTiger only.
if (!FixtureHelpers.isMongos(db) && isWiredTiger(db)) {
    db.system.profile.drop();
    db.setProfilingLevel(2);
    testGetMore({
        command: {
            aggregate: coll.getName(),
            pipeline: [{$match: {x: 10}}],
            cursor: {batchSize: 1},
            comment: 'optimize_away_pipeline'
        },
        expectedResult: [{_id: 1, x: 10}]
    });
    db.setProfilingLevel(0);
    let profile = db.system.profile.find({}, {op: 1, ns: 1}).sort({ts: 1}).toArray();
    assert.sameMembers(
        profile,
        [{op: "command", ns: coll.getFullName()}, {op: "getmore", ns: coll.getFullName()}]);
    // Test getMore puts a correct namespace into profile data for a view with an optimized away
    // pipeline.
    if (!FixtureHelpers.isSharded(coll)) {
        db.system.profile.drop();
        db.setProfilingLevel(2);
        testGetMore({
            command: {
                find: view.getName(),
                filter: {x: 10},
                batchSize: 1,
                comment: 'optimize_away_pipeline'
            },
            expectedResult: [{_id: 1, x: 10}]
        });
        db.setProfilingLevel(0);
        profile = db.system.profile.find({}, {op: 1, ns: 1}).sort({ts: 1}).toArray();
        assert.sameMembers(
            profile,
            [{op: "query", ns: view.getFullName()}, {op: "getmore", ns: view.getFullName()}]);
    }
}
}());
