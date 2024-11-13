/*
 * Test partitioning inside $setWindowFields parses as expected.
 *
 * @tags: [
 *   # We assume the pipeline is not split into a shardsPart and mergerPart.
 *   assumes_unsharded_collection,
 *   # We're testing the explain plan, not the query results, so the facet passthrough would fail.
 *   do_not_wrap_aggregations_in_facets,
 * ]
 */
import {getSingleNodeExplain} from "jstests/libs/query/analyze_plan.js";

const coll = db[jsTestName()];
const collName = coll.getName();
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

// The same test as above, but 'partitionBy' references a $let variable.
assert.commandFailedWithCode(coll.runCommand({
    aggregate: coll.getName(),
    pipeline: [{
        $lookup: {
            from: collName, 
            let: {v: "$arr"},
            pipeline: [{
                $fill: {
                    partitionBy: {$concatArrays: ["$$v", "$$v"]},
                    sortBy: {int_field: -1},
                    output: {str: {method: 'locf'}}
                }
            }],
            as: "o",
        }
    }],
    cursor: {}
}),
                             ErrorCodes.TypeMismatch);

// Test that a constant expression for 'partitionBy' is equivalent to no partitioning.
const constantPartitionExprs = [null, "constant", {$add: [1, 2]}];
constantPartitionExprs.forEach(function(partitionExpr) {
    const result = assert.commandWorked(coll.explain().aggregate([
        // prevent stages from being absorbed into the .find() layer
        {$_internalInhibitOptimization: {}},
        {$setWindowFields: {partitionBy: partitionExpr, output: {}}},
    ]));
    const explain = getSingleNodeExplain(result);
    assert(Array.isArray(explain.stages), explain);
    assert(explain.stages[0].$cursor, explain);
    assert(explain.stages[1].$_internalInhibitOptimization, explain);
    assert.eq({$_internalSetWindowFields: {output: {}}}, explain.stages[2], explain);
});