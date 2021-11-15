/**
 * Test projections with $and in cases where optimizations could be performed.
 */
(function() {
"use strict";

load("jstests/aggregation/extras/utils.js");
load('jstests/libs/analyze_plan.js');

const coll = db.projection_and;
coll.drop();

assert.commandWorked(coll.insert({_id: 0, a: 1}));
assert.commandWorked(coll.createIndex({a: 1}));

function runFindWithProjection({filter = {}, projection, expected} = {}) {
    const res = coll.find(filter, projection);
    assertArrayEq({actual: res.toArray(), expected: expected});
    return res;
}

let result = runFindWithProjection({
    filter: {a: 1},
    projection: {_id: 0, a: 1, b: {$and: [false, "$b"]}},
    expected: [{a: 1, b: false}]
});
// Query should be optimized and covered.
assert(isIndexOnly(db, getWinningPlan(result.explain().queryPlanner)));

result = runFindWithProjection(
    {projection: {a: {$and: ['$a', true, 1]}}, expected: [{_id: 0, a: true}]});
assert(isCollscan(db, getWinningPlan(result.explain().queryPlanner)));
})();
