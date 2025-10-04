// @tags: [
//   does_not_support_stepdowns,
//   requires_non_retryable_writes,
//   requires_getmore,
// ]

/**
 * Tests for sorting documents by fields that contain arrays.
 */
import {aggPlanHasStage, isQueryPlan, planHasStage} from "jstests/libs/query/analyze_plan.js";

let coll = db.jstests_array_sort;

/**
 * Runs a $match-$sort-$project query as both a find and then an aggregate. Asserts that the
 * result set, after being converted to an array, is equal to 'expected'. Also asserts that the
 * find plan either uses the SORT stage and the agg plan uses the "$sort" agg stage or does not use
 * either dependent on the value of expectBlockingSort.
 */
function testAggAndFindSort({filter, sort, project, hint, expected, expectBlockingSort}) {
    let cursor = coll.find(filter, project).sort(sort);
    assert.eq(cursor.toArray(), expected);
    if (hint) {
        // If there was a hint specified, make sure we get the same results with the hint.
        cursor = coll.find(filter, project).sort(sort).hint(hint);
        assert.eq(cursor.toArray(), expected);
    }
    let explain = coll.find(filter, project).sort(sort).explain();
    if (expectBlockingSort) {
        assert(planHasStage(db, explain, "SORT"));
    } else {
        assert(!planHasStage(db, explain, "SORT"));
    }

    let pipeline = [{$_internalInhibitOptimization: {}}, {$match: filter}, {$sort: sort}, {$project: project}];
    cursor = coll.aggregate(pipeline);
    assert.eq(cursor.toArray(), expected);
    explain = coll.explain().aggregate(pipeline);
    assert(aggPlanHasStage(explain, "$sort"));
}

coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [3, 0, 1]}));
assert.commandWorked(coll.insert({_id: 1, a: [8, 4, -1]}));

// Sanity check that a sort on "_id" is usually pushed down into the query layer, but that
// $_internalInhibitOptimization prevents this from happening. This makes sure that this test is
// actually exercising the agg blocking sort implementation.
let explain = coll.explain().aggregate([{$sort: {_id: 1}}]);
assert(!aggPlanHasStage(explain, "$sort"));
explain = coll.explain().aggregate([{$_internalInhibitOptimization: {}}, {$sort: {_id: 1}}]);
assert(aggPlanHasStage(explain, "$sort"));

// Ascending sort, without an index.
testAggAndFindSort({
    filter: {a: {$gte: 2}},
    sort: {a: 1},
    project: {_id: 1, a: 1},
    expected: [
        {_id: 1, a: [8, 4, -1]},
        {_id: 0, a: [3, 0, 1]},
    ],
    expectBlockingSort: true,
});

assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.insert({_id: 0, a: [3, 0, 1]}));
assert.commandWorked(coll.insert({_id: 1, a: [0, 4, -1]}));

// Descending sort, without an index.
testAggAndFindSort({
    filter: {a: {$gte: 2}},
    sort: {a: -1},
    project: {_id: 1, a: 1},
    expected: [
        {_id: 1, a: [0, 4, -1]},
        {_id: 0, a: [3, 0, 1]},
    ],
    expectBlockingSort: true,
});

assert.commandWorked(coll.remove({}));
assert.commandWorked(coll.createIndex({a: 1}));
assert.commandWorked(coll.insert({_id: 0, a: [3, 0, 1]}));
assert.commandWorked(coll.insert({_id: 1, a: [0, 4, -1]}));

// Descending sort, in the presence of an index.
testAggAndFindSort({
    filter: {a: {$gte: 2}},
    sort: {a: -1},
    project: {_id: 1, a: 1},
    expected: [
        {_id: 1, a: [0, 4, -1]},
        {_id: 0, a: [3, 0, 1]},
    ],
    expectBlockingSort: true,
});

// Descending sort, in the presence of an index with [minKey, maxKey] bounds has a non-blocking
// sort.
testAggAndFindSort({
    filter: {},
    sort: {a: -1},
    project: {_id: 1, a: 1},
    expected: [
        {_id: 1, a: [0, 4, -1]},
        {_id: 0, a: [3, 0, 1]},
    ],
    expectBlockingSort: false,
});

// Ascending sort, in the presence of an index with [minKey, maxKey] bounds has a non-blocking sort.
testAggAndFindSort({
    filter: {},
    sort: {a: 1},
    project: {_id: 1, a: 1},
    expected: [
        {_id: 1, a: [0, 4, -1]},
        {_id: 0, a: [3, 0, 1]},
    ],
    expectBlockingSort: false,
});

assert.commandWorked(coll.remove({}));
assert.commandWorked(
    coll.insert({
        _id: 0,
        x: [
            {y: [4, 0, 1], z: 7},
            {y: 0, z: 9},
        ],
    }),
);
assert.commandWorked(
    coll.insert({
        _id: 1,
        x: [
            {y: 1, z: 7},
            {y: 0, z: [8, 6]},
        ],
    }),
);

// Compound mixed ascending/descending sorts, without an index. Sort key for doc with _id: 0 is
// {'': 0, '': 9}. Sort key for doc with _id: 1 is {'': 0, '': 8}.
testAggAndFindSort({
    filter: {},
    sort: {"x.y": 1, "x.z": -1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: true,
});

// Sort key for doc with _id: 0 is {'': 4, '': 7}. Sort key for doc with _id: 1 is {'': 1, '':
// 7}.
testAggAndFindSort({
    filter: {},
    sort: {"x.y": -1, "x.z": 1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: true,
});

assert.commandWorked(coll.createIndex({"x.y": 1, "x.z": -1}));

// Compound mixed ascending/descending sorts, with an index. Ascending and Descending sorts in the
// presence of an index with all fields having [minKey, maxKey] bounds has a non-blocking sort.
testAggAndFindSort({
    filter: {},
    sort: {"x.y": 1, "x.z": -1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: false,
});
testAggAndFindSort({
    filter: {},
    sort: {"x.y": -1, "x.z": 1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: false,
});
// Since there are bounds on "x.y", and since "x.y" shares a prefix with "x.z", this index cannot
// provide the sort.
testAggAndFindSort({
    filter: {"x.y": 1},
    sort: {"x.z": -1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: true,
});

// Since there are bounds on "x.y" this index cannot provide the sort.
testAggAndFindSort({
    filter: {"x.y": 1},
    sort: {"x.y": -1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: true,
});

// Since there are bounds on "x.y" and "x.z", this index cannot provide the sort.
testAggAndFindSort({
    filter: {"x.y": 1, "x.z": 1},
    sort: {"x.y": -1, "x.z": -1},
    project: {_id: 1},
    expected: [],
    expectBlockingSort: true,
});

assert.commandWorked(coll.createIndex({"x.y": 1, "x": -1}));

// Since there are bounds on 'x.y', and since 'x.y' shares a prefix with 'x', the index cannot
// provide a sort on the multikey field 'x'.
testAggAndFindSort({
    filter: {"x.y": 1},
    sort: {"x": -1},
    project: {_id: 1},
    expected: [{_id: 0}, {_id: 1}],
    expectBlockingSort: true,
});

// Since multikey index,'d', has no shared prefixes with 'e', the index can provide a sort on the
// multikey field 'e'.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, d: 3, e: [1, 2, 3]}));
assert.commandWorked(coll.insert({_id: 1, d: [0, 4, -1], e: 4}));
assert.commandWorked(coll.createIndex({d: 1, e: 1}));
testAggAndFindSort({filter: {d: 1}, sort: {e: 1}, project: {_id: 1}, expected: [], expectBlockingSort: false});

// Test that a multikey index can provide a sort over a non-multikey field.
coll.drop();
assert.commandWorked(coll.createIndex({a: 1, "b.c": 1}));
assert.commandWorked(coll.insert({a: [1, 2, 3], b: {c: 9}}));
explain = coll.find({a: 2}).sort({"b.c": -1}).explain();
assert(planHasStage(db, explain, "IXSCAN"));
assert(!planHasStage(db, explain, "SORT"));

const pipeline = [{$match: {a: 2}}, {$sort: {"b.c": -1}}];
explain = coll.explain().aggregate(pipeline);
assert(isQueryPlan(explain));
assert(planHasStage(db, explain, "IXSCAN"));
assert(!planHasStage(db, explain, "SORT"));

// Test that we can correctly sort by an array field in agg when there are additional fields not
// involved in the sort pattern.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: 1, b: {c: 1}, d: [{e: {f: 1, g: [6, 5, 4]}}, {e: {g: [3, 2, 1]}}]}));
assert.commandWorked(coll.insert({_id: 1, a: 2, b: {c: 2}, d: [{e: {f: 2, g: [5, 4, 3]}}, {e: {g: [2, 1, 0]}}]}));

testAggAndFindSort({
    filter: {},
    sort: {"d.e.g": 1},
    project: {_id: 1},
    expected: [{_id: 1}, {_id: 0}],
    expectBlockingSort: true,
});

// Test a sort over the trailing field of a compound index, where the two fields of the index
// share a path prefix. This is designed as a regression test for SERVER-31858.
coll.drop();
assert.commandWorked(
    coll.insert({
        _id: 2,
        a: [
            {b: 1, c: 2},
            {b: 2, c: 3},
        ],
    }),
);
assert.commandWorked(
    coll.insert({
        _id: 0,
        a: [
            {b: 2, c: 0},
            {b: 1, c: 4},
        ],
    }),
);
assert.commandWorked(
    coll.insert({
        _id: 1,
        a: [
            {b: 1, c: 5},
            {b: 2, c: 1},
        ],
    }),
);
assert.commandWorked(coll.createIndex({"a.b": 1, "a.c": 1}));
testAggAndFindSort({
    filter: {"a.b": 1},
    project: {_id: 1},
    sort: {"a.c": 1},
    expected: [{_id: 0}, {_id: 1}, {_id: 2}],
    expectBlockingSort: true,
});

// Test that an indexed and unindexed sort return the same thing for a path "a.x" which
// traverses through an array.
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [{x: 2}]}));
assert.commandWorked(coll.insert({_id: 1, a: [{x: 1}]}));
assert.commandWorked(coll.insert({_id: 2, a: [{x: 3}]}));
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: true,
});
assert.commandWorked(coll.createIndex({"a.x": 1}));
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: false,
});
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    hint: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: false,
});

// Now repeat the test with multiple entries along the path "a.x".
coll.drop();
assert.commandWorked(coll.insert({_id: 0, a: [{x: 2}, {x: 3}]}));
assert.commandWorked(coll.insert({_id: 1, a: [{x: 1}, {x: 4}]}));
assert.commandWorked(coll.insert({_id: 2, a: [{x: 3}, {x: 4}]}));
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: true,
});
assert.commandWorked(coll.createIndex({"a.x": 1}));
// Sort with an index on "a.x".
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: false,
});
testAggAndFindSort({
    filter: {},
    project: {_id: 1},
    sort: {"a.x": 1},
    hint: {"a.x": 1},
    expected: [{_id: 1}, {_id: 0}, {_id: 2}],
    expectBlockingSort: false,
});
