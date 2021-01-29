// test short-circuiting of $and and $or in
// $project stages to a $const boolean
//
// Cannot implicitly shard accessed collections because the explain output from a mongod when run
// against a sharded collection is wrapped in a "shards" object with keys for each shard.
//
// This test makes assumptions about how the explain output will be formatted, so cannot be
// transformed to be put inside a $facet stage or when pipeline optimization is disabled.
// @tags: [
//   assumes_unsharded_collection,
//   do_not_wrap_aggregations_in_facets,
//   requires_pipeline_optimization,
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'getPlanStage'.

const t = db.jstests_aggregation_server6192;
t.drop();
assert.commandWorked(t.insert({x: true}));

function optimize(expression) {
    const explained = t.explain().aggregate([
        // This inhibit optimization prevents the expression being pushed down into the .find()
        // layer, to make assertions simpler. But it doesn't prevent the $project stage itself
        // from being optimized.
        {$_internalInhibitOptimization: {}},
        {$project: {result: expression}},
    ]);

    const stage = getAggPlanStage(explained, "$project");
    assert(stage, explained);
    assert(stage.$project.result, explained);
    return stage.$project.result;
}

function assertOptimized(expression, value) {
    const optimized = optimize(expression);
    assert.docEq(optimized, {$const: value}, "ensure short-circuiting worked", optimized);
}

function assertNotOptimized(expression) {
    const optimized = optimize(expression);
    // 'optimized' may be simpler than 'expression', but we assert it did not optimize to a
    // constant.
    assert.neq(Object.keys(optimized), ['$const'], "ensure no short-circuiting", optimized);
}

// short-circuiting for $and
assertOptimized({$and: [0, '$x']}, false);
assertOptimized({$and: [0, 1, '$x']}, false);
assertOptimized({$and: [0, 1, '', '$x']}, false);

assertOptimized({$and: [1, 0, '$x']}, false);
assertOptimized({$and: [1, '', 0, '$x']}, false);
assertOptimized({$and: [1, 1, 0, 1]}, false);

// short-circuiting for $or
assertOptimized({$or: [1, '$x']}, true);
assertOptimized({$or: [1, 0, '$x']}, true);
assertOptimized({$or: [1, '', '$x']}, true);

assertOptimized({$or: [0, 1, '$x']}, true);
assertOptimized({$or: ['', 0, 1, '$x']}, true);
assertOptimized({$or: [0, 0, 0, 1]}, true);

// examples that should not short-circuit
assertNotOptimized({$and: [1, '$x']});
assertNotOptimized({$or: [0, '$x']});
assertNotOptimized({$and: ['$x', '$x']});
assertNotOptimized({$or: ['$x', '$x']});
assertNotOptimized({$and: ['$x']});
assertNotOptimized({$or: ['$x']});
}());
