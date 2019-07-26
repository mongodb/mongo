/**
 * $group stages with no accumulators or with only $first accumulators can sometimes be converted
 * into a DISTINCT_SCAN (see SERVER-9507). This optimization potentially applies to a $group when it
 * begins the pipeline or when it is preceded only by one or both of $match and $sort (in that
 * order). In all cases, it must be possible to do a DISTINCT_SCAN that sees each value of the
 * distinct field exactly once among matching documents and also provides any requested sort. The
 * test queries below show most $match/$sort/$group combinations where that is possible.
 *
 * The sharding and $facet passthrough suites modifiy aggregation pipelines in a way that prevents
 * the DISTINCT_SCAN optimization from being applied, which breaks the test.
 * @tags: [assumes_unsharded_collection, do_not_wrap_aggregations_in_facets]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");

let coll = db.group_conversion_to_distinct_scan;
coll.drop();

// Add test data and indexes. Fields prefixed with "mk" are multikey.
let indexList = [
    {pattern: {a: 1, b: 1, c: 1}, option: {}},
    {pattern: {mkA: 1, b: 1, c: 1}, option: {}},
    {pattern: {aa: 1, mkB: 1, c: 1}, option: {}},
    {pattern: {aa: 1, bb: 1, c: 1}, option: {}},
    {pattern: {"foo.a": 1, "foo.b": 1}, option: {}},
    {pattern: {"mkFoo.a": 1, "mkFoo.b": 1}, option: {}},
    {pattern: {"foo.a": 1, "mkFoo.b": 1}, option: {}}
];

function createIndexes() {
    for (let indexSpec of indexList) {
        assert.commandWorked(coll.createIndex(indexSpec.pattern, indexSpec.option));
    }
}
createIndexes();

assert.commandWorked(coll.insert([
    {_id: 0, a: 1, b: 1, c: 1},
    {_id: 1, a: 1, b: 2, c: 2},
    {_id: 2, a: 1, b: 2, c: 3},
    {_id: 3, a: 1, b: 3, c: 2},
    {_id: 4, a: 2, b: 2, c: 2},
    {_id: 5, b: 1, c: 1},
    {_id: 6, a: null, b: 1, c: 1},

    {_id: 7, aa: 1, mkB: 2, bb: 2},
    {_id: 8, aa: 1, mkB: [1, 3], bb: 1},
    {_id: 9, aa: 2, mkB: [], bb: 3},

    {_id: 10, mkA: 1, c: 3},
    {_id: 11, mkA: [2, 3, 4], c: 3},
    {_id: 12, mkA: 2, c: 2},
    {_id: 13, mkA: 3, c: 4},

    {_id: 14, foo: {a: 1, b: 1}, mkFoo: {a: 1, b: 1}},
    {_id: 15, foo: {a: 1, b: 2}, mkFoo: {a: 1, b: 2}},
    {_id: 16, foo: {a: 2, b: 2}, mkFoo: {a: 2, b: 2}},
    {_id: 17, foo: {b: 1}, mkFoo: {b: 1}},
    {_id: 18, foo: {a: null, b: 1}, mkFoo: {a: null, b: 1}},
    {_id: 19, foo: {a: 3}, mkFoo: [{a: 3, b: 4}, {a: 4, b: 3}]},

    {_id: 20, str: "foo", d: 1},
    {_id: 21, str: "FoO", d: 2},
    {_id: 22, str: "bar", d: 4},
    {_id: 23, str: "bAr", d: 3}
]));

// Helper for dropping an index and removing it from the list of indexes.
function removeIndex(pattern) {
    assert.commandWorked(coll.dropIndex(pattern));
    indexList = indexList.filter((ix) => bsonWoCompare(ix.pattern, pattern) != 0);
}

function addIndex(pattern, option) {
    indexList.push({pattern: pattern, option: option});
    assert.commandWorked(coll.createIndex(pattern, option));
}

// Check that 'pipeline' returns the correct results with and without a hint added to the query.
// We also test with and without indices to check all the possibilities. 'options' is the
// options to pass to aggregate() and may be omitted. Similarly, the hint object can be omitted
// and will default to a $natural hint.
function assertResultsMatchWithAndWithoutHintandIndexes(pipeline,
                                                        expectedResults,
                                                        hintObj = {
                                                            $natural: 1
                                                        },
                                                        options = {}) {
    assert.commandWorked(coll.dropIndexes());
    const resultsNoIndex = coll.aggregate(pipeline, options).toArray();

    createIndexes();
    const resultsWithIndex = coll.aggregate(pipeline, options).toArray();

    const passedOptions = Object.assign({}, {hint: hintObj}, options);
    const resultsWithHint = coll.aggregate(pipeline, passedOptions).toArray();

    assert.sameMembers(resultsNoIndex, resultsWithIndex);
    assert.sameMembers(resultsWithIndex, resultsWithHint);
    assert.sameMembers(resultsWithHint, expectedResults);
}

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index.
//
let pipeline = [{$sort: {a: 1}}, {$group: {_id: "$a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: null}, {_id: 1}, {_id: 2}]);
let explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

// Pipelines that use the DISTINCT_SCAN optimization should not also have a blocking sort.
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $group pipeline can use DISTINCT_SCAN even when the user does not specify a
// sort.
//
pipeline = [{$group: {_id: "$a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: null}, {_id: 1}, {_id: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $group pipeline with a $natural hint does not use DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
explain = coll.explain().aggregate(pipeline, {hint: {$natural: 1}});
assert.neq(null, getAggPlanStage(explain, "COLLSCAN"), explain);

//
// Verify that a $group pipeline with a pertinent hint as string does use DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
explain = coll.explain().aggregate(pipeline, {hint: "a_1_b_1_c_1"});
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

//
// Verify that a $group pipeline with a pertinent hint as an object does use DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
explain = coll.explain().aggregate(pipeline, {hint: {a: 1, b: 1, c: 1}});
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

//
// Verify that a $group pipeline with a non-pertinent hint does not use DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: 1}, {_id: 2}], {_id: 1});
explain = coll.explain().aggregate(pipeline, {hint: {_id: 1}});
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), explain);
assert.eq({_id: 1}, getAggPlanStage(explain, "IXSCAN").keyPattern);

//
// Verify that a $group pipeline with an index filter still uses DISTINCT_SCAN.
//
assert.commandWorked(db.runCommand({
    planCacheSetFilter: coll.getName(),
    query: {},
    projection: {a: 1, _id: 0},
    indexes: ["a_1_b_1_c_1"]
}));

pipeline = [{$group: {_id: "$a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: null}, {_id: 1}, {_id: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(true, explain.stages[0].$cursor.queryPlanner.indexFilterSet);

//
// Verify that a $group pipeline with an index filter and $natural hint uses DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
explain = coll.explain().aggregate(pipeline, {hint: {$natural: 1}});
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(true, explain.stages[0].$cursor.queryPlanner.indexFilterSet);

//
// Verify that a $group pipeline with an index filter and non-pertinent hint uses DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: 1}, {_id: 2}], {_id: 1});
explain = coll.explain().aggregate(pipeline, {hint: {_id: 1}});
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(true, explain.stages[0].$cursor.queryPlanner.indexFilterSet);

assert.commandWorked(db.runCommand({planCacheClearFilters: coll.getName()}));

//
// Verify that a $sort-$group pipeline _does not_ use a DISTINCT_SCAN on a multikey field.
//
pipeline = [{$sort: {mkA: 1}}, {$group: {_id: "$mkA"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: [2, 3, 4]}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN when the sort is available from an
// index and there are $first accumulators.
//
pipeline = [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN when a $first accumulator needs the
// entire document.
//
pipeline = [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$first: "$$ROOT"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null, accum: {_id: 6, a: null, b: 1, c: 1}},
    {_id: 1, accum: {_id: 3, a: 1, b: 3, c: 2}},
    {_id: 2, accum: {_id: 4, a: 2, b: 2, c: 2}}
]);
explain = coll.explain().aggregate(pipeline);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern, explain);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN when sorting and grouping by fields
// with dotted paths.
//
pipeline =
    [{$sort: {"foo.a": 1, "foo.b": 1}}, {$group: {_id: "$foo.a", accum: {$first: "$foo.b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline,
    [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}, {_id: 3, accum: null}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({"foo.a": 1, "foo.b": 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $group pipeline can use DISTINCT_SCAN to group on a dotted path field, even
// when the user does not specify a sort.
//
pipeline = [{$group: {_id: "$foo.a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline,
                                               [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that we _do not_ attempt to use a DISTINCT_SCAN on a multikey field.
//
pipeline = [{$group: {_id: "$mkA"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}, {_id: [2, 3, 4]}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that we may not use a DISTINCT_SCAN on a dotted field when the last component
// is not multikey, but an intermediate component is.
//
pipeline = [{$group: {_id: "$mkFoo.a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null},
    {_id: 1},
    {_id: 2},
    {_id: [3, 4]},
]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that we _do not_ attempt to use a DISTINCT_SCAN on a multikey dotted-path field when
// a sort is present.
//
pipeline = [
    {$sort: {"mkFoo.a": 1, "mkFoo.b": 1}},
    {$group: {_id: "$mkFoo.a", accum: {$first: "$mkFoo.b"}}}
];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null, accum: null},
    {_id: 1, accum: 1},
    {_id: 2, accum: 2},
    {_id: [3, 4], accum: [4, 3]}
]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that we _do not_ attempt a DISTINCT_SCAN to satisfy a sort on a multikey field, even
// when the field we are grouping by is not multikey.
//
pipeline = [{$sort: {aa: 1, mkB: 1}}, {$group: {_id: "$aa", accum: {$first: "$mkB"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: null}, {_id: 1, accum: [1, 3]}, {_id: 2, accum: []}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), tojson(explain));

//
// Verify that with dotted paths we _do not_ attempt a DISTINCT_SCAN to satisfy a sort on a
// multikey field, even when the field we are grouping by is not multikey.
//
pipeline =
    [{$sort: {"foo.a": 1, "mkFoo.b": 1}}, {$group: {_id: "$foo.a", accum: {$first: "$mkFoo.b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline,
    [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}, {_id: 3, accum: [4, 3]}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that we can use a DISTINCT_SCAN on a multikey index to sort and group on a dotted-path
// field, so long as the field we are sorting over is not multikey and comes before any multikey
// fields in the index key pattern.
//
// We drop the {"foo.a": 1, "foo.b": 1} to force this test to use the multikey
// {"foo.a": 1, "mkFoo.b"} index. The rest of the test doesn't use either of those indexes.
//
removeIndex({"foo.a": 1, "foo.b": 1});
pipeline = [{$sort: {"foo.a": 1}}, {$group: {_id: "$foo.a"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline,
                                               [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({"foo.a": 1, "mkFoo.b": 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN even when there is a $first
// accumulator that accesses a multikey field.
//
pipeline = [{$sort: {aa: 1, bb: 1}}, {$group: {_id: "$aa", accum: {$first: "$mkB"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: null}, {_id: 1, accum: [1, 3]}, {_id: 2, accum: []}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({aa: 1, bb: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline can use DISTINCT_SCAN even when there is a $first
// accumulator that includes an expression.
//
pipeline = [{$sort: {a: 1, b: 1}}, {$group: {_id: "$a", accum: {$first: {$add: ["$b", "$c"]}}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: null}, {_id: 1, accum: 2}, {_id: 2, accum: 4}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $match-$sort-$group pipeline can use a DISTINCT_SCAN to sort and group by a
// field that is not the first field in a compound index, so long as the previous fields are
// scanned with equality bounds (i.e., are point queries).
//
pipeline = [{$match: {a: 1}}, {$sort: {b: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Same as the previous case but with the sort order matching the index key pattern, so the
// query planner does not need to infer the availability of a sort on {b: 1} based on the
// equality bounds for the 'a field.
//
pipeline = [{$match: {a: 1}}, {$sort: {a: 1, b: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Same as the previous case but with no user-specified sort.
//
pipeline = [{$match: {a: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $match-$sort-$group pipeline _does not_ use a DISTINCT_SCAN to sort and group
// on the second field of an index when there is no equality match on the first field.
//
pipeline = [{$sort: {a: 1, b: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline,
                                               [{_id: null}, {_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that a $match-$sort-$limit-$group pipeline _does not_ coalesce the $sort-$limit and
// then consider the result eligible for the DISTINCT_SCAN optimization.
//
// In this example, the {$limit: 3} filters out the document {a: 1, b: 3, c: 2}, which means we
// don't see a {_id: 3} group. If we instead applied the {$limit: 3} after the $group stage, we
// would incorrectly list three groups. DISTINCT_SCAN won't work here, because we have to
// examine each document in order to determine which groups get filtered out by the $limit.
//
pipeline = [{$match: {a: 1}}, {$sort: {a: 1, b: 1}}, {$limit: 3}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}]);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that an additional $project stage does not lead to incorrect results (although it will
// preclude the use of the DISTINCT_SCAN optimization).
//
pipeline =
    [{$match: {a: 1}}, {$project: {a: 1, b: 1}}, {$sort: {a: 1, b: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}, {_id: 3}]);

//
// Verify that a $sort-$group can use a DISTINCT_SCAN even when the requested sort is the
// reverse of the index's sort.
//
pipeline = [{$sort: {a: -1, b: -1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: 1}, {_id: 1, accum: 3}, {_id: 2, accum: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline _does not_ use DISTINCT_SCAN when there are non-$first
// accumulators.
//
pipeline = [{$sort: {a: 1}}, {$group: {_id: "$a", accum: {$sum: "$b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: 2}, {_id: 1, accum: 8}, {_id: 2, accum: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

// An index scan is still possible, though.
assert.neq(null, getAggPlanStage(explain, "IXSCAN"), explain);
assert.eq({a: 1, b: 1, c: 1}, getAggPlanStage(explain, "IXSCAN").keyPattern);
assert.eq(null, getAggPlanStage(explain, "SORT"), explain);

//
// Verify that a $sort-$group pipeline _does not_ use DISTINCT_SCAN when documents are not
// sorted by the field used for grouping.
//
pipeline = [{$sort: {b: 1}}, {$group: {_id: "$a", accum: {$first: "$b"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null, accum: null}, {_id: 1, accum: 1}, {_id: 2, accum: 2}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

//
// Verify that a $match-$sort-$group pipeline _does not_ use a DISTINCT_SCAN when the match does
// not provide equality (point query) bounds for each field before the grouped-by field in the
// index.
//
pipeline = [{$match: {a: {$gt: 0}}}, {$sort: {b: 1}}, {$group: {_id: "$b"}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [{_id: 1}, {_id: 2}, {_id: 3}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);

////////////////////////////////////////////////////////////////////////////////////////////////
// We execute all the collation-related tests three times with three different configurations
// (no index, index without collation, index with collation).
//
// Collation tests 1: no index on string field.
////////////////////////////////////////////////////////////////////////////////////////////////

const collationOption = {
    collation: {locale: "en_US", strength: 2}
};

//
// Verify that a $group on an unindexed field uses a collection scan.
//
pipeline = [{$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "FoO"}, {_id: "bAr"}, {_id: "bar"}, {_id: "foo"}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a collated $group on an unindexed field uses a collection scan.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "bAr"}, {_id: "foo"}], {$natural: 1}, collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a $sort-$group pipeline uses a collection scan.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null, accum: null},
    {_id: "FoO", accum: 2},
    {_id: "bAr", accum: 3},
    {_id: "bar", accum: 4},
    {_id: "foo", accum: 1}
]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a collated $sort-$group pipeline with a $first accumulator uses a collection
// scan.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline,
    [{_id: null, accum: null}, {_id: "bAr", accum: 3}, {_id: "foo", accum: 1}],
    {$natural: 1},
    collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

////////////////////////////////////////////////////////////////////////////////////////////////
// Collation tests 2: index on string field with no collation.
////////////////////////////////////////////////////////////////////////////////////////////////

addIndex({str: 1, d: 1});

//
// Verify that a $group uses a DISTINCT_SCAN.
//
pipeline = [{$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "FoO"}, {_id: "bAr"}, {_id: "bar"}, {_id: "foo"}]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({str: 1, d: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

//
// Verify that a $sort-$group pipeline with a collation _does not_ scan the index, which is not
// aware of the collation.
//
// Note that, when using a case-insensitive collation, "bAr" and "bar" will get grouped
// together, and the decision as to which one will represent the group is arbitary. The
// tie-breaking {d: 1} component of the sort forces a specific decision for this aggregation,
// making this test more reliable.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "bAr"}, {_id: "foo"}], {$natural: 1}, collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a $sort-$group pipeline uses a DISTINCT_SCAN.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null, accum: null},
    {_id: "FoO", accum: 2},
    {_id: "bAr", accum: 3},
    {_id: "bar", accum: 4},
    {_id: "foo", accum: 1}
]);
explain = coll.explain().aggregate(pipeline);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({str: 1, d: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

//
// Verify that a $sort-$group that use a collation and includes a $first accumulators  _does
// not_ scan the index, which is not aware of the collation.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline,
    [{_id: null, accum: null}, {_id: "bAr", accum: 3}, {_id: "foo", accum: 1}],
    {$natural: 1},
    collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

////////////////////////////////////////////////////////////////////////////////////////////////
// Collation tests 3: index on string field with case-insensitive collation.
////////////////////////////////////////////////////////////////////////////////////////////////

removeIndex({str: 1, d: 1});
addIndex({str: 1, d: 1}, collationOption);

//
// Verify that a $group with no collation _does not_ scan the index, which does have a
// collation.
//
pipeline = [{$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "FoO"}, {_id: "bAr"}, {_id: "bar"}, {_id: "foo"}]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a $sort-$group with a collation uses a DISTINCT_SCAN on the index, which uses a
// matching collation.
//
// Note that, when using a case-insensitive collation, "bAr" and "bar" will get grouped
// together, and the decision as to which one will represent the group is arbitary. The
// tie-breaking {d: 1} component of the sort forces a specific decision for this aggregation,
// making this test more reliable.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str"}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline, [{_id: null}, {_id: "bAr"}, {_id: "foo"}], {$natural: 1}, collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({str: 1, d: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);

//
// Verify that a $sort-$group pipeline with no collation _does not_ scan the index, which does
// have a collation.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(pipeline, [
    {_id: null, accum: null},
    {_id: "FoO", accum: 2},
    {_id: "bAr", accum: 3},
    {_id: "bar", accum: 4},
    {_id: "foo", accum: 1}
]);
explain = coll.explain().aggregate(pipeline);
assert.eq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq(null, getAggPlanStage(explain, "IXSCAN"), explain);

//
// Verify that a $sort-$group pipeline that uses a collation and includes a $first accumulator
// uses a DISTINCT_SCAN, which uses a matching collation.
//
pipeline = [{$sort: {str: 1, d: 1}}, {$group: {_id: "$str", accum: {$first: "$d"}}}];
assertResultsMatchWithAndWithoutHintandIndexes(
    pipeline,
    [{_id: null, accum: null}, {_id: "bAr", accum: 3}, {_id: "foo", accum: 1}],
    {$natural: 1},
    collationOption);
explain = coll.explain().aggregate(pipeline, collationOption);
assert.neq(null, getAggPlanStage(explain, "DISTINCT_SCAN"), explain);
assert.eq({str: 1, d: 1}, getAggPlanStage(explain, "DISTINCT_SCAN").keyPattern);
}());
