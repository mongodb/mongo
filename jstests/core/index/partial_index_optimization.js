/**
 * Tests for classic query optimization with partial indices: do not generate fetch stage in the
 * plan if a query predicate is satisfied by the filter expression of the chosen partial index. If
 * the fetch phase is needed for another reason, make sure that the predicate is not in the fetch
 * filter.
 *
 * @tags: [
 *    # the test conflicts with hidden wildcard indexes
 *    assumes_no_implicit_index_creation,
 *    does_not_support_stepdowns,
 *    cqf_incompatible,
 *    multiversion_incompatible,
 *    requires_fcv_70,
 * ]
 */

(function() {
"use strict";

load("jstests/libs/analyze_plan.js");  // For assertStagesForExplainOfCommand(),
                                       // assertNoFetchFilter(), assertFetchFilter().

function flagVal(n) {
    return (n % 5 > 3) ? true : false;
}

function stateVal(n) {
    const states = ["open", "closed", "unknown"];
    return states[n % 3];
}

function getDocs(len, start = 0) {
    return Array.from({length: len}, (_, i) => ({
                                         _id: start + i,
                                         a: i,
                                         b: i + 3,
                                         c: [i, i + 5],
                                         flag: flagVal(i),
                                         state: stateVal(i),
                                         array: [{a: i, state: stateVal(i)}, {b: i}]
                                     }));
}

const coll = db.partial_index_opt;
coll.drop();
assert.commandWorked(coll.insertMany(getDocs(100)));
assert.commandWorked(coll.insertMany([
    {
        _id: 100,
        a: 100,
        state: "open",
        array: [{a: 100, state: "closed"}, {a: 101, state: "closed"}]
    },
    {_id: 101, a: 101, state: "open", array: [{a: 101, state: "open"}]},
    {_id: 102, a: 102, state: "closed", array: [{a: 102, state: "open"}]}
]));

const expectedStagesCount = ["COUNT", "COUNT_SCAN"];

assert.commandWorked(coll.createIndex({a: 1}, {"partialFilterExpression": {flag: true}}));
let cmdObj = {find: coll.getName(), filter: {flag: true, a: 4}, projection: {_id: 0, a: 1}};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// The following plan has a fetch phase because of the projection, but no filter on it.
cmdObj = {
    find: coll.getName(),
    filter: {flag: true, a: 4},
    projection: {a: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Count command.
cmdObj = {
    count: coll.getName(),
    query: {flag: true, a: 4}
};
assertStagesForExplainOfCommand({
    coll: coll,
    cmdObj: cmdObj,
    expectedStages: expectedStagesCount,
    stagesNotExpected: ["FETCH"]
});

// Partial index with filter expression with conjunction.
assert.commandWorked(coll.createIndex(
    {a: 1}, {name: "a_1_range", "partialFilterExpression": {a: {$gte: 20, $lte: 40}}}));
cmdObj = {
    find: coll.getName(),
    filter: {a: {$gte: 20, $lte: 40}},
    projection: {_id: 0, a: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

cmdObj = {
    find: coll.getName(),
    filter: {a: {$gte: 25, $lte: 30}},
    projection: {_id: 0, a: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Partial index with compound key.
assert.commandWorked(coll.createIndex({a: 1, b: 1}, {"partialFilterExpression": {flag: true}}));
cmdObj = {
    find: coll.getName(),
    filter: {a: {$gte: 50}, b: {$in: [55, 57, 59, 62]}, flag: true},
    projection: {_id: 0, a: 1, b: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Filter expression with conjunction on multiple fields.
assert.commandWorked(coll.createIndex(
    {b: 1}, {name: "b_1_state_open", "partialFilterExpression": {state: "open", b: {$gt: 50}}}));

cmdObj = {
    find: coll.getName(),
    filter: {state: "open", b: {$gt: 80}},
    projection: {_id: 0, b: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

cmdObj = {
    count: coll.getName(),
    query: {state: "open", b: {$gt: 50}}
};
assertStagesForExplainOfCommand({
    coll: coll,
    cmdObj: cmdObj,
    expectedStages: expectedStagesCount,
    stagesNotExpected: ["FETCH"]
});

// Index filter expression with $exists.
assert.commandWorked(coll.createIndex(
    {a: 1}, {name: "a_1_b_exists", "partialFilterExpression": {b: {$exists: true}}}));

cmdObj = {
    find: coll.getName(),
    filter: {a: {$gte: 90}, b: {$exists: true}},
    projection: {_id: 0, a: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Filter expression in a multi-key index.
assert.commandWorked(
    coll.createIndex({c: 1}, {name: "c_1_a", partialFilterExpression: {a: {$lte: 30}}}));

cmdObj = {
    count: coll.getName(),
    query: {c: {$lte: 50}, a: {$lte: 30}}
};
assertStagesForExplainOfCommand({
    coll: coll,
    cmdObj: cmdObj,
    expectedStages: expectedStagesCount,
    stagesNotExpected: ["FETCH"]
});

// The following plan has a fetch phase, but no filter on 'a'.
cmdObj = {
    find: coll.getName(),
    filter: {c: {$lte: 50}, a: {$lte: 30}},
    projection: {_id: 0, c: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Test that the same filter expression under $elemMatch will not be removed from the fetch filter.
assert.commandWorked(
    coll.createIndex({a: 1}, {name: "a_1_state_open", "partialFilterExpression": {state: "open"}}));

let predicate = {
    a: {$gte: 100},
    state: "open",
    array: {$elemMatch: {$and: [{a: {$gte: 100}}, {state: "open"}]}}
};
let fetchFilter = {
    "array": {"$elemMatch": {"$and": [{"a": {"$gte": 100}}, {"state": {"$eq": "open"}}]}}
};
assertFetchFilter({coll: coll, predicate: predicate, expectedFilter: fetchFilter, nReturned: 1});

// Index on $elemMatch predicate. Test that the index filter predicate is removed from the fetch
// filter while $elemMatch predicate is preserved.
assert.commandWorked(coll.createIndex(
    {"array.a": 1}, {name: "array_a_1_state_open", "partialFilterExpression": {state: "open"}}));

predicate = {
    state: "open",
    array: {$elemMatch: {$and: [{a: {$gte: 100}}, {state: "open"}]}}
};
fetchFilter = {
    "array": {"$elemMatch": {"$and": [{"a": {"$gte": 100}}, {"state": {"$eq": "open"}}]}}
};
assertFetchFilter({coll: coll, predicate: predicate, expectedFilter: fetchFilter, nReturned: 1});

// Test for index filter expression over nested field.
assert.commandWorked(coll.createIndex(
    {"array.a": 1},
    {name: "array_a_1_array_state_open", "partialFilterExpression": {"array.state": "open"}}));

cmdObj = {
    find: coll.getName(),
    filter: {$and: [{"array.a": {$gte: 100}}, {"array.state": "open"}]},
    projection: {_id: 0, array: 1}
};
assertNoFetchFilter({coll: coll, cmdObj: cmdObj});

// Tests that the query predicate is not removed if it is a subset of an $or index filter.
assert.commandWorked(coll.createIndex(
    {a: 1}, {name: "a_1_or", "partialFilterExpression": {$or: [{b: {$gte: 80}}, {flag: "true"}]}}));

predicate = {
    $and: [{a: {$gte: 75}}, {b: {$gte: 80}}]
};
fetchFilter = {
    "b": {"$gte": 80}
};
assertFetchFilter({coll: coll, predicate: predicate, expectedFilter: fetchFilter, nReturned: 23});

// Possible optimization: the following query could use a bounded index scan on 'a' and remove the
// $or sub-predicate as it is covered by the partial index filter. Currently, the index is not
// considered and a collection scan is used instead.
const exp =
    coll.find({$and: [{a: {$gte: 90}}, {$or: [{b: {$gte: 80}}, {flag: "true"}]}]}).explain();
assert(isCollscan(db, exp),
       "Expected collection scan, got " + tojson(getWinningPlan(exp.queryPlanner)));
})();
