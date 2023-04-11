// Test the plan cache entries for a $lookup with a single query solution. It should produce a
// single cache entry.
// @tags: [
//   assumes_unsharded_collection,
//   do_not_wrap_aggregations_in_facets,
//   assumes_read_concern_unchanged,
//   assumes_read_preference_unchanged,
//   tenant_migration_incompatible
// ]
(function() {
"use strict";

const outer = db.outer;
const inner = db.inner;

outer.drop();
inner.drop();

assert.commandWorked(outer.insert([{_id: 0, a: 0}, {_id: 1, a: 1}, {_id: 2, a: 2}]));
assert.commandWorked(inner.insert([{_id: 3, b: 0}, {_id: 4, b: 1}, {_id: 5, b: 2}]));

const expectedResults = [{a: 0, docs: [{b: 0}]}, {a: 1, docs: [{b: 1}]}, {a: 2, docs: [{b: 2}]}];

{
    // Sort by "a" so we know the cached plan will hold b=1 (the second run will be cached).
    const pipeline = [
        {$sort: {a: 1}},
        {$lookup: {from: "inner", localField: "a", foreignField: "b", as: "docs", pipeline: [{$project: {_id: 0}}]}},
        {$project: {_id: 0}}
    ];

    assert.eq(outer.aggregate(pipeline).toArray(), expectedResults);

    assert.eq(inner.getPlanCache().list().length, 1);
    const innerPlan = inner.getPlanCache().list()[0];
    assert(innerPlan.isActive);
    assert.eq(innerPlan.cachedPlan.inputStage.stage, "COLLSCAN");
    assert.eq(innerPlan.cachedPlan.inputStage.filter, {b: {$eq: 1}});
}

inner.getPlanCache().clear();

{
    // Test with "let" syntax.
    const pipeline = [
        {$sort: {a: 1}},
        {$lookup: {
            from: "inner",
            let: {aRenamed: "$a"},
            pipeline: [
                {$match: {$expr: {$eq: ["$b",  "$$aRenamed"]}}},
                {$project: {_id: 0}},
            ],
            as: "docs"
        }},
        {$project: {_id: 0}}
    ];

    assert.eq(outer.aggregate(pipeline).toArray(), expectedResults);

    assert.eq(inner.getPlanCache().list().length, 1);
    const innerPlan = inner.getPlanCache().list()[0];
    assert(innerPlan.isActive);
    assert.eq(innerPlan.cachedPlan.inputStage.stage, "COLLSCAN");
    assert.eq(Object.keys(innerPlan.cachedPlan.inputStage.filter["$and"][0]), ["$expr"]);
}
}());
