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
//   sbe_incompatible,
// ]
(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For 'getPlanStage'.

const t = db.jstests_aggregation_server6192;
t.drop();
assert.commandWorked(t.insert({x: true}));

function assertOptimized(pipeline, v) {
    const explained = t.runCommand("aggregate", {pipeline: pipeline, explain: true});
    const projectStage = getPlanStage(explained, "PROJECTION_DEFAULT");
    assert.eq(projectStage.transformBy.a["$const"], v, "ensure short-circuiting worked", explained);
}

function assertNotOptimized(pipeline) {
    const explained = t.runCommand("aggregate", {pipeline: pipeline, explain: true});
    const projectStage = getPlanStage(explained, "PROJECTION_DEFAULT");
    assert(!("$const" in projectStage.transformBy.a), "ensure no short-circuiting");
}

// short-circuiting for $and
assertOptimized([{$project: {a: {$and: [0, '$x']}}}], false);
assertOptimized([{$project: {a: {$and: [0, 1, '$x']}}}], false);
assertOptimized([{$project: {a: {$and: [0, 1, '', '$x']}}}], false);

assertOptimized([{$project: {a: {$and: [1, 0, '$x']}}}], false);
assertOptimized([{$project: {a: {$and: [1, '', 0, '$x']}}}], false);
assertOptimized([{$project: {a: {$and: [1, 1, 0, 1]}}}], false);

// short-circuiting for $or
assertOptimized([{$project: {a: {$or: [1, '$x']}}}], true);
assertOptimized([{$project: {a: {$or: [1, 0, '$x']}}}], true);
assertOptimized([{$project: {a: {$or: [1, '', '$x']}}}], true);

assertOptimized([{$project: {a: {$or: [0, 1, '$x']}}}], true);
assertOptimized([{$project: {a: {$or: ['', 0, 1, '$x']}}}], true);
assertOptimized([{$project: {a: {$or: [0, 0, 0, 1]}}}], true);

// examples that should not short-circuit
assertNotOptimized([{$project: {a: {$and: [1, '$x']}}}]);
assertNotOptimized([{$project: {a: {$or: [0, '$x']}}}]);
assertNotOptimized([{$project: {a: {$and: ['$x', '$x']}}}]);
assertNotOptimized([{$project: {a: {$or: ['$x', '$x']}}}]);
assertNotOptimized([{$project: {a: {$and: ['$x']}}}]);
assertNotOptimized([{$project: {a: {$or: ['$x']}}}]);
}());
