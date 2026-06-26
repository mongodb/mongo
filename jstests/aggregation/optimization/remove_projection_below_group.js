/**
 * Tests the QSN optimizer rule that eliminates a $project node sitting directly below a $group
 * when the group does not read any fields from the projected document.  This covers two cases:
 *
 *  1. Any projection (including computed expressions) before a zero-dependency $group (e.g.
 *     {_id: null, total: {$sum: 1}}).  The group's output is entirely independent of the
 *     projection's output, so the projection is redundant and can be safely dropped.
 *
 *  2. A simple inclusion projection before a $group that only needs a subset of those fields
 *     (existing behavior, regression guard).
 *
 * In both cases the pipeline must produce the same results as if the projection had run.
 * Case 1 is a regression test for SERVER-129906.
 *
 * @tags: [
 *   do_not_wrap_aggregations_in_facets,
 *   requires_pipeline_optimization,
 * ]
 */
import {assertArrayEq} from "jstests/aggregation/extras/utils.js";
import {describe, it} from "jstests/libs/mochalite.js";

const coll = db[jsTestName()];
coll.drop();

// Insert documents with fields used in the pipelines below.
// k0 = a + b per document: 1, 3, 7, 13, 21
assert.commandWorked(
    coll.insertMany([
        {a: 0, b: 1},
        {a: 1, b: 2},
        {a: 3, b: 4},
        {a: 6, b: 7},
        {a: 10, b: 11},
    ]),
);

describe("remove_projection_below_group optimizer rule", function () {
    it("computed projection before zero-dependency group: correct document count", function () {
        // $count expands to $group({_id:null,total:{$sum:1}}) + $project({_id:0,total:1}).
        // The optimizer removes the intermediate computed projection because the $group
        // has no field dependencies; results must still be the full document count.
        assertArrayEq({
            actual: coll
                .aggregate([{$project: {k0: {$add: ["$a", "$b"]}}}, {$count: "total"}])
                .toArray(),
            expected: [{total: 5}],
        });
    });

    it("computed projection before group that reads the computed field: correct sum", function () {
        // The $group accumulates over 'k0' which is computed by the preceding $project.
        // The optimizer must NOT remove the projection here; doing so would make $k0
        // undefined and produce a sum of zero instead of the real total.
        // Expected sum: (0+1)+(1+2)+(3+4)+(6+7)+(10+11) = 1+3+7+13+21 = 45
        assertArrayEq({
            actual: coll
                .aggregate([
                    {$project: {k0: {$add: ["$a", "$b"]}}},
                    {$group: {_id: null, total: {$sum: "$k0"}}},
                ])
                .toArray(),
            expected: [{_id: null, total: 45}],
        });
    });

    it("simple inclusion projection before zero-dependency group: correct count", function () {
        // Regression guard for the pre-existing case: a simple inclusion projection is also
        // eliminated when the group has no field dependencies.
        assertArrayEq({
            actual: coll.aggregate([{$project: {a: 1, b: 1}}, {$count: "total"}]).toArray(),
            expected: [{total: 5}],
        });
    });

    it("simple inclusion projection before group that uses its fields: correct sum", function () {
        // A simple inclusion that passes field 'a' through to a group that sums 'a'.
        // The projection is also eligible for elimination by the existing isInclusionOnly path
        // (requiredFields ⊆ projection output), but results must be correct either way.
        assertArrayEq({
            actual: coll
                .aggregate([{$project: {a: 1}}, {$group: {_id: null, total: {$sum: "$a"}}}])
                .toArray(),
            expected: [{_id: null, total: 20}],
        });
    });
});
