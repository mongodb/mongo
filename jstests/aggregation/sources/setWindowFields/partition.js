/*
 * Test partitioning inside $setWindowFields.
 *
 * @tags: [
 *   # We assume the pipeline is not split into a shardsPart and mergerPart.
 *   assumes_unsharded_collection,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
(function() {
"use strict";

load('jstests/aggregation/extras/utils.js');  // For resultsEq.

const coll = db[jsTestName()];
coll.drop();
assert.commandWorked(coll.insert({int_field: 0, arr: [1, 2]}));

// Test for runtime error when 'partitionBy' expression evaluates to an array
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $setWindowFields: {
            partitionBy: "$arr",
            sortBy: {_id: 1},
            output: {a: {$sum: "$int_field", window: {documents: ["unbounded", "current"]}}}
        }
    }],
    cursor: {}
}),
                             ErrorCodes.TypeMismatch);

// Test that a constant expression for 'partitionBy' is equivalent to no partitioning.
const constantPartitionExprs = [null, "constant", {$add: [1, 2]}];
constantPartitionExprs.forEach(function(partitionExpr) {
    const result = coll.explain().aggregate([
        // prevent stages from being absorbed into the .find() layer
        {$_internalInhibitOptimization: {}},
        {$setWindowFields: {partitionBy: partitionExpr, output: {}}},
    ]);
    assert.commandWorked(result);
    assert(Array.isArray(result.stages), result);
    assert(result.stages[0].$cursor, result);
    assert(result.stages[1].$_internalInhibitOptimization, result);
    assert.eq({$_internalSetWindowFields: {output: {}}}, result.stages[2]);
});

coll.drop();
assert.commandWorked(coll.insertMany([
    {int_field: 0},
    {int_field: null, other_field: null},
    {other_field: 0},
    {int_field: null},
    {other_field: null},
    {int_field: null, other_field: null}
]));

// Test that missing and null field are in the same partition.
let res = coll.aggregate([
    {$setWindowFields: {partitionBy: "$int_field", output: {count: {$sum: 1}}}},
    {$project: {_id: 0}}
]);
assert(resultsEq(res.toArray(), [
    {int_field: null, other_field: null, count: 5},
    {other_field: 0, count: 5},
    {int_field: null, count: 5},
    {other_field: null, count: 5},
    {int_field: null, other_field: null, count: 5},
    {int_field: 0, count: 1}
]));

// Test that the compound key with the mix of missing and null field works correctly.
res = coll.aggregate([
    {
        $setWindowFields: {
            partitionBy: {int_field: "$int_field", other_field: "$other_field"},
            output: {count: {$sum: 1}}
        }
    },
    {$project: {_id: 0}}
]);
assert(resultsEq(res.toArray(), [
    {int_field: null, count: 1},
    {int_field: null, other_field: null, count: 2},
    {int_field: null, other_field: null, count: 2},
    {other_field: null, count: 1},
    {int_field: 0, count: 1},
    {other_field: 0, count: 1}
]));
})();
