/**
 * Tests that aggregation pipelines including a $match stage that is sensitive to the
 * presence of a field (eg. {$exists: ...} or {$type: 'null'}) are not covered.
 *
 * @tags: [
 *   # Relies on the leading $sort/$limit being pushed down to the query system,
 *   # so the pipelines cannot be wrapped in $facet stages.
 *   do_not_wrap_aggregations_in_facets,
 *   # Disabling pipeline optimizations prevents pushdown.
 *   requires_pipeline_optimization,
 *   requires_fcv_91,
 * ]
 */

import {aggPlanHasStage} from "jstests/libs/query/analyze_plan.js";

/**
 * Asserts that a presence-sensitive predicate on the stored field 'b' is NOT answered from a
 * covered projection (i.e. the plan fetches), and that the result matches what a full document scan
 * would produce. 'indexSpec' is the compound index (the first field is also the $sort field, and is
 * the shard key in the sharded passthrough suites); 'sortField' is that field's name.
 */
function runPresenceSensitivePredicateTests(indexSpec, sortField) {
    const testColl = db[jsTestName() + "_" + sortField];
    testColl.drop();

    // 'b' is present-with-null, missing, or present with a real value.
    assert.commandWorked(
        testColl.insertMany([
            {[sortField]: 0, b: null},
            {[sortField]: 1},
            {[sortField]: 2, b: "string"},
        ]),
    );
    assert.commandWorked(testColl.createIndex(indexSpec));

    for (const predicate of [{$exists: false}, {$exists: true}, {$type: "null"}]) {
        // $sort forces an index scan; $limit keeps the $match from being pushed down (as it comes
        // _before_ the $match).
        const pipeline = [
            {$sort: {[sortField]: 1}},
            {$limit: 20},
            {$match: {b: predicate}},
            {$count: "num"},
        ];

        const explained = testColl.explain("queryPlanner").aggregate(pipeline);
        assert(
            !aggPlanHasStage(explained, "PROJECTION_COVERED"),
            "expected no covered projection",
            {explained: explained},
        );

        const withIndex = testColl.aggregate(pipeline).toArray();
        const withoutIndex = testColl.aggregate(pipeline, {hint: {$natural: 1}}).toArray();
        assert.eq(withIndex, withoutIndex, `${tojson(predicate)} produced different results`, {
            withIndex: withIndex,
            withoutIndex: withoutIndex,
        });
    }
}

runPresenceSensitivePredicateTests({a: 1, b: 1}, "a");
// In the sharded passthrough suites collections are sharded on {_id: "hashed"}, so an index
// {_id: 1, b: 1} covers both the shard key _id as well as b, but nevertheless the find command
// pushed-down by the pipeline must not be covered if there are presence-sensitive predicates.
runPresenceSensitivePredicateTests({_id: 1, b: 1}, "_id");

// Presence-sensitive predicates inside $facet sub-pipelines must also prevent covering.
{
    const facetColl = db[jsTestName() + "_facet"];
    facetColl.drop();
    assert.commandWorked(facetColl.insertOne({}));

    const pipeline = [
        {$sort: {a: 1}},
        {$limit: 10},
        {$facet: {f: [{$match: {a: {$exists: true}}}, {$count: "cnt"}]}},
    ];

    const withoutIndex = facetColl.aggregate(pipeline, {hint: {$natural: 1}}).toArray();
    assert.eq([{f: []}], withoutIndex);

    assert.commandWorked(facetColl.createIndex({a: 1}));
    const withIndex = facetColl.aggregate(pipeline).toArray();
    assert.eq(withoutIndex, withIndex, "$facet-nested $exists produced different results", {
        withIndex: withIndex,
        withoutIndex: withoutIndex,
    });
}

// Tricky case: the inner fieldRef (`c` inside the $elemMatch)
// must not be confused with an unrelated root predicate on the same-named field
// (e.g. 'c: 5'), which would wrongly block a covered projection.
{
    const nestedColl = db[jsTestName() + "_nested"];
    nestedColl.drop();
    assert.commandWorked(
        nestedColl.insertMany([
            {a: 1, b: {c: 1}, c: 5},
            {a: 2, b: {}, c: 5},
        ]),
    );
    assert.commandWorked(nestedColl.createIndex({a: 1, b: 1, c: 1}));

    // The root 'c: 5' is a value predicate; the $elemMatch-nested {$exists: false} on 'c' (inside
    // 'b') is about array elements, not the stored field, so it must not prevent a covered plan.
    const pipeline = [
        {$sort: {a: 1}},
        {$limit: 10},
        {$match: {c: 5, b: {$not: {$elemMatch: {c: {$exists: false}}}}}},
        {$count: "num"},
    ];
    const withIndex = nestedColl.aggregate(pipeline).toArray();
    const withoutIndex = nestedColl.aggregate(pipeline, {hint: {$natural: 1}}).toArray();
    assert.eq(withIndex, withoutIndex, "nested $elemMatch predicate produced different results", {
        withIndex: withIndex,
        withoutIndex: withoutIndex,
    });

    // The nested $elemMatch predicate must not block covering: the plan should still be covered.
    const explained = nestedColl.explain("queryPlanner").aggregate(pipeline);
    if (!aggPlanHasStage(explained, "SHARDING_FILTER")) {
        assert(
            aggPlanHasStage(explained, "PROJECTION_COVERED"),
            "expected a covered projection despite the nested $elemMatch predicate",
            {explained: explained},
        );
    }
}
