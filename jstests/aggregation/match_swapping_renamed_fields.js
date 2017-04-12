/**
 * Tests for the $match swapping optimization's ability to handle fields renamed in $project or
 * $addFields.
 * @tags: [do_not_wrap_aggregations_in_facets]
 */
(function() {
    "use strict";

    load("jstests/libs/analyze_plan.js");

    let coll = db.match_swapping_renamed_fields;
    coll.drop();

    assert.writeOK(coll.insert([{a: 1, b: 1, c: 1}, {a: 2, b: 2, c: 2}, {a: 3, b: 3, c: 3}]));
    assert.commandWorked(coll.createIndex({a: 1}));

    // Test that a $match can result in index usage after moving past a field renamed by $project.
    let pipeline = [{$project: {_id: 0, z: "$a", c: 1}}, {$match: {z: {$gt: 1}}}];
    assert.eq(2, coll.aggregate(pipeline).itcount());
    let explain = coll.explain().aggregate(pipeline);
    assert.neq(null, getAggPlanStage(explain, "IXSCAN"));

    // Test that a $match can result in index usage after moving past a field renamed by $addFields.
    pipeline = [{$addFields: {z: "$a"}}, {$match: {z: {$gt: 1}}}];
    assert.eq(2, coll.aggregate(pipeline).itcount());
    explain = coll.explain().aggregate(pipeline);
    assert.neq(null, getAggPlanStage(explain, "IXSCAN"));

    // Test that a partially dependent match can split, with a rename applied, resulting in index
    // usage.
    pipeline =
        [{$project: {z: "$a", zz: {$sum: ["$a", "$b"]}}}, {$match: {z: {$gt: 1}, zz: {$lt: 5}}}];
    assert.eq(1, coll.aggregate(pipeline).itcount());
    explain = coll.explain().aggregate(pipeline);
    assert.neq(null, getAggPlanStage(explain, "IXSCAN"));

    // Test that a match can swap past several renames, resulting in index usage.
    pipeline = [
        {$project: {d: "$a"}},
        {$addFields: {e: "$$CURRENT.d"}},
        {$project: {f: "$$ROOT.e"}},
        {$match: {f: {$gt: 1}}}
    ];
    assert.eq(2, coll.aggregate(pipeline).itcount());
    explain = coll.explain().aggregate(pipeline);
    assert.neq(null, getAggPlanStage(explain, "IXSCAN"));
}());
